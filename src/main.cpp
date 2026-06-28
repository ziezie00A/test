#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/cocos.hpp>

// Include cross-platform FMOD headers packaged with Geode
#include <fmod.hpp>

// Include FFmpeg API headers provided by eclipse.ffmpeg-api
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <filesystem>

using namespace geode::prelude;

// ════════════════════════════════════════════════════════════════
//  Universal Path Resolution
// ════════════════════════════════════════════════════════════════
static std::string getRecordingsDir() {
    std::string dir;
#ifdef GEODE_IS_ANDROID
    dir = "/storage/emulated/0/MacroSafeguard/";
#elif defined(_WIN32)
    char* appdata = getenv("APPDATA");
    if (appdata) {
        dir = std::string(appdata) + "\\MacroSafeguard\\";
    } else {
        dir = std::string(getenv("USERPROFILE")) + "\\Documents\\MacroSafeguard\\";
    }
#else
    char* home = getenv("HOME");
    if (home) {
        dir = std::string(home) + "/.local/share/MacroSafeguard/";
    } else {
        dir = "/tmp/MacroSafeguard/";
    }
#endif

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ════════════════════════════════════════════════════════════════
//  Unified Cross-Platform Media Recorder
// ════════════════════════════════════════════════════════════════
class UniversalRecorder {
public:
    static UniversalRecorder& get() {
        static UniversalRecorder instance;
        return instance;
    }

    bool isRecording() const { return m_isRecording.load(); }

    bool start(const std::string& filename, int width, int height, int sampleRate) {
        if (m_isRecording.load()) return false;

        m_width = width & ~1; // Ensure dimensions are divisible by 2 for H.264
        m_height = height & ~1;
        m_sampleRate = sampleRate;
        m_outputPath = getRecordingsDir() + filename + ".mp4";
        
        m_videoFrameCount = 0;
        m_audioSampleCount = 0;
        m_isRecording.store(true);
        m_stopEncoder.store(false);

        // Clear existing stale data in queues securely
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<std::vector<uint8_t>>().swap(m_videoQueue);
            std::queue<std::vector<float>>().swap(m_audioQueue);
        }

        m_encoderThread = std::thread(&UniversalRecorder::encoderLoop, this);
        log::info("[MacroSafeguard] Recording pipeline initialized successfully.");
        return true;
    }

    void pushVideoFrame(const uint8_t* rgbaData, size_t size) {
        if (!m_isRecording.load()) return;

        std::lock_guard<std::mutex> lock(m_queueMutex);
        // BUG MITIGATION: Memory protection throttle. Drop frames if encoding lag exceeds 30 frames.
        if (m_videoQueue.size() > 30) {
            return; 
        }
        m_videoQueue.push(std::vector<uint8_t>(rgbaData, rgbaData + size));
        m_queueCv.notify_one();
    }

    void pushAudioSamples(const float* PCMData, size_t numSamples) {
        if (!m_isRecording.load()) return;

        std::lock_guard<std::mutex> lock(m_queueMutex);
        // Audio streams cannot afford frame drops without severe crackling.
        // Memory consumption here is minimal compared to video frames.
        m_audioQueue.push(std::vector<float>(PCMData, PCMData + numSamples));
        m_queueCv.notify_one();
    }

    void stop(bool saveFile) {
        if (!m_isRecording.load()) return;
        m_isRecording.store(false);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stopEncoder.store(true);
        }
        m_queueCv.notify_all();

        if (m_encoderThread.joinable()) {
            m_encoderThread.join();
        }

        if (!saveFile) {
            std::error_code ec;
            std::filesystem::remove(m_outputPath, ec);
            log::info("[MacroSafeguard] Recording discarded safely.");
        } else {
            log::info("[MacroSafeguard] Recording successfully finalized at: {}", m_outputPath);
        }
    }

