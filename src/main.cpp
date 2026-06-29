#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/ui/Notification.hpp>

#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <filesystem>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

#ifdef GEODE_IS_ANDROID
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <unistd.h>

#if __ANDROID_API__ >= 26
#include <aaudio/AAudio.h>
#define AAUDIO_AVAILABLE 1
#else
#define AAUDIO_AVAILABLE 0
#endif
#endif

using namespace geode::prelude;

// ══════════════════════════════════════════════════════════════════
//  Recordings directory (platform-specific, null-safe)
// ══════════════════════════════════════════════════════════════════
static std::string getRecordingsDir() {
    std::string dir;
#ifdef GEODE_IS_ANDROID
    dir = "/storage/emulated/0/MacroSafeguard/";
#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        dir = std::string(appdata) + "\\MacroSafeguard\\";
    } else {
        const char* profile = getenv("USERPROFILE");
        dir = profile ? (std::string(profile) + "\\Documents\\MacroSafeguard\\")
                      : ".\\MacroSafeguard\\";
    }
#else
    const char* home = getenv("HOME");
    dir = home ? (std::string(home) + "/.local/share/MacroSafeguard/")
               : "/tmp/MacroSafeguard/";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) log::warn("[MacroSafeguard] Could not create dir {}: {}", dir, ec.message());
    return dir;
}

// ══════════════════════════════════════════════════════════════════
//  Android-only: video + audio recording
// ══════════════════════════════════════════════════════════════════
#ifdef GEODE_IS_ANDROID

// ─── RGBA → NV12 (YUV420SemiPlanar) with codec stride support ─────
//
// 'stride'      = row stride the codec expects for the Y plane (bytes).
//                 May be > width due to hardware alignment requirements.
// 'sliceHeight' = vertical stride (height of Y plane in the buffer).
//                 May be > height for the same reason.
//
// Without honouring these values every row is shifted, the codec
// silently rejects the malformed input, and the output is 0 bytes.
//
static void rgbaToNV12(
    const uint8_t* rgba, int width, int height,
    int stride, int sliceHeight,
    uint8_t* dst)
{
    uint8_t* yP  = dst;
    uint8_t* uvP = dst + stride * sliceHeight;   // UV plane starts after Y

    // Y plane — flip vertically to undo OpenGL's bottom-up readback
    for (int row = 0; row < height; ++row) {
        const uint8_t* src  = rgba + ((height - 1) - row) * width * 4;
        uint8_t*       yDst = yP   + row * stride;
        for (int col = 0; col < width; ++col) {
            uint8_t r = src[col*4], g = src[col*4+1], b = src[col*4+2];
            yDst[col] = static_cast<uint8_t>(((66*r + 129*g + 25*b + 128) >> 8) + 16);
        }
    }

    // Interleaved UV plane (NV12: U₀V₀ U₁V₁ …)
    // UV stride = stride (same as Y — they are interleaved, not separate planes)
    int uvH = height / 2, uvW = width / 2;
    for (int row = 0; row < uvH; ++row) {
        const uint8_t* src   = rgba + ((height - 1) - row * 2) * width * 4;
        uint8_t*       uvDst = uvP  + row * stride;
        for (int col = 0; col < uvW; ++col) {
            uint8_t r = src[col*2*4], g = src[col*2*4+1], b = src[col*2*4+2];
            uvDst[col*2+0] = static_cast<uint8_t>(((-38*r -  74*g + 112*b + 128) >> 8) + 128);
            uvDst[col*2+1] = static_cast<uint8_t>(((112*r -  94*g -  18*b + 128) >> 8) + 128);
        }
    }
}

