#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCEGLView.hpp>      // Hook swapBuffers for frame capture
#include <Geode/ui/Notification.hpp>

// Android NDK Media APIs — require API 21+ (fine for GD)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>

// OpenGL ES — glReadPixels
#include <GLES2/gl2.h>

#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <fcntl.h>
#include <unistd.h>

using namespace geode::prelude;

// ═══════════════════════════════════════════════════════════════════
//  RGBA → I420 (YUV420Planar)
//  Most Android H.264 encoders only accept YUV420Planar (I420), not RGBA.
//  OpenGL glReadPixels returns bottom-up rows, so we flip vertically here.
// ═══════════════════════════════════════════════════════════════════
static void rgbaToI420(
    const uint8_t* rgba, int width, int height,
    uint8_t* dst)   // layout: [Y plane][U plane][V plane]
{
    int ySize  = width * height;
    int uvSize = ySize / 4;

    uint8_t* yPlane = dst;
    uint8_t* uPlane = dst + ySize;
    uint8_t* vPlane = dst + ySize + uvSize;

    // Y plane — one luma sample per pixel, rows flipped
    for (int row = 0; row < height; ++row) {
        int srcRow = (height - 1) - row;
        const uint8_t* src  = rgba   + srcRow * width * 4;
        uint8_t*       yDst = yPlane + row    * width;
        for (int col = 0; col < width; ++col) {
            uint8_t r = src[col * 4 + 0];
            uint8_t g = src[col * 4 + 1];
            uint8_t b = src[col * 4 + 2];
            yDst[col] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    // U and V planes — one chroma sample per 2×2 block, rows flipped
    int uvHeight = height / 2;
    int uvWidth  = width  / 2;
    for (int row = 0; row < uvHeight; ++row) {
        int srcRow = (height - 1) - (row * 2);
        const uint8_t* src  = rgba   + srcRow * width * 4;
        uint8_t*       uDst = uPlane + row    * uvWidth;
        uint8_t*       vDst = vPlane + row    * uvWidth;
        for (int col = 0; col < uvWidth; ++col) {
            uint8_t r = src[col * 2 * 4 + 0];
            uint8_t g = src[col * 2 * 4 + 1];
            uint8_t b = src[col * 2 * 4 + 2];
            uDst[col] = static_cast<uint8_t>((( -38*r -  74*g + 112*b + 128) >> 8) + 128);
            vDst[col] = static_cast<uint8_t>(((  112*r -  94*g -  18*b + 128) >> 8) + 128);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  VideoRecorder
//  Uses AMediaCodec (H.264 encoder) + AMediaMuxer (MP4 container).
//  Frame capture runs on the GL thread; encoding runs on a background thread.
// ═══════════════════════════════════════════════════════════════════
class VideoRecorder {
public:
    static constexpr int TARGET_FPS = 30;
    static constexpr int BIT_RATE   = 4 * 1024 * 1024; // 4 Mbps

    bool isRecording() const { return m_isRecording.load(); }

    // ─── Start a new recording ────────────────────────────────────────────
    bool start(const std::string& path, int width, int height) {
        if (m_isRecording.load()) return false;

        m_width          = width;
        m_height         = height;
        m_path           = path;
        m_presentationUs = 0;
        m_capturedFrames = 0;
        m_recordStart    = std::chrono::steady_clock::now();

        // ── Create H.264 encoder ──────────────────────────────────────────
        m_codec = AMediaCodec_createEncoderByType("video/avc");
        if (!m_codec) {
            log::error("[MacroSafeguard] Failed to create AVC encoder");
            return false;
        }

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,             "video/avc");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_WIDTH,            width);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_HEIGHT,           height);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_FRAME_RATE,       TARGET_FPS);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,         BIT_RATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        // 0x13 = OMX_COLOR_FormatYUV420Planar — the safest choice on Android
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,     0x13);

        media_status_t st = AMediaCodec_configure(
            m_codec, fmt, nullptr, nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);

        if (st != AMEDIA_OK) {
            log::error("[MacroSafeguard] Codec configure failed: {}", static_cast<int>(st));
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
            return false;
        }

        // ── Create MP4 muxer ─────────────────────────────────────────────
        // AMediaMuxer needs a real file descriptor
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log::error("[MacroSafeguard] Cannot open output file: {}", path);
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
            return false;
        }
        m_fd    = fd;
        m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            log::error("[MacroSafeguard] Failed to create AMediaMuxer");
            ::close(fd); m_fd = -1;
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        AMediaCodec_start(m_codec);

        m_muxerStarted = false;
        m_videoTrack   = -1;
        m_stopEncoder.store(false);
        m_isRecording.store(true);

        m_encodeThread = std::thread(&VideoRecorder::encodeLoop, this);
        log::info("[MacroSafeguard] Recording started → {}", path);
        return true;
    }

    // ─── Capture current framebuffer — call from the GL thread ───────────
    // (Called in CCEGLView::swapBuffers before the swap, so back buffer
    //  still contains this frame's fully-rendered pixels.)
    void captureFrame() {
        if (!m_isRecording.load()) return;

        // Rate-limit to TARGET_FPS using wall-clock time
        auto now = std::chrono::steady_clock::now();
        int64_t elapsedUs  = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_recordStart).count();
        int64_t expectedUs = m_capturedFrames * (1'000'000LL / TARGET_FPS);
        if (elapsedUs < expectedUs) return;

        std::vector<uint8_t> rgba(static_cast<size_t>(m_width * m_height * 4));
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        {
            std::lock_guard<std::mutex> lk(m_queueMtx);
            m_queue.push({ elapsedUs, std::move(rgba) });
        }
        m_queueCv.notify_one();
        ++m_capturedFrames;
    }

    // ─── Finalize and close the recording ────────────────────────────────
    // saveFile=false → delete the file (attempt didn't qualify)
    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);

        // Signal the encode thread to drain and exit
        m_stopEncoder.store(true);
        m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        // Send EOS to the codec and drain any remaining output
        if (m_codec) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
            if (idx >= 0) {
                AMediaCodec_queueInputBuffer(
                    m_codec, static_cast<size_t>(idx), 0, 0,
                    m_presentationUs,
                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            }
            drainCodec(/*eos=*/true);
            AMediaCodec_stop(m_codec);
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
        }

        if (m_muxerStarted && m_muxer) AMediaMuxer_stop(m_muxer);
        if (m_muxer)  { AMediaMuxer_delete(m_muxer); m_muxer = nullptr; }
        if (m_fd >= 0){ ::close(m_fd); m_fd = -1; }

        if (!saveFile) {
            ::remove(m_path.c_str());
            log::info("[MacroSafeguard] Discarded recording (below threshold)");
        } else {
            log::info("[MacroSafeguard] Saved → {}", m_path);
        }

        m_capturedFrames = 0;
    }

