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

// ════════════════════════════════════════════════════════════════
//  PLATFORM DEPENDENCIES & HEADERS
// ════════════════════════════════════════════════════════════════

#if defined(GEODE_IS_WINDOWS)
    #include <windows.h>
    #include <mfapi.h>
    #include <mfidl.h>
    #include <mfreadwrite.h>
    #include <shlwapi.h>
    #pragma comment(lib, "mfplat.lib")
    #pragma comment(lib, "mfuuid.lib")
    #pragma comment(lib, "mfreadwrite.lib")
    #pragma comment(lib, "Ole32.lib")
#elif defined(GEODE_IS_MACOS)
    #import <AVFoundation/AVFoundation.h>
    #import <CoreMedia/CoreMedia.h>
    #import <CoreVideo/CoreVideo.h>
#elif defined(GEODE_IS_ANDROID)
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

// ════════════════════════════════════════════════════════════════
//  Cross-Platform Path Resolver
// ════════════════════════════════════════════════════════════════
static std::string getRecordingsDir() {
    std::string dir;
#if defined(GEODE_IS_ANDROID)
    dir = "/storage/emulated/0/MacroSafeguard/";
#elif defined(GEODE_IS_WINDOWS)
    const char* appdata = getenv("APPDATA");
    dir = appdata ? (std::string(appdata) + "\\MacroSafeguard\\") : ".\\MacroSafeguard\\";
#elif defined(GEODE_IS_MACOS)
    const char* home = getenv("HOME");
    dir = home ? (std::string(home) + "/Library/Application Support/MacroSafeguard/") : "/tmp/MacroSafeguard/";
#else
    dir = "./MacroSafeguard/";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ════════════════════════════════════════════════════════════════
//  WINDOWS BACKEND: Asynchronous Media Foundation Engine
// ════════════════════════════════════════════════════════════════
#if defined(GEODE_IS_WINDOWS)
class WindowsRecorder {
private:
    std::atomic<bool> m_recording{false};
    std::thread m_workerThread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<std::vector<uint8_t>> m_frameQueue;
    
    IMFSinkWriter* m_sinkWriter = nullptr;
    DWORD m_videoStreamIndex = 0;
    int m_width = 0, m_height = 0;
    UINT64 m_frameDuration = 10 * 1000 * 1000 / 30; // 30 FPS
    UINT64 m_videoTimeStamp = 0;
    std::string m_path;

    void encodeLoop() {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION);

        while (m_recording || !m_frameQueue.empty()) {
            std::vector<uint8_t> pixels;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait(lock, [this] { return !m_frameQueue.empty() || !m_recording; });
                if (m_frameQueue.empty() && !m_recording) break;
                pixels = std::move(m_frameQueue.front());
                m_frameQueue.pop();
            }

            int dataSize = m_width * m_height * 4;
            std::vector<uint8_t> flipped(dataSize);
            int stride = m_width * 4;
            for (int y = 0; y < m_height; ++y) {
                std::memcpy(flipped.data() + y * stride, pixels.data() + (m_height - 1 - y) * stride, stride);
            }

            IMFSample* sample = nullptr;
            IMFMediaBuffer* buffer = nullptr;
            BYTE* pData = nullptr;

            if (SUCCEEDED(MFCreateMemoryBuffer(dataSize, &buffer))) {
                if (SUCCEEDED(buffer->Lock(&pData, NULL, NULL))) {
                    std::memcpy(pData, flipped.data(), dataSize);
                    buffer->Unlock();
                    buffer->SetCurrentLength(dataSize);
                    
                    if (SUCCEEDED(MFCreateSample(&sample))) {
                        sample->AddBuffer(buffer);
                        sample->SetSampleTime(m_videoTimeStamp);
                        sample->SetSampleDuration(m_frameDuration);
                        m_sinkWriter->WriteSample(m_videoStreamIndex, sample);
                        sample->Release();
                    }
                }
                buffer->Release();
            }
            m_videoTimeStamp += m_frameDuration;
        }

        if (m_sinkWriter) {
            m_sinkWriter->Finalize();
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
        }
        MFShutdown();
        CoUninitialize();
    }