// ─── RGBA → I420 (YUV420Planar) with codec stride support ─────────
//
// Fallback for devices where the codec reports format 0x13 (I420)
// after being configured with YUV420Flexible.
//
static void rgbaToI420(
    const uint8_t* rgba, int width, int height,
    int stride, int sliceHeight,
    uint8_t* dst)
{
    int uvStride = stride / 2;
    int uvSlice  = sliceHeight / 2;
    uint8_t* yP = dst;
    uint8_t* uP = dst + stride * sliceHeight;
    uint8_t* vP = uP  + uvStride * uvSlice;

    for (int row = 0; row < height; ++row) {
        const uint8_t* src  = rgba + ((height - 1) - row) * width * 4;
        uint8_t*       yDst = yP   + row * stride;
        for (int col = 0; col < width; ++col) {
            uint8_t r = src[col*4], g = src[col*4+1], b = src[col*4+2];
            yDst[col] = static_cast<uint8_t>(((66*r + 129*g + 25*b + 128) >> 8) + 16);
        }
    }

    int uvH = height / 2, uvW = width / 2;
    for (int row = 0; row < uvH; ++row) {
        const uint8_t* src  = rgba + ((height - 1) - row * 2) * width * 4;
        uint8_t*       uDst = uP   + row * uvStride;
        uint8_t*       vDst = vP   + row * uvStride;
        for (int col = 0; col < uvW; ++col) {
            uint8_t r = src[col*2*4], g = src[col*2*4+1], b = src[col*2*4+2];
            uDst[col] = static_cast<uint8_t>(((-38*r -  74*g + 112*b + 128) >> 8) + 128);
            vDst[col] = static_cast<uint8_t>(((112*r -  94*g -  18*b + 128) >> 8) + 128);
        }
    }
}

// ══════════════════════════════════════════════════════════════════
//  VideoRecorder
// ══════════════════════════════════════════════════════════════════
class VideoRecorder {
public:
    static constexpr int     TARGET_FPS           = 30;
    static constexpr int     BIT_RATE             = 4 * 1024 * 1024;
    // 10 ms: enough for the encoder to finish a frame without stalling.
    // 0 (the original value) causes dequeueOutputBuffer to return
    // AMEDIACODEC_INFO_TRY_AGAIN_LATER immediately, so OUTPUT_FORMAT_CHANGED
    // is missed and the muxer never starts → 0-byte file.
    static constexpr int64_t DRAIN_TIMEOUT_US     = 10'000;
    static constexpr int64_t DRAIN_EOS_TIMEOUT_US = 500'000;