private:
    UniversalRecorder() {}
    
    std::atomic<bool> m_isRecording{false};
    std::atomic<bool> m_stopEncoder{false};
    std::thread       m_encoderThread;
    std::mutex        m_queueMutex;
    std::condition_variable m_queueCv;

    std::queue<std::vector<uint8_t>> m_videoQueue;
    std::queue<std::vector<float>>   m_audioQueue;

    std::string m_outputPath;
    int m_width = 0;
    int m_height = 0;
    int m_sampleRate = 44100;
    
    int64_t m_videoFrameCount = 0;
    int64_t m_audioSampleCount = 0;

    void encoderLoop() {
        // Initialize FFmpeg contexts natively across Windows, Mac, and Android
        AVFormatContext* fmtCtx = nullptr;
        avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, m_outputPath.c_str());
        if (!fmtCtx) return;

        // Configure H.264 Video Stream
        const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
        AVStream* videoStream = avformat_new_stream(fmtCtx, videoCodec);
        AVCodecContext* videoEncCtx = avcodec_alloc_context3(videoCodec);
        
        videoEncCtx->width = m_width;
        videoEncCtx->height = m_height;
        videoEncCtx->time_base = {1, 60}; // Target fixed engine 60fps tracking
        videoEncCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        videoEncCtx->bit_rate = 4000000;
        videoEncCtx->gop_size = 12;
        avcodec_open2(videoEncCtx, videoCodec, nullptr);
        avcodec_parameters_from_context(videoStream->codecpar, videoEncCtx);

        // Configure AAC Audio Stream
        const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        AVStream* audioStream = avformat_new_stream(fmtCtx, audioCodec);
        AVCodecContext* audioEncCtx = avcodec_alloc_context3(audioCodec);
        
        audioEncCtx->sample_rate = m_sampleRate;
        audioEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Float Planar natively matched to FMOD
        audioEncCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        audioEncCtx->bit_rate = 128000;
        avcodec_open2(audioEncCtx, audioCodec, nullptr);
        avcodec_parameters_from_context(audioStream->codecpar, audioEncCtx);

        // Open Output File System Interfacing
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_open(&fmtCtx->pb, m_outputPath.c_str(), AVIO_FLAG_WRITE);
        }
        avformat_write_header(fmtCtx, nullptr);

        // Initialize Scaling Context to safely parse RGBA frames to YUV420P
        SwsContext* swsCtx = sws_getContext(m_width, m_height, AV_PIX_FMT_RGBA,
                                            m_width, m_height, AV_PIX_FMT_YUV420P,
                                            SWS_BICUBIC, nullptr, nullptr, nullptr);

        AVFrame* yuvFrame = av_frame_alloc();
        yuvFrame->format = AV_PIX_FMT_YUV420P;
        yuvFrame->width = m_width;
        yuvFrame->height = m_height;
        av_frame_get_buffer(yuvFrame, 32);

        // Core processing consumer loop
        while (true) {
            std::vector<uint8_t> rawFrame;
            std::vector<float> rawAudio;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait(lock, [this] {
                    return !m_videoQueue.empty() || !m_audioQueue.empty() || m_stopEncoder.load();
                });

                if (!m_videoQueue.empty()) {
                    rawFrame = std::move(m_videoQueue.front());
                    m_videoQueue.pop();
                }
                if (!m_audioQueue.empty()) {
                    rawAudio = std::move(m_audioQueue.front());
                    m_audioQueue.pop();
                }
                if (m_videoQueue.empty() && m_audioQueue.empty() && m_stopEncoder.load()) {
                    break; // Finalized execution signal received safely
                }
            }

            // Encode Video Frame if fetched
            if (!rawFrame.empty()) {
                const uint8_t* srcData[1] = { rawFrame.data() };
                int srcLinesize[1] = { m_width * 4 };
                sws_scale(swsCtx, srcData, srcLinesize, 0, m_height, yuvFrame->data, yuvFrame->linesize);
                
                yuvFrame->pts = m_videoFrameCount++;
                avcodec_send_frame(videoEncCtx, yuvFrame);
                AVPacket* pkt = av_packet_alloc();
                if (avcodec_receive_packet(videoEncCtx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, videoEncCtx->time_base, videoStream->time_base);
                    pkt->stream_index = videoStream->index;
                    av_interleaved_write_frame(fmtCtx, pkt);
                }
                av_packet_free(&pkt);
            }

            // Encode Audio Chunk if fetched
            if (!rawAudio.empty()) {
                AVFrame* audioFrame = av_frame_alloc();
                audioFrame->nb_samples = static_cast<int>(rawAudio.size() / 2);
                audioFrame->format = AV_SAMPLE_FMT_FLTP;
                audioFrame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                av_frame_get_buffer(audioFrame, 0);

                // Split interleaved FMOD input samples into separated layout tracking structures
                float* leftChannel = reinterpret_cast<float*>(audioFrame->data[0]);
                float* rightChannel = reinterpret_cast<float*>(audioFrame->data[1]);
                for (size_t i = 0; i < rawAudio.size() / 2; ++i) {
                    leftChannel[i] = rawAudio[i * 2];
                    rightChannel[i] = rawAudio[i * 2 + 1];
                }

                audioFrame->pts = m_audioSampleCount;
                m_audioSampleCount += audioFrame->nb_samples;

                avcodec_send_frame(audioEncCtx, audioFrame);
                AVPacket* pkt = av_packet_alloc();
                if (avcodec_receive_packet(audioEncCtx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, audioEncCtx->time_base, audioStream->time_base);
                    pkt->stream_index = audioStream->index;
                    av_interleaved_write_frame(fmtCtx, pkt);
                }
                av_packet_free(&pkt);
                av_frame_free(&audioFrame);
            }
        }

        // Flush encoders, write file tail signatures, clean heap memory
        av_write_trailer(fmtCtx);
        av_frame_free(&yuvFrame);
        sws_freeContext(swsCtx);
        avcodec_free_context(&videoEncCtx);
        avcodec_free_context(&audioEncCtx);
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmtCtx->pb);
        }
        avformat_free_context(fmtCtx);
    }
};