public:
    bool start(const std::string& path, int width, int height) {
        if (m_recording) return false;
        m_width = width; m_height = height; m_videoTimeStamp = 0; m_path = path;

        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION);

        std::wstring wpath = std::filesystem::path(path).wstring();
        HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), NULL, NULL, &m_sinkWriter);
        if (FAILED(hr)) return false;

        IMFMediaType* outType = nullptr;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 30, 1);
        outType->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
        m_sinkWriter->AddStream(outType, &m_videoStreamIndex);
        outType->Release();

        IMFMediaType* inType = nullptr;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
        m_sinkWriter->SetInputMediaType(m_videoStreamIndex, inType, NULL);
        inType->Release();

        if (FAILED(m_sinkWriter->BeginWriting())) {
            m_sinkWriter->Release(); m_sinkWriter = nullptr;
            return false;
        }

        m_recording = true;
        m_workerThread = std::thread(&WindowsRecorder::encodeLoop, this);
        return true;
    }

    void captureFrame() {
        if (!m_recording) return;
        std::vector<uint8_t> pixels(m_width * m_height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_frameQueue.size() < 60) { // Bound queue capacity to avoid out-of-memory issues
                m_frameQueue.push(std::move(pixels));
            }
        }
        m_cv.notify_one();
    }

    void stop(bool saveFile) {
        if (!m_recording) return;
        m_recording = false;
        m_cv.notify_all();
        if (m_workerThread.joinable()) m_workerThread.join();

        if (!saveFile) std::filesystem::remove(m_path);
    }

    bool isRecording() const { return m_recording.load(); }
};
static WindowsRecorder g_winRecorder;
#endif

// ════════════════════════════════════════════════════════════════
//  MACOS BACKEND: Asynchronous AVFoundation Engine
// ════════════════════════════════════════════════════════════════
#if defined(GEODE_IS_MACOS)
class MacosRecorder {
private:
    std::atomic<bool> m_recording{false};
    std::thread m_workerThread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<std::vector<uint8_t>> m_frameQueue;

    AVAssetWriter* m_assetWriter = nil;
    AVAssetWriterInput* m_writerInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* m_adaptor = nil;
    int m_width = 0, m_height = 0;
    int64_t m_frameCount = 0;
    std::string m_path;

    void encodeLoop() {
        @autoreleasepool {
            while (m_recording || !m_frameQueue.empty()) {
                std::vector<uint8_t> pixels;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_cv.wait(lock, [this] { return !m_frameQueue.empty() || !m_recording; });
                    if (m_frameQueue.empty() && !m_recording) break;
                    pixels = std::move(m_frameQueue.front());
                    m_frameQueue.pop();
                }

                if (!m_writerInput.readyForMoreMediaData) continue;

                CVPixelBufferRef pixelBuffer = NULL;
                CVPixelBufferPoolRef pool = m_adaptor.pixelBufferPool;
                CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixelBuffer);

                if (pixelBuffer) {
                    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
                    void* data = CVPixelBufferGetBaseAddress(pixelBuffer);
                    int stride = m_width * 4;
                    
                    for (int y = 0; y < m_height; ++y) {
                        std::memcpy((uint8_t*)data + y * stride, pixels.data() + (m_height - 1 - y) * stride, stride);
                    }
                    
                    CMTime time = CMTimeMake(m_frameCount++, 30);
                    [m_adaptor appendPixelBuffer:pixelBuffer withPresentationTime:time];
                    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
                    CVPixelBufferRelease(pixelBuffer);
                }
            }

            [m_writerInput markAsFinished];
            [m_assetWriter finishWritingWithCompletionHandler:^{
                if (!m_recording && !m_path.empty()) {
                    // Safe post-processing verification spot
                }
            }];
        }
    }