    // COLOR_FormatYUV420Flexible: Android guarantees every codec
    // supports this since API 21. After start() we query the actual
    // layout (real format, stride, slice-height) so we write the
    // buffer exactly as the codec expects.
    static constexpr int32_t COLOR_FLEXIBLE = 0x7F420888;
    static constexpr int32_t COLOR_NV12     = 0x15;  // YUV420SemiPlanar
    static constexpr int32_t COLOR_I420     = 0x13;  // YUV420Planar

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path, int width, int height) {
        if (m_isRecording.load()) return false;

        m_width = width; m_height = height; m_path = path;
        m_presentationUs = 0; m_capturedFrames = 0;
        // Defaults — overwritten below after codec start
        m_stride = width; m_sliceHeight = height; m_colorFormat = COLOR_NV12;
        m_recordStart = std::chrono::steady_clock::now();

        m_codec = AMediaCodec_createEncoderByType("video/avc");
        if (!m_codec) {
            log::error("[MacroSafeguard][Video] No AVC encoder found");
            return false;
        }

        // ── Step 1: configure ──────────────────────────────────────
        // Try COLOR_FormatYUV420Flexible first (guaranteed supported).
        // If it fails for any reason, fall back to NV12 directly.
        media_status_t st = configureCodec(COLOR_FLEXIBLE, width, height);
        bool queriedFmt = (st == AMEDIA_OK);

        if (st != AMEDIA_OK) {
            log::warn("[MacroSafeguard][Video] YUV420Flexible configure failed ({}), "
                      "trying NV12 fallback", (int)st);
            st = configureCodec(COLOR_NV12, width, height);
        }
        if (st != AMEDIA_OK) {
            log::error("[MacroSafeguard][Video] All configure attempts failed: {}", (int)st);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        // ── Step 2: open output file + muxer ──────────────────────
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log::error("[MacroSafeguard][Video] Cannot open file: {} (errno {})", path, errno);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }
        m_fd = fd;

        m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            log::error("[MacroSafeguard][Video] AMediaMuxer_new failed for {}", path);
            ::close(fd); m_fd = -1; ::remove(path.c_str());
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        // ── Step 3: start codec + query actual buffer layout ───────
        AMediaCodec_start(m_codec);

if (queriedFmt) {
    // When configured with YUV420Flexible the codec tells us
    // the real format, stride, and slice-height it expects.
    #if __ANDROID_API__ >= 28
    AMediaFormat* inFmt = AMediaCodec_getInputFormat(m_codec);
    if (inFmt) {
        int32_t cf = 0, s = 0, sh = 0;
        if (AMediaFormat_getInt32(inFmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf) && cf != 0)
            m_colorFormat = cf;
        if (AMediaFormat_getInt32(inFmt, AMEDIAFORMAT_KEY_STRIDE, &s) && s > 0)
            m_stride = s;
        // "slice-height" / AMEDIAFORMAT_KEY_SLICE_HEIGHT (API 28+).
        // Use the string key for compatibility down to API 26.
        if (AMediaFormat_getInt32(inFmt, "slice-height", &sh) && sh > 0)
            m_sliceHeight = sh;
        AMediaFormat_delete(inFmt);
    }
    #endif
}

        log::info("[MacroSafeguard][Video] color={:#x} stride={} sliceH={} → {}",
                  m_colorFormat, m_stride, m_sliceHeight, path);

        m_muxerStarted = false; m_videoTrack = -1;
        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&VideoRecorder::encodeLoop, this);
        return true;
    }

    void captureFrame() {
        if (!m_isRecording.load()) return;
        auto now = std::chrono::steady_clock::now();
        int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_recordStart).count();
        if (elapsedUs < m_capturedFrames * (1'000'000LL / TARGET_FPS)) return;

        std::vector<uint8_t> rgba(static_cast<size_t>(m_width * m_height * 4));
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        {
            std::lock_guard<std::mutex> lk(m_queueMtx);
            m_queue.push({ elapsedUs, std::move(rgba) });
        }
        m_queueCv.notify_one();
        ++m_capturedFrames;
    }

    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);
        m_stopEncoder.store(true);
        m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        if (m_codec) {
            // Drain any output the codec produced from the last batch
            // of frames BEFORE queuing the EOS input — otherwise those
            // final encoded frames are lost when the codec is stopped.
            drainCodec(false);

            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
            if (idx >= 0)
                AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0,
                    m_presentationUs, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            drainCodec(true);

            AMediaCodec_stop(m_codec);
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
        }

        if (m_muxerStarted && m_muxer) AMediaMuxer_stop(m_muxer);
        if (m_muxer)  { AMediaMuxer_delete(m_muxer); m_muxer = nullptr; }
        if (m_fd >= 0){ ::close(m_fd); m_fd = -1; }

        if (!saveFile) {
            ::remove(m_path.c_str());
            log::info("[MacroSafeguard][Video] Discarded");
        } else {
            log::info("[MacroSafeguard][Video] Saved → {}", m_path);
        }
        m_capturedFrames = 0;
    }

