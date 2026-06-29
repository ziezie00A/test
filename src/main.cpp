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

// ─── Platform-specific includes ──────────────────────────────────

#ifdef GEODE_IS_ANDROID
#  include <media/NdkMediaCodec.h>
#  include <media/NdkMediaMuxer.h>
#  include <media/NdkMediaFormat.h>
#  include <GLES2/gl2.h>
#  include <fcntl.h>
#  include <unistd.h>
#  if __ANDROID_API__ >= 26
#    include <aaudio/AAudio.h>
#    define AAUDIO_AVAILABLE 1
#  else
#    define AAUDIO_AVAILABLE 0
#  endif
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#  include <mferror.h>
#  include <wrl/client.h>
#  include <GL/gl.h>
// MSVC: these #pragmas link the MF libs automatically.
// MinGW users: add the three libs to CMakeLists.txt instead.
#  pragma comment(lib, "mfreadwrite.lib")
#  pragma comment(lib, "mfplat.lib")
#  pragma comment(lib, "mfuuid.lib")
using Microsoft::WRL::ComPtr;
#endif

#ifdef __APPLE__
#  include <OpenGL/gl.h>
#  include "mac_recorder.h"
#endif

using namespace geode::prelude;

// ================================================================
//  Recordings directory — platform-specific, null-safe
// ================================================================
static std::string getRecordingsDir() {
    std::string dir;

#ifdef GEODE_IS_ANDROID
    dir = "/storage/emulated/0/MacroSafeguard/";

#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        dir = std::string(appdata) + "\\MacroSafeguard\\";
    } else {
        const char* userprofile = getenv("USERPROFILE");
        if (userprofile) {
            dir = std::string(userprofile) + "\\Documents\\MacroSafeguard\\";
        } else {
            dir = ".\\MacroSafeguard\\"; // last-resort fallback
        }
    }

#elif defined(__APPLE__)
    // ~/Movies/ is the conventional location for user video on macOS
    const char* home = getenv("HOME");
    if (home) {
        dir = std::string(home) + "/Movies/MacroSafeguard/";
    } else {
        dir = "/tmp/MacroSafeguard/";
    }

#else
    // Linux / other Unix
    const char* home = getenv("HOME");
    if (home) {
        dir = std::string(home) + "/.local/share/MacroSafeguard/";
    } else {
        dir = "/tmp/MacroSafeguard/";
    }
#endif

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) log::warn("[MacroSafeguard] Could not create dir: {}", dir);
    return dir;
}

// ================================================================
//  RGBA (OpenGL, bottom-up) → NV12 (top-down, interleaved UV)
//
//  Used by the Android and Windows VideoRecorders.
//  NV12 = COLOR_FormatYUV420SemiPlanar (0x15) — universally
//  supported by every Android and Windows H.264 hardware encoder.
//  The old I420 (0x13) format was NOT guaranteed and caused the
//  codec to produce zero output, resulting in a 0-byte MP4.
// ================================================================
static void rgbaToNV12(const uint8_t* rgba, int width, int height,
                       uint8_t* dst)
{
    uint8_t* yP  = dst;
    uint8_t* uvP = dst + width * height;

    // Y plane — flip vertically to undo OpenGL's bottom-up origin
    for (int row = 0; row < height; ++row) {
        int            srcRow = (height - 1) - row;
        const uint8_t* src    = rgba + srcRow * width * 4;
        uint8_t*       yDst   = yP   + row    * width;
        for (int col = 0; col < width; ++col) {
            uint8_t r = src[col*4], g = src[col*4+1], b = src[col*4+2];
            yDst[col] = (uint8_t)(((66*r + 129*g + 25*b + 128) >> 8) + 16);
        }
    }

    // Interleaved UV plane — stride = width (U0 V0 U1 V1 …)
    int uvH = height / 2, uvW = width / 2;
    for (int row = 0; row < uvH; ++row) {
        int            srcRow = (height - 1) - row * 2;
        const uint8_t* src    = rgba + srcRow * width * 4;
        uint8_t*       uvDst  = uvP  + row    * width;
        for (int col = 0; col < uvW; ++col) {
            uint8_t r = src[col*2*4], g = src[col*2*4+1], b = src[col*2*4+2];
            uvDst[col*2]   = (uint8_t)(((-38*r -  74*g + 112*b + 128) >> 8) + 128);
            uvDst[col*2+1] = (uint8_t)(((112*r -  94*g -  18*b + 128) >> 8) + 128);
        }
    }
}