// ════════════════════════════════════════════════════════════════
//  FMOD Sound Engine Master Interception (Unified Platform Audio)
// ════════════════════════════════════════════════════════════════
FMOD_RESULT F_CALLBACK fmodAudioCallback(FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int* outchannels) {
    // Intercept data streams smoothly across systems
    if (UniversalRecorder::get().isRecording()) {
        UniversalRecorder::get().pushAudioSamples(inbuffer, length * inchannels);
    }
    
    // Explicitly pass audio along untouched to avoid complete engine muting
    for (unsigned int i = 0; i < length * inchannels; ++i) {
        outbuffer[i] = inbuffer[i];
    }
    return FMOD_OK;
}

// ════════════════════════════════════════════════════════════════
//  Cross-Platform State Control Globals
// ════════════════════════════════════════════════════════════════
static bool g_isPlayLayerActive = false;
static FMOD::DSP* g_fmodDsp = nullptr;

// ════════════════════════════════════════════════════════════════
//  CCDirector Hook — High performance engine draw loops
// ════════════════════════════════════════════════════════════════
class $modify(MyDirector, CCDirector) {
    void drawScene() {
        CCDirector::drawScene();

        if (g_isPlayLayerActive && UniversalRecorder::get().isRecording()) {
            auto view = this->getOpenGLView();
            if (!view) return;

            auto size = view->getFrameSize();
            int width = static_cast<int>(size.width);
            int height = static_cast<int>(size.height);
            size_t frameByteSize = static_cast<size_t>(width * height * 4);

            // Statically cache buffer arrays across draws to optimize processing alloc overhead
            static std::vector<uint8_t> pixelBuffer;
            if (pixelBuffer.size() != frameByteSize) {
                pixelBuffer.resize(frameByteSize);
            }

            // Safely execute cross-platform raw frame grabs directly off GPU contexts
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
            UniversalRecorder::get().pushVideoFrame(pixelBuffer.data(), frameByteSize);
        }
    }
};