private:
    struct Frame { int64_t ts; std::vector<uint8_t> rgba; };

    AMediaCodec*  m_codec      = nullptr;
    AMediaMuxer*  m_muxer      = nullptr;
    int           m_videoTrack = -1;
    int           m_fd         = -1;
    bool          m_muxerStarted = false;
    int           m_width = 0,  m_height = 0;
    int32_t       m_stride = 0, m_sliceHeight = 0, m_colorFormat = COLOR_NV12;
    std::string   m_path;
    int64_t       m_presentationUs = 0, m_capturedFrames = 0;
    std::chrono::steady_clock::time_point m_recordStart;
    std::atomic<bool>       m_isRecording{false}, m_stopEncoder{false};
    std::thread             m_encodeThread;
    std::mutex              m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<Frame>       m_queue;

    // Helper: build and apply a codec format; returns configure status.
    media_status_t configureCodec(int32_t colorFmt, int width, int height) {
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,             "video/avc");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_WIDTH,            width);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_HEIGHT,           height);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_FRAME_RATE,       TARGET_FPS);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,         BIT_RATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,     colorFmt);
        media_status_t st = AMediaCodec_configure(m_codec, fmt, nullptr, nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        return st;
    }

    void encodeLoop() {
        while (true) {
            Frame f;
            {
                std::unique_lock<std::mutex> lk(m_queueMtx);
                m_queueCv.wait(lk, [this]{
                    return !m_queue.empty() || m_stopEncoder.load();
                });
                if (m_queue.empty()) break;
                f = std::move(m_queue.front());
                m_queue.pop();
            }
            feedFrame(f);
            drainCodec(false);
        }
    }

void feedFrame(const Frame& f) {
    if (!m_codec) return;

    // Buffer size uses the actual stride × sliceHeight the codec
    // reported, NOT just width × height.  On many devices these
    // differ (e.g. width=1080 but stride=1088), and writing with
    // the wrong stride corrupts every row so the codec drops input.
    int bufferSize = m_stride * m_sliceHeight * 3 / 2;

    ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
    if (idx < 0) {
        log::warn("[MacroSafeguard][Video] dequeueInputBuffer returned {}", (int)idx);
        return;
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, (size_t)idx, &cap);
    if (!buf) {
        log::warn("[MacroSafeguard][Video] getInputBuffer returned null");
        AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0, f.ts, 0);
        return;
    }
    if ((int)cap < bufferSize) {
        log::warn("[MacroSafeguard][Video] Buffer cap={} < need={}", (int)cap, bufferSize);
        AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0, f.ts, 0);
        return;
    }

    // Zero-fill the whole buffer first so stride padding bytes are
    // well-defined (avoids undefined behaviour in the codec).
    memset(buf, 0, (size_t)bufferSize);

    // Convert based on the actual colour format the codec reported.
    if (m_colorFormat == COLOR_I420) {
        rgbaToI420(f.rgba.data(), m_width, m_height, m_stride, m_sliceHeight, buf);
    } else {
        // Treat everything else as NV12 (the overwhelmingly common case).
        rgbaToNV12(f.rgba.data(), m_width, m_height, m_stride, m_sliceHeight, buf);
    }

    m_presentationUs = f.ts;
    AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0,
        (size_t)bufferSize, f.ts, 0);
}

void drainCodec(bool eos) {
    if (!m_codec) return;
    AMediaCodecBufferInfo info;
    for (;;) {
        ssize_t out = AMediaCodec_dequeueOutputBuffer(
            m_codec, &info,
            eos ? DRAIN_EOS_TIMEOUT_US : DRAIN_TIMEOUT_US);

        if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // The codec has settled on an output format — start the muxer.
            if (!m_muxerStarted && m_muxer) {
                AMediaFormat* outFmt = AMediaCodec_getOutputFormat(m_codec);
                m_videoTrack = (int)AMediaMuxer_addTrack(m_muxer, outFmt);
                AMediaFormat_delete(outFmt);
                AMediaMuxer_start(m_muxer);
                m_muxerStarted = true;
                log::info("[MacroSafeguard][Video] Muxer started, track={}", m_videoTrack);
            }
            continue;
        }

        if (out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }

        if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;

        if (out < 0) {
            log::warn("[MacroSafeguard][Video] Unexpected dequeue value: {}", (int)out);
            break;
        }

        // CRITICAL: Buffer samples even if muxer hasn't started yet
        // They will be written once muxer starts
        if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && info.size > 0)
        {
            // If muxer isn't started yet but we have valid output, 
            // keep dequeuing to find OUTPUT_FORMAT_CHANGED
            if (m_muxerStarted && m_videoTrack >= 0) {
                size_t cap = 0;
                uint8_t* b = AMediaCodec_getOutputBuffer(m_codec, (size_t)out, &cap);
                if (b) {
                    AMediaMuxer_writeSampleData(
                        m_muxer, (size_t)m_videoTrack, b, &info);
                } else {
                    log::warn("[MacroSafeguard][Video] Output buffer null (out={})", out);
                }
            } else if (!eos) {
                log::debug("[MacroSafeguard][Video] Buffered output before muxer start");
            }
        }

        AMediaCodec_releaseOutputBuffer(m_codec, (size_t)out, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
    }
}