// ================================================================
//  ▓▓▓▓▓▓▓  ANDROID  ▓▓▓▓▓▓▓
// ================================================================
#ifdef GEODE_IS_ANDROID

class VideoRecorder {
public:
    static constexpr int     TARGET_FPS           = 30;
    static constexpr int     BIT_RATE             = 4 * 1024 * 1024;
    static constexpr int64_t DRAIN_TIMEOUT_US     = 10'000;   // 10 ms per-frame
    static constexpr int64_t DRAIN_EOS_TIMEOUT_US = 500'000;  // 500 ms final EOS

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path, int width, int height) {
        if (m_isRecording.load()) return false;
        m_width = width; m_height = height; m_path = path;
        m_presentationUs = 0; m_capturedFrames = 0;
        m_recordStart = std::chrono::steady_clock::now();

        m_codec = AMediaCodec_createEncoderByType("video/avc");
        if (!m_codec) {
            log::error("[MacroSafeguard][Video] No AVC encoder"); return false;
        }

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,             "video/avc");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_WIDTH,            width);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_HEIGHT,           height);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_FRAME_RATE,       TARGET_FPS);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,         BIT_RATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,     0x15); // NV12
        media_status_t st = AMediaCodec_configure(m_codec, fmt, nullptr, nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            log::error("[MacroSafeguard][Video] Codec configure failed: {}", (int)st);
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }

        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log::error("[MacroSafeguard][Video] Cannot open: {}", path);
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }
        m_fd = fd;
        m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            log::error("[MacroSafeguard][Video] AMediaMuxer_new failed");
            ::close(fd); m_fd = -1; ::remove(path.c_str());
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }

        AMediaCodec_start(m_codec);
        m_muxerStarted = false; m_videoTrack = -1;
        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&VideoRecorder::encodeLoop, this);
        log::info("[MacroSafeguard][Video] Started → {}", path);
        return true;
    }

    void captureFrame() {
        if (!m_isRecording.load()) return;
        auto now = std::chrono::steady_clock::now();
        int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_recordStart).count();
        if (elapsedUs < m_capturedFrames * (1'000'000LL / TARGET_FPS)) return;

        std::vector<uint8_t> rgba(m_width * m_height * 4);
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
        m_stopEncoder.store(true); m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        if (m_codec) {
            drainCodec(false); // flush any buffered output before EOS
            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
            if (idx >= 0)
                AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0,
                    m_presentationUs, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            drainCodec(true);
            AMediaCodec_stop(m_codec); AMediaCodec_delete(m_codec); m_codec = nullptr;
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

    AMediaCodec* m_codec = nullptr;
    AMediaMuxer* m_muxer = nullptr;
    int m_videoTrack = -1, m_fd = -1;
    bool m_muxerStarted = false;
    int m_width = 0, m_height = 0;
    std::string m_path;
    int64_t m_presentationUs = 0, m_capturedFrames = 0;
    std::chrono::steady_clock::time_point m_recordStart;
    std::atomic<bool> m_isRecording{false}, m_stopEncoder{false};
    std::thread m_encodeThread;
    std::mutex m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<Frame> m_queue;

    void encodeLoop() {
        while (true) {
            Frame f;
            {
                std::unique_lock<std::mutex> lk(m_queueMtx);
                m_queueCv.wait(lk, [this]{
                    return !m_queue.empty() || m_stopEncoder.load();
                });
                if (m_queue.empty()) break;
                f = std::move(m_queue.front()); m_queue.pop();
            }
            feedFrame(f);
            drainCodec(false);
        }
    }

    void feedFrame(const Frame& f) {
        if (!m_codec) return;
        int nv12Size = m_width * m_height * 3 / 2;
        ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
        if (idx < 0) return;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, (size_t)idx, &cap);
        if (!buf || (int)cap < nv12Size) {
            log::warn("[MacroSafeguard][Video] Input buffer too small");
            AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0, f.ts, 0);
            return;
        }
        rgbaToNV12(f.rgba.data(), m_width, m_height, buf);
        m_presentationUs = f.ts;
        AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, (size_t)nv12Size, f.ts, 0);
    }

    void drainCodec(bool eos) {
        if (!m_codec) return;
        AMediaCodecBufferInfo info;
        for (;;) {
            ssize_t out = AMediaCodec_dequeueOutputBuffer(
                m_codec, &info, eos ? DRAIN_EOS_TIMEOUT_US : DRAIN_TIMEOUT_US);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if (!m_muxerStarted && m_muxer) {
                    AMediaFormat* outFmt = AMediaCodec_getOutputFormat(m_codec);
                    m_videoTrack = (int)AMediaMuxer_addTrack(m_muxer, outFmt);
                    AMediaFormat_delete(outFmt);
                    AMediaMuxer_start(m_muxer);
                    m_muxerStarted = true;
                    log::info("[MacroSafeguard][Video] Muxer started");
                }
                continue;
            }
            if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
            if (out < 0) {
                log::warn("[MacroSafeguard][Video] dequeueOutput: {}", (int)out);
                break;
            }
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
                && m_muxerStarted && m_videoTrack >= 0 && info.size > 0) {
                size_t cap = 0;
                uint8_t* b = AMediaCodec_getOutputBuffer(m_codec, (size_t)out, &cap);
                if (b) AMediaMuxer_writeSampleData(m_muxer, (size_t)m_videoTrack, b, &info);
            }
            AMediaCodec_releaseOutputBuffer(m_codec, (size_t)out, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }
};

