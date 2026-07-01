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
#include <future>

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
#elif defined(__APPLE__)
    #undef CommentType
    #define CommentType AppleCommentType
    #import <AVFoundation/AVFoundation.h>
    #import <CoreMedia/CoreMedia.h>
    #import <CoreVideo/CoreVideo.h>
    #undef CommentType
    #define CommentType CommentTypeDummy
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
//  Cross-Platform Sandbox-Safe Path Resolver
// ════════════════════════════════════════════════════════════════
static std::string getRecordingsDir() {
    auto saveDir = geode::Mod::get()->getSaveDir();
    auto recordingsDir = saveDir / "Recordings";

    std::error_code ec;
    std::filesystem::create_directories(recordingsDir, ec);
    if (ec) {
        geode::log::error("Recorder: failed to create recordings dir: {}", ec.message());
    }

    std::string dirStr = recordingsDir.string();
    if (!dirStr.empty() && dirStr.back() != '/' && dirStr.back() != '\\') {
#if defined(GEODE_IS_WINDOWS)
        dirStr += "\\";
#else
        dirStr += "/";
#endif
    }
    return dirStr;
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

    // NOTE: This used to be called from the *calling* thread while
    // WriteSample()/Finalize() were called from the worker thread.
    // IMFSinkWriter objects created under COINIT_APARTMENTTHREADED are
    // apartment-affine; calling them from a different thread/apartment
    // than the one that created them silently fails every call (HRESULTs
    // were never checked), which is why the resulting .mp4 came out as
    // an empty/near-empty container. Fix: create + use the sink writer
    // entirely on the worker thread, under COINIT_MULTITHREADED, and
    // check every HRESULT.
    bool initSinkWriter() {
        std::wstring wpath = std::filesystem::path(m_path).wstring();
        HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), NULL, NULL, &m_sinkWriter);
        if (FAILED(hr)) {
            geode::log::error("Recorder: MFCreateSinkWriterFromURL failed (0x{:08x})", static_cast<unsigned>(hr));
            return false;
        }

        IMFMediaType* outType = nullptr;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 30, 1);
        outType->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
        hr = m_sinkWriter->AddStream(outType, &m_videoStreamIndex);
        outType->Release();
        if (FAILED(hr)) {
            geode::log::error("Recorder: AddStream failed (0x{:08x})", static_cast<unsigned>(hr));
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
            return false;
        }

        IMFMediaType* inType = nullptr;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, m_width, m_height);
        hr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, inType, NULL);
        inType->Release();
        if (FAILED(hr)) {
            geode::log::error("Recorder: SetInputMediaType failed (0x{:08x})", static_cast<unsigned>(hr));
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
            return false;
        }

        hr = m_sinkWriter->BeginWriting();
        if (FAILED(hr)) {
            geode::log::error("Recorder: BeginWriting failed (0x{:08x})", static_cast<unsigned>(hr));
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
            return false;
        }

        return true;
    }

    void encodeLoop(std::promise<bool> readyPromise) {
        HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        bool comInited = SUCCEEDED(coHr) || coHr == S_FALSE;
        MFStartup(MF_VERSION);

        bool ok = initSinkWriter();
        readyPromise.set_value(ok);

        if (!ok) {
            MFShutdown();
            if (comInited) CoUninitialize();
            return;
        }

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

            HRESULT hr = MFCreateMemoryBuffer(dataSize, &buffer);
            if (SUCCEEDED(hr)) {
                hr = buffer->Lock(&pData, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    std::memcpy(pData, flipped.data(), dataSize);
                    buffer->Unlock();
                    buffer->SetCurrentLength(dataSize);

                    hr = MFCreateSample(&sample);
                    if (SUCCEEDED(hr)) {
                        sample->AddBuffer(buffer);
                        sample->SetSampleTime(m_videoTimeStamp);
                        sample->SetSampleDuration(m_frameDuration);
                        hr = m_sinkWriter->WriteSample(m_videoStreamIndex, sample);
                        if (FAILED(hr)) {
                            geode::log::error("Recorder: WriteSample failed (0x{:08x})", static_cast<unsigned>(hr));
                        }
                        sample->Release();
                    }
                } else {
                    geode::log::error("Recorder: buffer->Lock failed (0x{:08x})", static_cast<unsigned>(hr));
                }
                buffer->Release();
            } else {
                geode::log::error("Recorder: MFCreateMemoryBuffer failed (0x{:08x})", static_cast<unsigned>(hr));
            }
            m_videoTimeStamp += m_frameDuration;
        }

        if (m_sinkWriter) {
            HRESULT hr = m_sinkWriter->Finalize();
            if (FAILED(hr)) {
                geode::log::error("Recorder: Finalize failed (0x{:08x})", static_cast<unsigned>(hr));
            }
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
        }
        MFShutdown();
        if (comInited) CoUninitialize();
    }