// ══════════════════════════════════════════════════════════════════
//  MicRecorder — AAudio → AAC → M4A (Android 26+ only)
// ══════════════════════════════════════════════════════════════════
#if AAUDIO_AVAILABLE

class MicRecorder {
public:
    static constexpr int     SAMPLE_RATE          = 44100;
    static constexpr int     CHANNELS             = 1;
    static constexpr int     AAC_BITRATE          = 128 * 1024;
    static constexpr int64_t DRAIN_TIMEOUT_US     = 10'000;
    static constexpr int64_t DRAIN_EOS_TIMEOUT_US = 500'000;

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path) {
        if (m_isRecording.load()) return false;
        m_path = path;
        m_presentationUs = 0;
        m_startTime = std::chrono::steady_clock::now();

        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
            log::warn("[MacroSafeguard][Mic] AAudio unavailable");
            return false;
        }
        AAudioStreamBuilder_setDirection    (builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSharingMode  (builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate   (builder, SAMPLE_RATE);
        AAudioStreamBuilder_setChannelCount (builder, CHANNELS);
        AAudioStreamBuilder_setFormat       (builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setDataCallback (builder, dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

        aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &m_stream);
        AAudioStreamBuilder_delete(builder);
        if (res != AAUDIO_OK) {
            log::warn("[MacroSafeguard][Mic] Cannot open stream ({}). "
                      "Enable Microphone permission in Android Settings → Apps → GD.",
                      (int)res);
            m_stream = nullptr;
            return false;
        }

        if (!setupEncoder(path)) {
            AAudioStream_close(m_stream); m_stream = nullptr;
            return false;
        }

        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&MicRecorder::encodeLoop, this);

        if (AAudioStream_requestStart(m_stream) != AAUDIO_OK) {
            log::error("[MacroSafeguard][Mic] Failed to start AAudio stream");
            stop(false);
            return false;
        }
        log::info("[MacroSafeguard][Mic] Started → {}", path);
        return true;
    }

    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);

        if (m_stream) {
            AAudioStream_requestStop(m_stream);
            AAudioStream_close(m_stream);
            m_stream = nullptr;
        }

        m_stopEncoder.store(true);
        m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        if (m_codec) {
            drainEncoder(false);   // flush last chunk before EOS
            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
            if (idx >= 0)
                AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0,
                    m_presentationUs, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            drainEncoder(true);
            AMediaCodec_stop(m_codec);
            AMediaCodec_delete(m_codec);
            m_codec = nullptr;
        }

        if (m_muxerStarted && m_muxer) AMediaMuxer_stop(m_muxer);
        if (m_muxer)  { AMediaMuxer_delete(m_muxer); m_muxer = nullptr; }
        if (m_fd >= 0){ ::close(m_fd); m_fd = -1; }

        if (!saveFile) {
            ::remove(m_path.c_str());
            log::info("[MacroSafeguard][Mic] Discarded");
        } else {
            log::info("[MacroSafeguard][Mic] Saved → {}", m_path);
        }
    }