// ─── MicRecorder (Android, AAudio → AAC M4A) ─────────────────────
#if AAUDIO_AVAILABLE

class MicRecorder {
public:
    static constexpr int     SAMPLE_RATE           = 44100;
    static constexpr int     CHANNELS              = 1;
    static constexpr int     AAC_BITRATE           = 128 * 1024;
    static constexpr int64_t DRAIN_TIMEOUT_US      = 10'000;
    static constexpr int64_t DRAIN_EOS_TIMEOUT_US  = 500'000;

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path) {
        if (m_isRecording.load()) return false;
        m_path = path; m_presentationUs = 0;
        m_startTime = std::chrono::steady_clock::now();

        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
            log::warn("[MacroSafeguard][Mic] AAudio unavailable"); return false;
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
                      "Grant Microphone permission in Android Settings → Apps → GD.",
                      (int)res);
            m_stream = nullptr; return false;
        }

        if (!setupEncoder(path)) {
            AAudioStream_close(m_stream); m_stream = nullptr; return false;
        }

        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&MicRecorder::encodeLoop, this);

        if (AAudioStream_requestStart(m_stream) != AAUDIO_OK) {
            log::error("[MacroSafeguard][Mic] Failed to start stream");
            stop(false); return false;
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
        m_stopEncoder.store(true); m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        if (m_codec) {
            drainEncoder(false); // flush before EOS
            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
            if (idx >= 0)
                AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0,
                    m_presentationUs, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            drainEncoder(true);
            AMediaCodec_stop(m_codec); AMediaCodec_delete(m_codec); m_codec = nullptr;
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
    AAudioStream* m_stream = nullptr;
    AMediaCodec*  m_codec  = nullptr;
    AMediaMuxer*  m_muxer  = nullptr;
    int m_audioTrack = -1, m_fd = -1;
    bool m_muxerStarted = false;
    std::string m_path;
    int64_t m_presentationUs = 0;
    std::chrono::steady_clock::time_point m_startTime;
    std::atomic<bool> m_isRecording{false}, m_stopEncoder{false};
    std::thread m_encodeThread;
    std::mutex m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<PcmChunk> m_queue;

    bool setupEncoder(const std::string& path) {
        m_codec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        if (!m_codec) {
            log::error("[MacroSafeguard][Mic] No AAC encoder"); return false;
        }
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,           "audio/mp4a-latm");
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE,    SAMPLE_RATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT,  CHANNELS);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_BIT_RATE,       AAC_BITRATE);
        AMediaFormat_setInt32 (fmt, AMEDIAFORMAT_KEY_AAC_PROFILE,    2);
        media_status_t st = AMediaCodec_configure(m_codec, fmt, nullptr, nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            log::error("[MacroSafeguard][Mic] AAC configure failed: {}", (int)st);
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log::error("[MacroSafeguard][Mic] Cannot open: {}", path);
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }
        m_fd = fd;
        m_muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            log::error("[MacroSafeguard][Mic] AMediaMuxer_new failed");
            ::close(fd); m_fd = -1; ::remove(path.c_str());
            AMediaCodec_delete(m_codec); m_codec = nullptr; return false;
        }
        AMediaCodec_start(m_codec);
        m_muxerStarted = false; m_audioTrack = -1;
        return true;
    }

    static aaudio_data_callback_result_t dataCallback(
        AAudioStream*, void* ud, void* data, int32_t numFrames)
    {
        auto* self = static_cast<MicRecorder*>(ud);
        if (!self->m_isRecording.load()) return AAUDIO_CALLBACK_RESULT_STOP;
        auto now = std::chrono::steady_clock::now();
        int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
            now - self->m_startTime).count();
        std::vector<int16_t> samples(numFrames * CHANNELS);
        std::memcpy(samples.data(), data, samples.size() * sizeof(int16_t));
        {
            std::lock_guard<std::mutex> lk(self->m_queueMtx);
            self->m_queue.push({ ts, std::move(samples) });
        }
        self->m_queueCv.notify_one();
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream*, void*, aaudio_result_t e) {
        log::error("[MacroSafeguard][Mic] AAudio error: {}", (int)e);
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
            feedPcm(chunk);
            drainEncoder(false);
        }
    }

    void feedPcm(const PcmChunk& chunk) {
        if (!m_codec) return;
        size_t bytes = chunk.samples.size() * sizeof(int16_t);
        ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 100'000);
        if (idx < 0) return;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, (size_t)idx, &cap);
        if (!buf || cap < bytes) {
            AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, 0, chunk.ts, 0);
            return;
        }
        std::memcpy(buf, chunk.samples.data(), bytes);
        m_presentationUs = chunk.ts;
        AMediaCodec_queueInputBuffer(m_codec, (size_t)idx, 0, bytes, chunk.ts, 0);
    }

    void drainEncoder(bool eos) {
        if (!m_codec) return;
        AMediaCodecBufferInfo info;
        for (;;) {
            ssize_t out = AMediaCodec_dequeueOutputBuffer(
                m_codec, &info, eos ? DRAIN_EOS_TIMEOUT_US : DRAIN_TIMEOUT_US);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if (!m_muxerStarted && m_muxer) {
                    AMediaFormat* outFmt = AMediaCodec_getOutputFormat(m_codec);
                    m_audioTrack = (int)AMediaMuxer_addTrack(m_muxer, outFmt);
                    AMediaFormat_delete(outFmt);
                    AMediaMuxer_start(m_muxer); m_muxerStarted = true;
                    log::info("[MacroSafeguard][Mic] Muxer started");
                }
                continue;
            }
            if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
            if (out < 0) break;
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
                && m_muxerStarted && m_audioTrack >= 0 && info.size > 0) {
                size_t cap = 0;
                uint8_t* b = AMediaCodec_getOutputBuffer(m_codec, (size_t)out, &cap);
                if (b) AMediaMuxer_writeSampleData(m_muxer, (size_t)m_audioTrack, b, &info);
            }
            AMediaCodec_releaseOutputBuffer(m_codec, (size_t)out, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }
};