private:
    struct Frame {
        int64_t              timestampUs;
        std::vector<uint8_t> rgba;
    };

    AMediaCodec* m_codec       = nullptr;
    AMediaMuxer* m_muxer       = nullptr;
    int          m_videoTrack  = -1;
    int          m_fd          = -1;
    bool         m_muxerStarted = false;

    int          m_width = 0, m_height = 0;
    std::string  m_path;
    int64_t      m_presentationUs = 0;
    int64_t      m_capturedFrames = 0;

    std::chrono::steady_clock::time_point m_recordStart;

    std::atomic<bool>        m_isRecording{false};
    std::atomic<bool>        m_stopEncoder{false};
    std::thread              m_encodeThread;
    std::mutex               m_queueMtx;
    std::condition_variable  m_queueCv;
    std::queue<Frame>        m_queue;

    // ─── Background encode loop ───────────────────────────────────────────
    // Drains the frame queue, feeds frames to the codec, and writes
    // encoded data to the muxer. Exits once stop() sets m_stopEncoder
    // AND the queue is fully empty.
    void encodeLoop() {
        while (true) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lk(m_queueMtx);
                m_queueCv.wait(lk, [this] {
                    return !m_queue.empty() || m_stopEncoder.load();
                });
                if (m_queue.empty()) break; // stopped and nothing left
                frame = std::move(m_queue.front());
                m_queue.pop();
            }                               // lock released before encoding
            feedFrame(frame);
            drainCodec(/*eos=*/false);
        }
    }

    // ─── Convert one RGBA frame to I420 and feed to the encoder ──────────
    void feedFrame(const Frame& frame) {
        if (!m_codec) return;

        int ySize    = m_width * m_height;
        int uvSize   = ySize / 4;
        int i420Size = ySize + 2 * uvSize;

        ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
        if (idx < 0) return;

        size_t   bufCap = 0;
        uint8_t* buf    = AMediaCodec_getInputBuffer(
            m_codec, static_cast<size_t>(idx), &bufCap);

        if (!buf || static_cast<int>(bufCap) < i420Size) {
            // Buffer too small — skip, release slot
            AMediaCodec_queueInputBuffer(
                m_codec, static_cast<size_t>(idx), 0, 0, frame.timestampUs, 0);
            return;
        }

        rgbaToI420(frame.rgba.data(), m_width, m_height, buf);
        m_presentationUs = frame.timestampUs;

        AMediaCodec_queueInputBuffer(
            m_codec, static_cast<size_t>(idx), 0,
            static_cast<size_t>(i420Size),
            frame.timestampUs, 0);
    }

    // ─── Pull encoded output and write to the MP4 muxer ──────────────────
    void drainCodec(bool eos) {
        if (!m_codec) return;

        AMediaCodecBufferInfo info;
        for (;;) {
            ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(
                m_codec, &info, eos ? 500'000 : 0);

            if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                // First encoded output: grab the format, register the video track,
                // and start the muxer. This must happen exactly once.
                if (!m_muxerStarted && m_muxer) {
                    AMediaFormat* outFmt = AMediaCodec_getOutputFormat(m_codec);
                    m_videoTrack = static_cast<int>(
                        AMediaMuxer_addTrack(m_muxer, outFmt));
                    AMediaFormat_delete(outFmt);
                    AMediaMuxer_start(m_muxer);
                    m_muxerStarted = true;
                }
                continue;
            }

            if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
            if (outIdx < 0) break;  // unexpected error

            // CODEC_CONFIG buffers carry SPS/PPS — already baked into the track
            // format, so we must NOT write them again as sample data.
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
                && m_muxerStarted && m_videoTrack >= 0 && info.size > 0)
            {
                size_t   outCap = 0;
                uint8_t* outBuf = AMediaCodec_getOutputBuffer(
                    m_codec, static_cast<size_t>(outIdx), &outCap);
                if (outBuf) {
                    AMediaMuxer_writeSampleData(
                        m_muxer, static_cast<size_t>(m_videoTrack),
                        outBuf, &info);
                }
            }

            AMediaCodec_releaseOutputBuffer(
                m_codec, static_cast<size_t>(outIdx), false);

            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
//  Globals
// ═══════════════════════════════════════════════════════════════════
static VideoRecorder    g_recorder;
static bool             g_isPlayLayerActive = false;
static uint64_t         g_currentFrame      = 0;

// Input log — kept for potential future replay use
struct ActionEvent {
    uint64_t frame;
    int      button;
    bool     isPress;
    bool     isPlayer2;
};
static std::vector<ActionEvent> g_recordedActions;

// ═══════════════════════════════════════════════════════════════════
//  CCEGLView hook
//  We capture BEFORE swapBuffers so the back buffer still holds the
//  fully-rendered current frame. After the swap the back-buffer
//  content is undefined by the EGL spec.
// ═══════════════════════════════════════════════════════════════════
class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        if (g_isPlayLayerActive) {
            g_recorder.captureFrame();
        }
        CCEGLView::swapBuffers();
    }
};