private:
    struct PcmChunk { int64_t ts; std::vector<int16_t> samples; };

    AAudioStream* m_stream     = nullptr;
    AMediaCodec*  m_codec      = nullptr;
    AMediaMuxer*  m_muxer      = nullptr;
    int           m_audioTrack = -1;
    int           m_fd         = -1;
    bool          m_muxerStarted = false;
    std::string   m_path;
    int64_t       m_presentationUs = 0;
    std::chrono::steady_clock::time_point m_startTime;
    std::atomic<bool>       m_isRecording{false}, m_stopEncoder{false};
    std::thread             m_encodeThread;
    std::mutex              m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<PcmChunk>    m_queue;

    bool setupEncoder(const std::string& path) {
        m_codec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        if (!m_codec) {
            log::error("[MacroSafeguard][Mic] No AAC encoder");
            return false;
        }
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,          "audio/mp4a-latm");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE,   SAMPLE_RATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, CHANNELS);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,      AAC_BITRATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_AAC_PROFILE,   2);
        media_status_t st = AMediaCodec_configure(m_codec, fmt, nullptr, nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            log::error("[MacroSafeguard][Mic] AAC configure failed: {}", (int)st);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log::error("[MacroSafeguard][Mic] Cannot open {}", path);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }
        m_fd = fd;
        m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            log::error("[MacroSafeguard][Mic] AMediaMuxer_new failed");
            ::close(fd); m_fd = -1; ::remove(path.c_str());
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }
        AMediaCodec_start(m_codec);
        m_muxerStarted = false; m_audioTrack = -1;
        return true;
    }

    static aaudio_data_callback_result_t dataCallback(
        AAudioStream*, void* ud, void* audio, int32_t nFrames)
    {
        auto* self = static_cast<MicRecorder*>(ud);
        if (!self->m_isRecording.load()) return AAUDIO_CALLBACK_RESULT_STOP;
        auto now = std::chrono::steady_clock::now();
        int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
            now - self->m_startTime).count();
        std::vector<int16_t> samples(nFrames * CHANNELS);
        std::memcpy(samples.data(), audio, samples.size() * sizeof(int16_t));
        {
            std::lock_guard<std::mutex> lk(self->m_queueMtx);
            self->m_queue.push({ ts, std::move(samples) });
        }
        self->m_queueCv.notify_one();
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream*, void*, aaudio_result_t e) {
        log::error("[MacroSafeguard][Mic] Stream error: {}", (int)e);
    }

    void encodeLoop() {
        while (true) {
            PcmChunk chunk;
            {
                std::unique_lock<std::mutex> lk(m_queueMtx);
                m_queueCv.wait(lk, [this]{
                    return !m_queue.empty() || m_stopEncoder.load();
                });
                if (m_queue.empty()) break;
                chunk = std::move(m_queue.front()); m_queue.pop();
            }
            feedPcm(chunk); drainEncoder(false);
        }
    }

    void feedPcm(const PcmChunk& c) {
        if (!m_codec) return;
        size_t bytes = c.samples.size() * sizeof(int16_t);
        ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
        if (idx < 0) return;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, (size_t)idx, &cap);
        if (!buf || cap < bytes) {
            AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0, c.ts, 0);
            return;
        }
        std::memcpy(buf, c.samples.data(), bytes);
        m_presentationUs = c.ts;
        AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, bytes, c.ts, 0);
    }

    void drainEncoder(bool eos) {
        if (!m_codec) return;
        AMediaCodecBufferInfo info;
        for (;;) {
            ssize_t out = AMediaCodec_dequeueOutputBuffer(
                m_codec, &info,
                eos ? DRAIN_EOS_TIMEOUT_US : DRAIN_TIMEOUT_US);

            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if (!m_muxerStarted && m_muxer) {
                    AMediaFormat* outFmt = AMediaCodec_getOutputFormat(m_codec);
                    m_audioTrack = (int)AMediaMuxer_addTrack(m_muxer, outFmt);
                    AMediaFormat_delete(outFmt);
                    AMediaMuxer_start(m_muxer);
                    m_muxerStarted = true;
                    log::info("[MacroSafeguard][Mic] Muxer started, track={}", m_audioTrack);
                }
                continue;
            }
            // Same fix as video: OUTPUT_BUFFERS_CHANGED must continue, not break.
            if (out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) { continue; }
            if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
            if (out < 0) {
                log::warn("[MacroSafeguard][Mic] Unexpected dequeue value: {}", (int)out);
                break;
            }
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
                && m_muxerStarted && m_audioTrack >= 0 && info.size > 0)
            {
                size_t cap = 0;
                uint8_t* b = AMediaCodec_getOutputBuffer(m_codec, (size_t)out, &cap);
                if (b) AMediaMuxer_writeSampleData(m_muxer, (size_t)m_audioTrack, b, &info);
            }
            AMediaCodec_releaseOutputBuffer(m_codec, (size_t)out, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }
};