#else // Android < API 26 — no AAudio

class MicRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&) {
        log::warn("[MacroSafeguard][Mic] AAudio requires Android 26+");
        return false;
    }
    void stop(bool) {}
};

#endif // AAUDIO_AVAILABLE
#endif // GEODE_IS_ANDROID

// ================================================================
//  ▓▓▓▓▓▓▓  WINDOWS  ▓▓▓▓▓▓▓
//  Uses Windows Media Foundation (IMFSinkWriter) for H.264 MP4.
//  Hardware encoding is used automatically when available.
//  Available on Windows 7+ with the Media Feature Pack installed
//  (built-in on all non-N editions of Windows 10 and 11).
// ================================================================
#ifdef _WIN32

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

class VideoRecorder {
public:
    static constexpr int TARGET_FPS = 30;
    static constexpr int BIT_RATE   = 4 * 1024 * 1024;

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path, int width, int height) {
        if (m_isRecording.load()) return false;
        m_width = width; m_height = height; m_path = path; m_frameCount = 0;

        if (FAILED(MFStartup(MF_VERSION))) {
            log::error("[MacroSafeguard][Video] MFStartup failed"); return false;
        }
        m_mfStarted = true;

        // Enable hardware transforms (hardware H.264 encoding) when available
        ComPtr<IMFAttributes> pAttr;
        MFCreateAttributes(&pAttr, 1);
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        HRESULT hr = MFCreateSinkWriterFromURL(
            utf8ToWide(path).c_str(), nullptr, pAttr.Get(), &m_sinkWriter);
        if (FAILED(hr)) {
            log::error("[MacroSafeguard][Video] MFCreateSinkWriterFromURL: {:08X}", (UINT)hr);
            cleanup(); return false;
        }

        // Output media type: H.264
        ComPtr<IMFMediaType> pOut;
        MFCreateMediaType(&pOut);
        pOut->SetGUID   (MF_MT_MAJOR_TYPE,         MFMediaType_Video);
        pOut->SetGUID   (MF_MT_SUBTYPE,            MFVideoFormat_H264);
        pOut->SetUINT32 (MF_MT_AVG_BITRATE,        BIT_RATE);
        pOut->SetUINT32 (MF_MT_INTERLACE_MODE,     MFVideoInterlace_Progressive);
        MFSetAttributeSize (pOut.Get(), MF_MT_FRAME_SIZE,         width, height);
        MFSetAttributeRatio(pOut.Get(), MF_MT_FRAME_RATE,         TARGET_FPS, 1);
        MFSetAttributeRatio(pOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = m_sinkWriter->AddStream(pOut.Get(), &m_streamIdx);
        if (FAILED(hr)) {
            log::error("[MacroSafeguard][Video] AddStream: {:08X}", (UINT)hr);
            cleanup(); return false;
        }

        // Input media type: NV12 (top-down, stride = width)
        // NV12 is universally supported by Windows H.264 hardware encoders.
        ComPtr<IMFMediaType> pIn;
        MFCreateMediaType(&pIn);
        pIn->SetGUID   (MF_MT_MAJOR_TYPE,         MFMediaType_Video);
        pIn->SetGUID   (MF_MT_SUBTYPE,            MFVideoFormat_NV12);
        pIn->SetUINT32 (MF_MT_INTERLACE_MODE,     MFVideoInterlace_Progressive);
        pIn->SetUINT32 (MF_MT_DEFAULT_STRIDE,     (UINT32)width); // Y stride = width
        MFSetAttributeSize (pIn.Get(), MF_MT_FRAME_SIZE,         width, height);
        MFSetAttributeRatio(pIn.Get(), MF_MT_FRAME_RATE,         TARGET_FPS, 1);
        MFSetAttributeRatio(pIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = m_sinkWriter->SetInputMediaType(m_streamIdx, pIn.Get(), nullptr);
        if (FAILED(hr)) {
            log::error("[MacroSafeguard][Video] SetInputMediaType: {:08X}", (UINT)hr);
            cleanup(); return false;
        }

        hr = m_sinkWriter->BeginWriting();
        if (FAILED(hr)) {
            log::error("[MacroSafeguard][Video] BeginWriting: {:08X}", (UINT)hr);
            cleanup(); return false;
        }

        m_recordStart = std::chrono::steady_clock::now();
        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&VideoRecorder::encodeLoop, this);
        log::info("[MacroSafeguard][Video] Started → {}", path);
        return true;
    }

    void captureFrame() {
        if (!m_isRecording.load()) return;
        auto now = std::chrono::steady_clock::now();
        int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_recordStart).count();
        if (elapsedUs < m_frameCount * (1'000'000LL / TARGET_FPS)) return;

        std::vector<uint8_t> rgba(m_width * m_height * 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        {
            std::lock_guard<std::mutex> lk(m_queueMtx);
            m_queue.push({ elapsedUs, std::move(rgba) });
        }
        m_queueCv.notify_one();
        ++m_frameCount;
    }

    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);
        m_stopEncoder.store(true); m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();

        if (m_sinkWriter) m_sinkWriter->Finalize();
        cleanup();

        if (!saveFile) {
            std::error_code ec;
            std::filesystem::remove(m_path, ec);
            log::info("[MacroSafeguard][Video] Discarded");
        } else {
            log::info("[MacroSafeguard][Video] Saved → {}", m_path);
        }
        m_frameCount = 0;
    }