// ════════════════════════════════════════════════════════════════
//  PlayLayer Lifecycle Hook
// ════════════════════════════════════════════════════════════════
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
        m_fields->m_previousBest          = level->m_normalPercent;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;
        m_fields->m_hasQuit               = false;

        startNewRecording();
        return true;
    }

    void startNewRecording() {
        if (m_fields->m_hasQuit) return;

        if (UniversalRecorder::get().isRecording()) {
            UniversalRecorder::get().stop(false);
        }

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string nameString = "Recording_" + std::to_string(ms);

        auto view = CCDirector::sharedDirector()->getOpenGLView();
        if (!view) return;
        auto size = view->getFrameSize();

        // Dynamically request current operational parameter tracking configs from FMOD
        auto fmodSystem = geode::FMODAudioEngine::sharedEngine()->m_system;
        int sampleRate = 44100;
        if (fmodSystem) {
            fmodSystem->getSoftwareFormat(&sampleRate, nullptr, nullptr);
        }

        UniversalRecorder::get().start(nameString, static_cast<int>(size.width), static_cast<int>(size.height), sampleRate);

        // Wire FMOD Interception Framework to global master nodes seamlessly if uninitialized
        if (fmodSystem && !g_fmodDsp) {
            FMOD::ChannelGroup* masterGroup = nullptr;
            fmodSystem->getMasterChannelGroup(&masterGroup);
            if (masterGroup) {
                FMOD_DSP_DESCRIPTION dspDesc;
                std::memset(&dspDesc, 0, sizeof(dspDesc));
                dspDesc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
                std::strncpy(dspDesc.name, "MacroSafeguardAudioInterception", sizeof(dspDesc.name));
                dspDesc.read = fmodAudioCallback;

                fmodSystem->createDSP(&dspDesc, &g_fmodDsp);
                if (g_fmodDsp) {
                    masterGroup->addDSP(0, g_fmodDsp);
                }
            }
        }
    }

    void checkAndSaveIfQualified() {
        if (m_fields->m_hasSavedThisAttempt) return;

        bool    onlyNewBest = Mod::get()->getSettingValue<bool>("only-new-best");
        int64_t threshold   = Mod::get()->getSettingValue<int64_t>("custom-percentage");

        bool shouldSave = false;
        if (onlyNewBest) {
            shouldSave = (m_fields->m_maxPercentThisAttempt > m_fields->m_previousBest);
        } else {
            shouldSave = (m_fields->m_maxPercentThisAttempt >= static_cast<int>(threshold));
        }
        if (m_fields->m_maxPercentThisAttempt >= 100) shouldSave = true;

        m_fields->m_hasSavedThisAttempt = true;

        // Finalize multi-stream encoder threads safely
        UniversalRecorder::get().stop(shouldSave);

        if (shouldSave) {
            std::string msg = "Saved! Video + Game audio safely exported to MacroSafeguard.";

            // Thread-safely defer notification to the next frame loop.
            geode::queueInMainThread([msg]() {
                auto notif = Notification::create(msg, NotificationIcon::Success, 3.5f);
                if (notif) {
                    notif->show();
                }
            });
        }
    }

    void resetLevel() {
        checkAndSaveIfQualified();
        int newBest = this->m_level ? this->m_level->m_normalPercent : 0;

        PlayLayer::resetLevel();

        m_fields->m_previousBest          = newBest;
        m_fields->m_maxPercentThisAttempt = 0;
        m_fields->m_hasSavedThisAttempt   = false;

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
            int pct = std::clamp(this->getCurrentPercentInt(), 0, 100);
            if (pct > m_fields->m_maxPercentThisAttempt) {
                m_fields->m_maxPercentThisAttempt = pct;
            }
        }
    }

    void onQuit() {
        m_fields->m_hasQuit = true;
        checkAndSaveIfQualified();
        g_isPlayLayerActive = false;
        PlayLayer::onQuit();
    }
};