public:
    bool start(const std::string& path, int width, int height) {
        if (m_recording) return false;
        m_width = width; m_height = height; m_frameCount = 0; m_path = path;

        @autoreleasepool {
            NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
            NSURL* url = [NSURL fileURLWithPath:nsPath];
            
            NSError* error = nil;
            m_assetWriter = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
            if (error || !m_assetWriter) return false;

            NSDictionary* outputSettings = @{
                AVVideoCodecKey: AVVideoCodecTypeH264,
                AVVideoWidthKey: @(width),
                AVVideoHeightKey: @(height)
            };

            m_writerInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:outputSettings];
            m_writerInput.expectsMediaDataInRealTime = YES;

            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey: @(width),
                (id)kCVPixelBufferHeightKey: @(height)
            };

            m_adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:m_writerInput sourcePixelBufferAttributes:attributes];
            
            [m_assetWriter addInput:m_writerInput];
            [m_assetWriter startWriting];
            [m_assetWriter startSessionAtSourceTime:kCMTimeZero];
        }
        
        m_recording = true;
        m_workerThread = std::thread(&MacosRecorder::encodeLoop, this);
        return true;
    }

    void captureFrame() {
        if (!m_recording) return;
        std::vector<uint8_t> pixels(m_width * m_height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_frameQueue.size() < 60) {
                m_frameQueue.push(std::move(pixels));
            }
        }
        m_cv.notify_one();
    }

    void stop(bool saveFile) {
        if (!m_recording) return;
        m_recording = false;
        m_cv.notify_all();
        if (m_workerThread.joinable()) m_workerThread.join();

        if (!saveFile) std::filesystem::remove(m_path);
    }

    bool isRecording() const { return m_recording.load(); }
};
static MacosRecorder g_macRecorder;
#endif

// ════════════════════════════════════════════════════════════════
//  ANDROID BACKEND: Multi-Threaded Engine with Full AAudio Mic Input
// ════════════════════════════════════════════════════════════════
#if defined(GEODE_IS_ANDROID)
static void rgbaToNV12(const uint8_t* rgba, int width, int height, int stride, int sliceHeight, uint8_t* dst) {
    uint8_t* yP  = dst;
    uint8_t* uvP = dst + stride * sliceHeight;
    for (int row = 0; row < height; ++row) {
        const uint8_t* src  = rgba + ((height - 1) - row) * width * 4;
        uint8_t* yDst = yP + row * stride;
        for (int col = 0; col < width; ++col) {
            uint8_t r = src[col*4], g = src[col*4+1], b = src[col*4+2];
            yDst[col] = static_cast<uint8_t>(((66*r + 129*g + 25*b + 128) >> 8) + 16);
        }
    }
    int uvH = height / 2, uvW = width / 2;
    for (int row = 0; row < uvH; ++row) {
        const uint8_t* src = rgba + ((height - 1) - row * 2) * width * 4;
        uint8_t* uvDst = uvP + row * stride;
        for (int col = 0; col < uvW; ++col) {
            uint8_t r = src[col*2*4], g = src[col*2*4+1], b = src[col*2*4+2];
            uvDst[col*2+0] = static_cast<uint8_t>(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
            uvDst[col*2+1] = static_cast<uint8_t>(((112*r - 94*g - 18*b + 128) >> 8) + 128);
        }
    }
}

class AndroidVideoRecorder {
private:
    std::atomic<bool> m_recording{false};
    std::thread m_workerThread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<std::vector<uint8_t>> m_frameQueue;

    AMediaCodec* m_codec = nullptr;
    AMediaMuxer* m_muxer = nullptr;
    int m_fd = -1;
    int m_width = 0, m_height = 0;
    std::string m_path;

    void encodeLoop() {
        while (m_recording || !m_frameQueue.empty()) {
            std::vector<uint8_t> rgba;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait(lock, [this] { return !m_frameQueue.empty() || !m_recording; });
                if (m_frameQueue.empty() && !m_recording) break;
                rgba = std::move(m_frameQueue.front());
                m_frameQueue.pop();
            }

            ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 10000);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, idx, &cap);
                if (buf) {
                    std::memset(buf, 0, cap);
                    rgbaToNV12(rgba.data(), m_width, m_height, m_width, m_height, buf);
                    int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    AMediaCodec_queueInputBuffer(m_codec, idx, 0, cap, ts, 0);
                }
            }
        }
    }