private:
    struct Frame { int64_t ts; std::vector<uint8_t> rgba; };

    ComPtr<IMFSinkWriter> m_sinkWriter;
    DWORD m_streamIdx = 0;
    int m_width = 0, m_height = 0;
    std::string m_path;
    int64_t m_frameCount = 0;
    bool m_mfStarted = false;
    std::chrono::steady_clock::time_point m_recordStart;
    std::atomic<bool> m_isRecording{false}, m_stopEncoder{false};
    std::thread m_encodeThread;
    std::mutex m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<Frame> m_queue;

    void cleanup() {
        m_sinkWriter.Reset();
        if (m_mfStarted) { MFShutdown(); m_mfStarted = false; }
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
                f = std::move(m_queue.front()); m_queue.pop();
            }
            writeFrame(f);
        }
    }

    void writeFrame(const Frame& f) {
        if (!m_sinkWriter) return;

        // Convert RGBA (OpenGL bottom-up) → NV12 (top-down)
        int nv12Size = m_width * m_height * 3 / 2;
        std::vector<uint8_t> nv12(nv12Size);
        rgbaToNV12(f.rgba.data(), m_width, m_height, nv12.data());

        // Wrap in an MF media buffer
        ComPtr<IMFMediaBuffer> pBuf;
        if (FAILED(MFCreateMemoryBuffer((DWORD)nv12Size, &pBuf))) return;
        BYTE* pData = nullptr;
        if (FAILED(pBuf->Lock(&pData, nullptr, nullptr))) return;
        memcpy(pData, nv12.data(), nv12Size);
        pBuf->Unlock();
        pBuf->SetCurrentLength((DWORD)nv12Size);

        // Wrap buffer in an MF sample with timestamp + duration
        ComPtr<IMFSample> pSample;
        if (FAILED(MFCreateSample(&pSample))) return;
        pSample->AddBuffer(pBuf.Get());
        // MF timestamps are in 100-nanosecond units (1 us = 10 × 100ns)
        pSample->SetSampleTime    (f.ts * 10LL);
        pSample->SetSampleDuration(10'000'000LL / TARGET_FPS);

        HRESULT hr = m_sinkWriter->WriteSample(m_streamIdx, pSample.Get());
        if (FAILED(hr))
            log::warn("[MacroSafeguard][Video] WriteSample: {:08X}", (UINT)hr);
    }
};