// ═══════════════════════════════════════════════════════════════════
//  PlayLayer hook
// ═══════════════════════════════════════════════════════════════════
class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        int  m_previousBest          = 0;
        int  m_maxPercentThisAttempt = 0;
        bool m_hasSavedThisAttempt   = false;
        bool m_hasQuit               = false; // prevents startNewRecording after onQuit
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_isPlayLayerActive = true;
        g_currentFrame      = 0;
        g_recordedActions.clear();

        m_fields->m_previousBest          = level->m_normalPercent;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;
        m_fields->m_hasQuit               = false;

        startNewRecording();
        return true;
    }

    // ─── Open a fresh MP4 file for the upcoming attempt ───────────────────
    void startNewRecording() {
        if (m_fields->m_hasQuit) return;

        // Safety: stop any lingering recorder before opening a new file
        if (g_recorder.isRecording()) g_recorder.stop(false);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::stringstream ss;
        ss << "attempt_" << ms << ".mp4";
        auto filePath = Mod::get()->getSaveDir() / ss.str();

        auto* view   = CCDirector::sharedDirector()->getOpenGLView();
        auto  size   = view->getFrameSize();
        // H.264 requires even dimensions
        int   width  = static_cast<int>(size.width)  & ~1;
        int   height = static_cast<int>(size.height) & ~1;

        if (!g_recorder.start(filePath.string(), width, height)) {
            log::error("[MacroSafeguard] Failed to start VideoRecorder");
        }
    }

    // ─── Decide whether to keep or discard the finished recording ─────────
    void checkAndSaveIfQualified() {
        if (m_fields->m_hasSavedThisAttempt) return; // already handled

        bool    onlyNewBest = Mod::get()->getSettingValue<bool>("only-new-best");
        int64_t threshold   = Mod::get()->getSettingValue<int64_t>("custom-percentage");

        bool shouldSave = false;

        if (onlyNewBest) {
            // Compare against the snapshot taken at the start of this attempt
            shouldSave = (m_fields->m_maxPercentThisAttempt > m_fields->m_previousBest);
        } else {
            shouldSave = (m_fields->m_maxPercentThisAttempt >= static_cast<int>(threshold));
        }
        // Always save a full level clear regardless of settings
        if (m_fields->m_maxPercentThisAttempt >= 100) shouldSave = true;

        // Lock immediately so no other path triggers a second save
        m_fields->m_hasSavedThisAttempt = true;
        g_recorder.stop(shouldSave);

        if (shouldSave) {
            Notification::create(
                "Attempt saved as MP4!",
                NotificationIcon::Success,
                2.0f
            )->show();
        }
    }

    // ─── resetLevel: save/discard current attempt, then begin the next ────
    void resetLevel() {
        checkAndSaveIfQualified();

        // FIX: snapshot BEFORE calling super, while m_normalPercent is still
        // the authoritative post-death value (GD updates it before resetLevel)
        int newBest = this->m_level ? this->m_level->m_normalPercent : 0;

        PlayLayer::resetLevel();

        m_fields->m_previousBest          = newBest;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;
        g_currentFrame                     = 0;
        g_recordedActions.clear();

        startNewRecording();
    }

    // ─── levelComplete: force 100%, save immediately, then let GD handle ──
    void levelComplete() {
        m_fields->m_maxPercentThisAttempt = 100;
        checkAndSaveIfQualified();          // finalize before GD shows end screen
        PlayLayer::levelComplete();
    }

    // ─── update: tick frame counter and track max percent ─────────────────
    void update(float dt) {
        PlayLayer::update(dt);
        if (g_isPlayLayerActive) {
            ++g_currentFrame;
            int pct = std::clamp(this->getCurrentPercentInt(), 0, 100);
            if (pct > m_fields->m_maxPercentThisAttempt) {
                m_fields->m_maxPercentThisAttempt = pct;
            }
        }
    }

    // ─── onQuit: catch force-quits during an attempt ──────────────────────
    void onQuit() {
        m_fields->m_hasQuit = true;  // block startNewRecording if resetLevel fires
        checkAndSaveIfQualified();
        g_isPlayLayerActive = false;
        g_recordedActions.clear();
        PlayLayer::onQuit();
    }
};

// ═══════════════════════════════════════════════════════════════════
//  PlayerObject hook — input log
// ═══════════════════════════════════════════════════════════════════
class $modify(MyPlayerObject, PlayerObject) {
    void pushButton(PlayerButton button) {
        PlayerObject::pushButton(button);
        if (!g_isPlayLayerActive) return;
        bool isP2 = false;
        auto pl = PlayLayer::get();
        if (pl && pl->m_player2 == this) isP2 = true;
        g_recordedActions.push_back(
            { g_currentFrame, static_cast<int>(button), true, isP2 });
    }

    void releaseButton(PlayerButton button) {
        PlayerObject::releaseButton(button);
        if (!g_isPlayLayerActive) return;
        bool isP2 = false;
        auto pl = PlayLayer::get();
        if (pl && pl->m_player2 == this) isP2 = true;
        g_recordedActions.push_back(
            { g_currentFrame, static_cast<int>(button), false, isP2 });
    }
};