public:
    bool start(const std::string& path, int width, int height) {
        if (m_recording) return false;
        m_width = width; m_height = height; m_videoTimeStamp = 0; m_path = path;

        std::promise<bool> readyPromise;
        std::future<bool> readyFuture = readyPromise.get_future();

        m_recording = true;
        m_workerThread = std::thread(&WindowsRecorder::encodeLoop, this, std::move(readyPromise));

        // Block until the sink writer is actually created & BeginWriting()
        // has succeeded on the owning (worker) thread, so callers get a
        // real success/failure result instead of an optimistic true.
        bool ok = readyFuture.get();
        if (!ok) {
            m_recording = false;
            m_cv.notify_all();
            if (m_workerThread.joinable()) m_workerThread.join();
            return false;
        }
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
static WindowsRecorder g_winRecorder;
#endif

// ════════════════════════════════════════════════════════════════
//  APPLE BACKEND (macOS & iOS): Asynchronous AVFoundation Engine
// ════════════════════════════════════════════════════════════════
#if defined(__APPLE__)
class AppleRecorder {
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

                if (!m_writerInput.readyForMoreMediaData) {
                    geode::log::warn("Recorder: writer input not ready, dropping frame");
                    continue;
                }

                CVPixelBufferRef pixelBuffer = NULL;
                CVPixelBufferPoolRef pool = m_adaptor.pixelBufferPool;
                CVReturn cvr = CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixelBuffer);

                if (cvr == kCVReturnSuccess && pixelBuffer) {
                    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
                    void* data = CVPixelBufferGetBaseAddress(pixelBuffer);
                    int stride = m_width * 4;

                    for (int y = 0; y < m_height; ++y) {
                        std::memcpy((uint8_t*)data + y * stride, pixels.data() + (m_height - 1 - y) * stride, stride);
                    }

                    CMTime time = CMTimeMake(m_frameCount++, 30);
                    BOOL appended = [m_adaptor appendPixelBuffer:pixelBuffer withPresentationTime:time];
                    if (!appended) {
                        geode::log::error("Recorder: appendPixelBuffer failed: {}",
                            m_assetWriter.error ? m_assetWriter.error.localizedDescription.UTF8String : "unknown error");
                    }
                    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
                    CVPixelBufferRelease(pixelBuffer);
                } else {
                    geode::log::error("Recorder: CVPixelBufferPoolCreatePixelBuffer failed ({})", (int)cvr);
                }
            }

            // NOTE: finishWritingWithCompletionHandler is asynchronous.
            // The previous empty completion handler meant nothing ever
            // waited for it, so stop()/the worker thread could return
            // before the file was actually flushed to disk - a race that
            // can easily produce a truncated or empty .mp4. Block on a
            // semaphore until finalize genuinely completes.
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [m_writerInput markAsFinished];
            [m_assetWriter finishWritingWithCompletionHandler:^{
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

            if (m_assetWriter.status == AVAssetWriterStatusFailed) {
                geode::log::error("Recorder: asset writer failed: {}",
                    m_assetWriter.error ? m_assetWriter.error.localizedDescription.UTF8String : "unknown error");
            }
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
            if (error || !m_assetWriter) {
                geode::log::error("Recorder: AVAssetWriter init failed: {}",
                    error ? error.localizedDescription.UTF8String : "unknown error");
                return false;
            }

            NSDictionary* outputSettings = @{
                AVVideoCodecKey: AVVideoCodecTypeH264,
                AVVideoWidthKey: @(width),
                AVVideoHeightKey: @(height)
            };

            m_writerInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:outputSettings];
            m_writerInput.expectsMediaDataInRealTime = YES;

            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32RGBA),
                (id)kCVPixelBufferWidthKey: @(width),
                (id)kCVPixelBufferHeightKey: @(height)
            };

            m_adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:m_writerInput sourcePixelBufferAttributes:attributes];

            if (![m_assetWriter canAddInput:m_writerInput]) {
                geode::log::error("Recorder: cannot add writer input");
                return false;
            }
            [m_assetWriter addInput:m_writerInput];

            if (![m_assetWriter startWriting]) {
                geode::log::error("Recorder: startWriting failed: {}",
                    m_assetWriter.error ? m_assetWriter.error.localizedDescription.UTF8String : "unknown error");
                return false;
            }
            [m_assetWriter startSessionAtSourceTime:kCMTimeZero];
        }

        m_recording = true;
        m_workerThread = std::thread(&AppleRecorder::encodeLoop, this);
        return true;
    }

    void captureFrame() {
        if (!m_recording) return;
        std::vector<uint8_t> pixels(m_width * m_height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

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
static AppleRecorder g_appleRecorder;
#endif

// ════════════════════════════════════════════════════════════════
//  ANDROID BACKEND: Multi-Threaded Engine
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
    int m_inputStride = 0, m_inputSliceHeight = 0; // actual layout the codec wants
    int64_t m_frameCount = 0;
    std::string m_path;

    void encodeLoop() {
        bool muxerStarted = false;
        ssize_t trackIdx = -1;
        AMediaCodecBufferInfo info;

        while (m_recording || !m_frameQueue.empty()) {
            // 1. INPUT PROCESSING
            std::vector<uint8_t> rgba;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return !m_frameQueue.empty() || !m_recording;
                });
                if (!m_frameQueue.empty()) {
                    rgba = std::move(m_frameQueue.front());
                    m_frameQueue.pop();
                }
            }

            if (!rgba.empty()) {
                ssize_t inIdx = AMediaCodec_dequeueInputBuffer(m_codec, 2000);
                if (inIdx >= 0) {
                    size_t cap = 0;
                    uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, inIdx, &cap);
                    // Required size uses the codec's own stride/slice-height, not
                    // a tightly-packed assumption - many real hardware encoders
                    // pad these beyond width/height, and writing/queuing a
                    // tightly-packed buffer against that layout causes every
                    // frame to be silently rejected (no output ever produced,
                    // muxer track never starts, file ends up empty).
                    size_t frameSize = static_cast<size_t>(m_inputStride) * m_inputSliceHeight * 3 / 2;
                    if (buf && cap >= frameSize) {
                        rgbaToNV12(rgba.data(), m_width, m_height, m_inputStride, m_inputSliceHeight, buf);
                        int64_t ts = m_frameCount++ * 33333;
                        media_status_t st = AMediaCodec_queueInputBuffer(m_codec, inIdx, 0, frameSize, ts, 0);
                        if (st != AMEDIA_OK) {
                            geode::log::error("Recorder: queueInputBuffer failed ({})", (int)st);
                        }
                    } else {
                        geode::log::error("Recorder: input buffer too small (cap={}, need={}) - discarding frame, encoder layout mismatch", cap, frameSize);
                        AMediaCodec_queueInputBuffer(m_codec, inIdx, 0, 0, 0, 0);
                    }
                }
            }

            // 2. OUTPUT PROCESSING
            while (true) {
                ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 2000);
                if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    AMediaFormat* format = AMediaCodec_getOutputFormat(m_codec);
                    trackIdx = AMediaMuxer_addTrack(m_muxer, format);
                    if (trackIdx < 0) {
                        geode::log::error("Recorder: AMediaMuxer_addTrack failed ({})", (int)trackIdx);
                    } else {
                        media_status_t st = AMediaMuxer_start(m_muxer);
                        if (st != AMEDIA_OK) {
                            geode::log::error("Recorder: AMediaMuxer_start failed ({})", (int)st);
                        } else {
                            muxerStarted = true;
                        }
                    }
                    AMediaFormat_delete(format);
                } else if (outIdx >= 0) {
                    size_t cap = 0;
                    uint8_t* buf = AMediaCodec_getOutputBuffer(m_codec, outIdx, &cap);
                    if (buf && muxerStarted && info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        AMediaMuxer_writeSampleData(m_muxer, trackIdx, buf, &info);
                    }
                    AMediaCodec_releaseOutputBuffer(m_codec, outIdx, false);
                    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }

        // 3. DRAIN LOGIC: Lock loop until encoder processes the EOS flag
        while (true) {
            ssize_t inIdx = AMediaCodec_dequeueInputBuffer(m_codec, 10000);
            if (inIdx >= 0) {
                AMediaCodec_queueInputBuffer(m_codec, inIdx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                break;
            }
        }

        bool sawEOS = false;
        while (!sawEOS) {
            ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 50000);
            if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* format = AMediaCodec_getOutputFormat(m_codec);
                trackIdx = AMediaMuxer_addTrack(m_muxer, format);
                if (trackIdx >= 0 && AMediaMuxer_start(m_muxer) == AMEDIA_OK) {
                    muxerStarted = true;
                }
                AMediaFormat_delete(format);
            } else if (outIdx >= 0) {
                if (muxerStarted && info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                    size_t cap = 0;
                    uint8_t* buf = AMediaCodec_getOutputBuffer(m_codec, outIdx, &cap);
                    AMediaMuxer_writeSampleData(m_muxer, trackIdx, buf, &info);
                }
                AMediaCodec_releaseOutputBuffer(m_codec, outIdx, false);
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                    sawEOS = true;
                }
            } else {
                break;
            }
        }

        if (!muxerStarted) {
            geode::log::error("Recorder: muxer never started, output file will be empty");
        } else {
            AMediaMuxer_stop(m_muxer);
        }
    }