// Mic recording not implemented on Windows (stub)
class MicRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&) { return false; }
    void stop(bool) {}
};

#endif // _WIN32

// ================================================================
//  ▓▓▓▓▓▓▓  macOS  ▓▓▓▓▓▓▓
//  Wraps mac_recorder.mm (AVFoundation, Objective-C++).
//  The queue + encode-thread pattern keeps glReadPixels on the
//  main thread while the AVAssetWriter runs in the background.
// ================================================================
#ifdef __APPLE__

class VideoRecorder {
public:
    static constexpr int TARGET_FPS = 30;
    static constexpr int BIT_RATE   = 4 * 1024 * 1024;

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& path, int width, int height) {
        if (m_isRecording.load()) return false;
        m_width = width; m_height = height; m_frameCount = 0;
        m_recordStart = std::chrono::steady_clock::now();

        if (!mac_video_start(path.c_str(), width, height, TARGET_FPS, BIT_RATE))
            return false;

        m_stopEncoder.store(false); m_isRecording.store(true);
        m_encodeThread = std::thread(&VideoRecorder::encodeLoop, this);
        log::info("[MacroSafeguard][Video] Started (macOS) → {}", path);
        return true;
    }

    void captureFrame() {
        if (!m_isRecording.load()) return;
        auto now = std::chrono::steady_clock::now();
        int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_recordStart).count();
        if (elapsedUs < m_frameCount * (1'000'000LL / TARGET_FPS)) return;

        std::vector<uint8_t> rgba(m_width * m_height * 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        {
            std::lock_guard<std::mutex> lk(m_queueMtx);
            m_queue.push({ elapsedUs, std::move(rgba) });
        }
        m_queueCv.notify_one();
        ++m_frameCount;
    }

    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);
        m_stopEncoder.store(true); m_queueCv.notify_all();
        if (m_encodeThread.joinable()) m_encodeThread.join();
        mac_video_stop(saveFile); // finalize AVAssetWriter (blocks until done)
        m_frameCount = 0;
    }