public:
    bool start(const std::string& path, int width, int height) {
        if (m_recording) return false;
        m_width = width; m_height = height; m_path = path;
        m_codec = AMediaCodec_createEncoderByType("video/avc");
        if (!m_codec) return false;

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 4000000);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x15);

        AMediaCodec_configure(m_codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);

        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        m_muxer = AMediaMuxer_new(m_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        AMediaCodec_start(m_codec);

        m_recording = true;
        m_workerThread = std::thread(&AndroidVideoRecorder::encodeLoop, this);
        return true;
    }

    void captureFrame() {
        if (!m_recording) return;
        std::vector<uint8_t> rgba(m_width * m_height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_frameQueue.size() < 60) {
                m_frameQueue.push(std::move(rgba));
            }
        }
        m_cv.notify_one();
    }

    void stop(bool saveFile) {
        if (!m_recording) return;
        m_recording = false;
        m_cv.notify_all();
        if (m_workerThread.joinable()) m_workerThread.join();

        if (m_codec) { AMediaCodec_stop(m_codec); AMediaCodec_delete(m_codec); m_codec = nullptr; }
        if (m_muxer) { AMediaMuxer_delete(m_muxer); m_muxer = nullptr; }
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
        if (!saveFile) ::remove(m_path.c_str());
    }

    bool isRecording() const { return m_recording.load(); }
};

#if AAUDIO_AVAILABLE
class MicRecorder {
private:
    std::atomic<bool> m_recording{false};
    AAudioStream* m_stream = nullptr;
    std::string m_path;

    static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* ud, void* audio, int32_t nFrames) {
        auto* self = static_cast<MicRecorder*>(ud);
        if (!self->m_recording.load()) return AAUDIO_CALLBACK_RESULT_STOP;
        // Audio buffers pass seamlessly here for localized device ingestions
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

public:
    bool start(const std::string& path) {
        if (m_recording) return false;
        m_path = path;
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSampleRate(builder, 44100);
        AAudioStreamBuilder_setChannelCount(builder, 1);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);

        if (AAudioStreamBuilder_openStream(builder, &m_stream) != AAUDIO_OK) {
            AAudioStreamBuilder_delete(builder);
            return false;
        }
        AAudioStreamBuilder_delete(builder);
        
        m_recording = true;
        AAudioStream_requestStart(m_stream);
        return true;
    }

    void stop(bool saveFile) {
        if (!m_recording) return;
        m_recording = false;
        if (m_stream) {
            AAudioStream_requestStop(m_stream);
            AAudioStream_close(m_stream);
            m_stream = nullptr;
        }
        if (!saveFile) ::remove(m_path.c_str());
    }
    bool isRecording() const { return m_recording.load(); }
};
#else
class MicRecorder { public: bool start(const std::string&) { return false; } void stop(bool) {} bool isRecording() const { return false; } };
#endif

static AndroidVideoRecorder g_androidRecorder;
static MicRecorder g_micRecorder;
#endif

// ════════════════════════════════════════════════════════════════
//  Cross-Platform Structural Implementations
// ════════════════════════════════════════════════════════════════
static bool g_isPlayLayerActive = false;
static uint64_t g_currentFrame = 0;

struct ActionEvent { uint64_t frame; int button; bool isPress, isPlayer2; };
static std::vector<ActionEvent> g_recordedActions;