#else   // Android < 26: stub out mic recording

class MicRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&) {
        log::warn("[MacroSafeguard][Mic] AAudio requires Android 26+");
        return false;
    }
    void stop(bool) {}
};

#endif  // AAUDIO_AVAILABLE

static VideoRecorder g_video;
static MicRecorder   g_mic;

#endif  // GEODE_IS_ANDROID

// ══════════════════════════════════════════════════════════════════
//  Cross-platform globals
// ══════════════════════════════════════════════════════════════════
static bool     g_isPlayLayerActive = false;
static uint64_t g_currentFrame      = 0;

struct ActionEvent { uint64_t frame; int button; bool isPress, isPlayer2; };
static std::vector<ActionEvent> g_recordedActions;

// ══════════════════════════════════════════════════════════════════
//  Cross-platform: save click data as CSV (non-Android platforms)
// ══════════════════════════════════════════════════════════════════
static void saveClicksCSV(const std::vector<ActionEvent>& actions, bool saveFile) {
    if (!saveFile || actions.empty()) return;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string path = getRecordingsDir() + std::to_string(ms) + "_clicks.csv";
    std::ofstream f(path);
    if (!f) { log::error("[MacroSafeguard] Cannot write CSV: {}", path); return; }
    f << "frame,button,press,player2\n";
    for (const auto& e : actions)
        f << e.frame << ',' << e.button << ','
          << (e.isPress ? 1 : 0) << ',' << (e.isPlayer2 ? 1 : 0) << '\n';
    log::info("[MacroSafeguard] Clicks CSV → {}", path);
}

// ══════════════════════════════════════════════════════════════════
//  CCEGLView hook — frame capture, Android only
// ══════════════════════════════════════════════════════════════════
#ifdef GEODE_IS_ANDROID
class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        if (g_isPlayLayerActive) g_video.captureFrame();
        CCEGLView::swapBuffers();
    }
};
#endif