private:
    struct Frame { int64_t ts; std::vector<uint8_t> rgba; };

    int m_width = 0, m_height = 0;
    int64_t m_frameCount = 0;
    std::chrono::steady_clock::time_point m_recordStart;
    std::atomic<bool> m_isRecording{false}, m_stopEncoder{false};
    std::thread m_encodeThread;
    std::mutex m_queueMtx;
    std::condition_variable m_queueCv;
    std::queue<Frame> m_queue;

    void encodeLoop() {
        while (true) {
            Frame f;
            {
                std::unique_lock<std::mutex> lk(m_queueMtx);
                m_queueCv.wait(lk, [this]{
                    return !m_queue.empty() || m_stopEncoder.load();
                });
                if (m_queue.empty()) break;
                f = std::move(m_queue.front()); m_queue.pop();
            }
            // mac_video_feed_frame converts RGBA→BGRA, creates CVPixelBuffer,
            // and appends it to the AVAssetWriterInput.
            mac_video_feed_frame(f.rgba.data(), m_width, m_height, f.ts);
        }
    }
};

// Mic recording not implemented on macOS (stub)
class MicRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&) { return false; }
    void stop(bool) {}
};

#endif // __APPLE__

// ================================================================
//  Stub VideoRecorder / MicRecorder for unsupported platforms
//  (e.g. Linux, iOS). Everything compiles; recording is a no-op.
// ================================================================
#if !defined(GEODE_IS_ANDROID) && !defined(_WIN32) && !defined(__APPLE__)

class VideoRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&, int, int) { return false; }
    void captureFrame() {}
    void stop(bool) {}
};

class MicRecorder {
public:
    bool isRecording() const { return false; }
    bool start(const std::string&) { return false; }
    void stop(bool) {}
};

#endif

// ================================================================
//  Global instances — now exist on every platform
// ================================================================
static VideoRecorder g_video;
static MicRecorder   g_mic;

// ================================================================
//  Cross-platform globals
// ================================================================
static bool     g_isPlayLayerActive = false;
static uint64_t g_currentFrame      = 0;