public:
    bool start(const std::string& path, int width, int height) {
        if (m_recording) return false;
        m_width = width; m_height = height; m_path = path; m_frameCount = 0;
        m_codec = AMediaCodec_createEncoderByType("video/avc");
        if (!m_codec) {
            geode::log::error("Recorder: createEncoderByType failed");
            return false;
        }

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 4000000);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x15); // NV12

        media_status_t status = AMediaCodec_configure(m_codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (status != AMEDIA_OK) {
            geode::log::error("Recorder: AMediaCodec_configure failed ({})", (int)status);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            geode::log::error("Recorder: failed to open output file (errno {})", errno);
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        m_muxer = AMediaMuxer_new(m_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!m_muxer) {
            geode::log::error("Recorder: AMediaMuxer_new failed");
            ::close(m_fd); m_fd = -1;
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        status = AMediaCodec_start(m_codec);
        if (status != AMEDIA_OK) {
            geode::log::error("Recorder: AMediaCodec_start failed ({})", (int)status);
            AMediaMuxer_delete(m_muxer); m_muxer = nullptr;
            ::close(m_fd); m_fd = -1;
            AMediaCodec_delete(m_codec); m_codec = nullptr;
            return false;
        }

        // Default to tightly-packed, then override with whatever the codec
        // actually reports for its input buffer layout - hardware encoders
        // very often pad stride/slice-height beyond width/height.
        m_inputStride = width;
        m_inputSliceHeight = height;
        AMediaFormat* inFmt = AMediaCodec_getInputFormat(m_codec);
        if (inFmt) {
            int32_t stride = 0, sliceHeight = 0;
            if (AMediaFormat_getInt32(inFmt, AMEDIAFORMAT_KEY_STRIDE, &stride) && stride > 0) {
                m_inputStride = stride;
            }
            if (AMediaFormat_getInt32(inFmt, "slice-height", &sliceHeight) && sliceHeight > 0) {
                m_inputSliceHeight = sliceHeight;
            }
            geode::log::debug("Recorder: encoder input layout stride={} sliceHeight={} (frame {}x{})",
                m_inputStride, m_inputSliceHeight, width, height);
            AMediaFormat_delete(inFmt);
        }

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

        aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &m_stream);
        AAudioStreamBuilder_delete(builder);
        if (res != AAUDIO_OK) {
            geode::log::error("Recorder: AAudio openStream failed ({})", (int)res);
            return false;
        }

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
    if (!f) {
        geode::log::error("Recorder: failed to open clicks CSV at {}", path);
        return;
    }
    f << "frame,button,press,player2\n";
    for (const auto& e : actions)
        f << e.frame << ',' << e.button << ',' << (e.isPress ? 1 : 0) << ',' << (e.isPlayer2 ? 1 : 0) << '\n';
}

// ════════════════════════════════════════════════════════════════
//  GEODE HOOKS
// ════════════════════════════════════════════════════════════════

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        if (g_isPlayLayerActive) {
#if defined(GEODE_IS_WINDOWS)
            g_winRecorder.captureFrame();
#elif defined(__APPLE__)
            g_appleRecorder.captureFrame();
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

        // 16-macroblock boundary alignment (required by most H.264 encoders)
        int width = static_cast<int>(size.width) & ~15;
        int height = static_cast<int>(size.height) & ~15;

        bool started = false;
#if defined(GEODE_IS_WINDOWS)
        started = g_winRecorder.start(m_fields->m_currentRecordingPath, width, height);
#elif defined(__APPLE__)
        started = g_appleRecorder.start(m_fields->m_currentRecordingPath, width, height);
#elif defined(GEODE_IS_ANDROID)
        started = g_androidRecorder.start(m_fields->m_currentRecordingPath, width, height);
        g_micRecorder.start(m_fields->m_audioRecordingPath);
#endif
        if (!started) {
            geode::log::error("Recorder: failed to start recording at {}", m_fields->m_currentRecordingPath);
        }
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
#elif defined(__APPLE__)
        g_appleRecorder.stop(shouldSave);
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