// ══════════════════════════════════════════════════════════════════
//  PlayLayer hook — all platforms
// ══════════════════════════════════════════════════════════════════
class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        int  m_previousBest          = 0;
        int  m_maxPercentThisAttempt = 0;
        bool m_hasSavedThisAttempt   = false;
        bool m_hasQuit               = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_isPlayLayerActive = true;
        g_currentFrame = 0;
        g_recordedActions.clear();
        m_fields->m_previousBest          = level->m_normalPercent;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;
        m_fields->m_hasQuit               = false;
        startNewRecording();
        return true;
    }

    void startNewRecording() {
        if (m_fields->m_hasQuit) return;
        g_recordedActions.clear();

#ifdef GEODE_IS_ANDROID
        if (g_video.isRecording()) g_video.stop(false);
        if (g_mic.isRecording())   g_mic.stop(false);

        auto dir = getRecordingsDir();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        auto* view  = CCDirector::sharedDirector()->getOpenGLView();
        auto  size  = view->getFrameSize();
        int   width  = static_cast<int>(size.width)  & ~1;
        int   height = static_cast<int>(size.height) & ~1;

        if (!g_video.start(dir + std::to_string(ms) + "_video.mp4", width, height))
            log::error("[MacroSafeguard] Video recorder failed to start");

        if (!g_mic.start(dir + std::to_string(ms) + "_clicks.m4a"))
            log::warn("[MacroSafeguard] Mic unavailable — enable Microphone permission in "
                      "Android Settings → Apps → GD → Permissions");
#endif
    }

    void checkAndSaveIfQualified() {
        if (m_fields->m_hasSavedThisAttempt) return;

        bool    onlyNewBest = Mod::get()->getSettingValue<bool>("only-new-best");
        int64_t threshold   = Mod::get()->getSettingValue<int64_t>("custom-percentage");

        bool shouldSave = onlyNewBest
            ? (m_fields->m_maxPercentThisAttempt > m_fields->m_previousBest)
            : (m_fields->m_maxPercentThisAttempt >= static_cast<int>(threshold));

        if (m_fields->m_maxPercentThisAttempt >= 100) shouldSave = true;
        m_fields->m_hasSavedThisAttempt = true;

#ifdef GEODE_IS_ANDROID
        bool hadVideo = g_video.isRecording();
        bool hadMic   = g_mic.isRecording();
        g_video.stop(shouldSave);
        g_mic.stop(shouldSave);

        if (shouldSave) {
            std::string msg = hadVideo && hadMic
                ? "Saved! Video + clicks in MacroSafeguard folder"
                : hadVideo
                    ? "Saved! Video in MacroSafeguard folder (no mic)"
                    : "Attempt qualified but recording failed — check logs";
            Notification::create(msg, NotificationIcon::Success, 3.5f)->show();
            log::info("[MacroSafeguard] Saved at: {}", getRecordingsDir());
        }
#else
        if (shouldSave) {
            saveClicksCSV(g_recordedActions, true);
            Notification::create("Saved! Clicks CSV in MacroSafeguard folder",
                NotificationIcon::Success, 3.5f)->show();
        }
#endif
    }

    void resetLevel() {
        checkAndSaveIfQualified();
        int newBest = m_level ? m_level->m_normalPercent : 0;
        PlayLayer::resetLevel();
        m_fields->m_previousBest          = newBest;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;
        g_currentFrame = 0;
        g_recordedActions.clear();
        startNewRecording();
    }

    void levelComplete() {
        m_fields->m_maxPercentThisAttempt = 100;
        checkAndSaveIfQualified();
        PlayLayer::levelComplete();
    }

    void update(float dt) {
        PlayLayer::update(dt);
        if (g_isPlayLayerActive) {
            ++g_currentFrame;
            int pct = std::clamp(this->getCurrentPercentInt(), 0, 100);
            if (pct > m_fields->m_maxPercentThisAttempt)
                m_fields->m_maxPercentThisAttempt = pct;
        }
    }

    void onQuit() {
        m_fields->m_hasQuit = true;
        checkAndSaveIfQualified();
        g_isPlayLayerActive = false;
        g_recordedActions.clear();
        PlayLayer::onQuit();
    }
};

// ══════════════════════════════════════════════════════════════════
//  PlayerObject hook — all platforms
// ══════════════════════════════════════════════════════════════════
class $modify(MyPlayerObject, PlayerObject) {
    void pushButton(PlayerButton button) {
        PlayerObject::pushButton(button);
        if (!g_isPlayLayerActive) return;
        auto pl = PlayLayer::get();
        bool isP2 = pl && pl->m_player2 == this;
        g_recordedActions.push_back({ g_currentFrame, (int)button, true, isP2 });
    }
    void releaseButton(PlayerButton button) {
        PlayerObject::releaseButton(button);
        if (!g_isPlayLayerActive) return;
        auto pl = PlayLayer::get();
        bool isP2 = pl && pl->m_player2 == this;
        g_recordedActions.push_back({ g_currentFrame, (int)button, false, isP2 });
    }
};