struct ActionEvent {
    uint64_t frame; int button; bool isPress, isPlayer2;
};
static std::vector<ActionEvent> g_recordedActions;

// ================================================================
//  Save input events as a CSV — all platforms
// ================================================================
static void saveClicksCSV(const std::vector<ActionEvent>& actions, bool save) {
    if (!save || actions.empty()) return;
    auto dir = getRecordingsDir();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string path = dir + std::to_string(ms) + "_clicks.csv";
    std::ofstream f(path);
    if (!f.is_open()) {
        log::error("[MacroSafeguard] Cannot write CSV: {}", path); return;
    }
    f << "frame,button,press,player2\n";
    for (const auto& e : actions)
        f << e.frame << ',' << e.button << ','
          << (e.isPress ? 1 : 0) << ',' << (e.isPlayer2 ? 1 : 0) << '\n';
    log::info("[MacroSafeguard] Clicks CSV → {}", path);
}

// ================================================================
//  CCEGLView hook — ALL platforms (frame capture)
//  Previously Android-only; now the same hook works everywhere
//  since GD uses OpenGL on every supported desktop OS.
// ================================================================
class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        if (g_isPlayLayerActive) g_video.captureFrame();
        CCEGLView::swapBuffers();
    }
};

// ================================================================
//  PlayLayer hook — all platforms
// ================================================================
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
        if (g_video.isRecording()) g_video.stop(false);
        if (g_mic.isRecording())   g_mic.stop(false);

        auto dir = getRecordingsDir();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string videoPath = dir + std::to_string(ms) + "_video.mp4";
        std::string micPath   = dir + std::to_string(ms) + "_clicks.m4a";

        auto* view   = CCDirector::sharedDirector()->getOpenGLView();
        auto  size   = view->getFrameSize();
        int   width  = static_cast<int>(size.width)  & ~1;
        int   height = static_cast<int>(size.height) & ~1;

        if (!g_video.start(videoPath, width, height))
            log::error("[MacroSafeguard] Video recorder failed to start");

        if (!g_mic.start(micPath)) {
#ifdef GEODE_IS_ANDROID
            log::warn("[MacroSafeguard] Mic unavailable — grant Microphone "
                      "permission in Android Settings → Apps → GD → Permissions");
#endif
        }
        g_recordedActions.clear();
    }

    void checkAndSaveIfQualified() {
        if (m_fields->m_hasSavedThisAttempt) return;

        bool    onlyNewBest = Mod::get()->getSettingValue<bool>("only-new-best");
        int64_t threshold   = Mod::get()->getSettingValue<int64_t>("custom-percentage");

        bool shouldSave = false;
        if (onlyNewBest) {
            shouldSave = (m_fields->m_maxPercentThisAttempt > m_fields->m_previousBest);
        } else {
            shouldSave = (m_fields->m_maxPercentThisAttempt >= (int)threshold);
        }
        if (m_fields->m_maxPercentThisAttempt >= 100) shouldSave = true;

        m_fields->m_hasSavedThisAttempt = true;

        bool hadVideo = g_video.isRecording();
        bool hadMic   = g_mic.isRecording();
        g_video.stop(shouldSave);
        g_mic.stop(shouldSave);
        saveClicksCSV(g_recordedActions, shouldSave);

        if (shouldSave) {
            std::string msg;
            if (hadVideo && hadMic)
                msg = "Saved! Video + mic in MacroSafeguard folder";
            else if (hadVideo)
                msg = "Saved! Video in MacroSafeguard folder";
            else
                msg = "Saved! Clicks CSV in MacroSafeguard folder";

            Notification::create(msg, NotificationIcon::Success, 3.5f)->show();
            log::info("[MacroSafeguard] Saved at: {}", getRecordingsDir());
        }
    }

    void resetLevel() {
        checkAndSaveIfQualified();
        int newBest = this->m_level ? this->m_level->m_normalPercent : 0;
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

// ================================================================
//  PlayerObject hook — all platforms
// ================================================================
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