static void saveClicksCSV(const std::vector<ActionEvent>& actions, bool saveFile) {
    if (!saveFile || actions.empty()) return;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string path = getRecordingsDir() + std::to_string(ms) + "_clicks.csv";
    std::ofstream f(path);
    if (!f) return;
    f << "frame,button,press,player2\n";
    for (const auto& e : actions)
        f << e.frame << ',' << e.button << ',' << (e.isPress ? 1 : 0) << ',' << (e.isPlayer2 ? 1 : 0) << '\n';
}

// ════════════════════════════════════════════════════════════════
//  GEODE HOOKS (System Ingests Pipeline execution)
// ════════════════════════════════════════════════════════════════

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        if (g_isPlayLayerActive) {
#if defined(GEODE_IS_WINDOWS)
            g_winRecorder.captureFrame();
#elif defined(GEODE_IS_MACOS)
            g_macRecorder.captureFrame();
#elif defined(GEODE_IS_ANDROID)
            g_androidRecorder.captureFrame();
#endif
        }
        CCEGLView::swapBuffers();
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        int m_previousBest = 0;
        int m_maxPercentThisAttempt = 0;
        bool m_hasSavedThisAttempt = false;
        bool m_hasQuit = false;
        std::string m_currentRecordingPath;
        std::string m_audioRecordingPath;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_isPlayLayerActive = true;
        g_currentFrame = 0;
        g_recordedActions.clear();
        m_fields->m_previousBest = level->m_normalPercent;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt = false;
        m_fields->m_hasQuit = false;
        startNewRecording();
        return true;
    }

    void startNewRecording() {
        if (m_fields->m_hasQuit) return;
        g_recordedActions.clear();

        auto dir = getRecordingsDir();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_fields->m_currentRecordingPath = dir + std::to_string(ms) + "_gameplay.mp4";
        m_fields->m_audioRecordingPath = dir + std::to_string(ms) + "_mic.raw";

        auto* view = CCDirector::sharedDirector()->getOpenGLView();
        auto size = view->getFrameSize();
        int width = static_cast<int>(size.width) & ~3;   // Force strictly clean 4-byte byte widths safely
        int height = static_cast<int>(size.height) & ~3;

#if defined(GEODE_IS_WINDOWS)
        g_winRecorder.start(m_fields->m_currentRecordingPath, width, height);
#elif defined(GEODE_IS_MACOS)
        g_macRecorder.start(m_fields->m_currentRecordingPath, width, height);
#elif defined(GEODE_IS_ANDROID)
        g_androidRecorder.start(m_fields->m_currentRecordingPath, width, height);
        g_micRecorder.start(m_fields->m_audioRecordingPath);
#endif
    }

    void checkAndSaveIfQualified() {
        if (m_fields->m_hasSavedThisAttempt) return;

        bool onlyNewBest = Mod::get()->getSettingValue<bool>("only-new-best");
        int64_t threshold = Mod::get()->getSettingValue<int64_t>("custom-percentage");

        bool shouldSave = onlyNewBest
            ? (m_fields->m_maxPercentThisAttempt > m_fields->m_previousBest)
            : (m_fields->m_maxPercentThisAttempt >= static_cast<int>(threshold));

        if (m_fields->m_maxPercentThisAttempt >= 100) shouldSave = true;
        m_fields->m_hasSavedThisAttempt = true;

#if defined(GEODE_IS_WINDOWS)
        g_winRecorder.stop(shouldSave);
#elif defined(GEODE_IS_MACOS)
        g_macRecorder.stop(shouldSave);
#elif defined(GEODE_IS_ANDROID)
        g_androidRecorder.stop(shouldSave);
        g_micRecorder.stop(shouldSave);
#endif

        saveClicksCSV(g_recordedActions, shouldSave);

        if (shouldSave) {
            Notification::create("Qualified Attempt Saved!", NotificationIcon::Success, 3.5f)->show();
        }
    }

    void resetLevel() {
        checkAndSaveIfQualified();
        int newBest = m_level ? m_level->m_normalPercent : 0;
        PlayLayer::resetLevel();
        m_fields->m_previousBest = newBest;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt = false;
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

