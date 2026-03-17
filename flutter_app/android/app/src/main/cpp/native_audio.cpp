#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include <opus.h>
#include <cstring>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LOG_TAG "AudioBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Audio constants matching spec
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int FRAME_SIZE = 480; // 10ms at 48kHz
static constexpr int SAMPLES_PER_FRAME = FRAME_SIZE * CHANNELS;
static constexpr int HEADER_SIZE = 16;
static constexpr uint8_t PACKET_AUDIO = 0x01;
static constexpr uint8_t PACKET_MIC_AUDIO = 0x04;

// Jitter buffer constants — aggressive low-latency mode
static constexpr int MIN_BUFFER_FRAMES = 0;  // 0 = pass-through, play immediately
static constexpr int MAX_BUFFER_FRAMES = 3;  // 30ms max — accept some glitches over huge latency
static constexpr int INITIAL_BUFFER_FRAMES = 1; // 10ms — single-frame startup

struct AudioFrame {
    int16_t samples[SAMPLES_PER_FRAME];
    uint32_t sequence;
    uint64_t timestamp;
};

class JitterBuffer {
public:
    JitterBuffer() : targetFrames_(INITIAL_BUFFER_FRAMES), underruns_(0), earlyCount_(0) {}

    void push(const AudioFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Insert sorted by sequence number, drop duplicates
        if (!frames_.empty() && frame.sequence <= frames_.back().sequence) {
            // Out of order or duplicate - try to insert in order
            if (frame.sequence < frames_.front().sequence) {
                // Too old, drop
                return;
            }
            // Find insertion point
            for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                if (it->sequence == frame.sequence) return; // duplicate
                if (it->sequence > frame.sequence) {
                    frames_.insert(it, frame);
                    return;
                }
            }
            return;
        }
        frames_.push_back(frame);

        // Limit max buffer size
        while (frames_.size() > static_cast<size_t>(MAX_BUFFER_FRAMES * 2)) {
            frames_.pop_front();
        }
    }

    bool pop(AudioFrame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            underruns_++;
            // Grow buffer on underrun
            if (underruns_ >= 3 && targetFrames_ < MAX_BUFFER_FRAMES) {
                targetFrames_++;
                underruns_ = 0;
                LOGI("Jitter buffer grown to %d frames", targetFrames_);
            }
            return false;
        }

        // Only start playing once we have enough buffered frames
        // With targetFrames_ == 0, this is always satisfied (pass-through)
        if (!primed_ && targetFrames_ > 0 && frames_.size() < static_cast<size_t>(targetFrames_)) {
            return false;
        }
        primed_ = true;

        out = frames_.front();
        frames_.pop_front();

        // Adapt: shrink if consistently have excess
        if (frames_.size() > static_cast<size_t>(targetFrames_)) {
            earlyCount_++;
            if (earlyCount_ >= 10 && targetFrames_ > MIN_BUFFER_FRAMES) {
                targetFrames_--;
                earlyCount_ = 0;
                LOGI("Jitter buffer shrunk to %d frames", targetFrames_);
            }
        } else {
            earlyCount_ = 0;
        }

        underruns_ = 0;
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        targetFrames_ = INITIAL_BUFFER_FRAMES;
        underruns_ = 0;
        earlyCount_ = 0;
        primed_ = false;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(frames_.size());
    }

private:
    std::deque<AudioFrame> frames_;
    std::mutex mutex_;
    int targetFrames_;
    int underruns_;
    int earlyCount_;
    bool primed_ = false;
};

class AudioPlayer : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioPlayer() : decoder_(nullptr), running_(false), latencyMs_(0.0), muted_(false) {}

    void setMuted(bool muted) { muted_.store(muted); }

    bool start() {
        int error;
        decoder_ = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
        if (error != OPUS_OK || !decoder_) {
            LOGE("Failed to create Opus decoder: %s", opus_strerror(error));
            return false;
        }

        jitterBuffer_.reset();

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Exclusive)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setFramesPerCallback(FRAME_SIZE)
               ->setDataCallback(this)
               ->setErrorCallback(this)
               ->setUsage(oboe::Usage::Media)
               ->setContentType(oboe::ContentType::Speech)
               ->setBufferCapacityInFrames(FRAME_SIZE * 2);

        oboe::Result result = builder.openStream(stream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open stream: %s", oboe::convertToText(result));
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
            return false;
        }

        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
            LOGE("Failed to start stream: %s", oboe::convertToText(result));
            stream_->close();
            stream_.reset();
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
            return false;
        }

        running_ = true;
        LOGI("Audio player started, buffer size: %d", stream_->getBufferSizeInFrames());
        return true;
    }

    void stop() {
        running_ = false;
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        if (decoder_) {
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
        }
        jitterBuffer_.reset();
    }

    void feedPacket(const uint8_t* data, int length) {
        if (!running_ || !decoder_ || length < HEADER_SIZE) return;

        uint8_t version = data[0];
        uint8_t type = data[1];

        if (version != 0x01 || type != 0x01) return; // Only process audio packets

        uint32_t seq;
        uint64_t timestamp;
        uint16_t payloadLen;

        memcpy(&seq, data + 2, 4);
        memcpy(&timestamp, data + 6, 8);
        memcpy(&payloadLen, data + 14, 2);

        if (length < HEADER_SIZE + payloadLen) return;

        const uint8_t* opusData = data + HEADER_SIZE;

        // Decode Opus
        AudioFrame frame;
        frame.sequence = seq;
        frame.timestamp = timestamp;

        int decoded = opus_decode(decoder_, opusData, payloadLen,
                                  frame.samples, FRAME_SIZE, 0);
        if (decoded < 0) {
            LOGE("Opus decode error: %s", opus_strerror(decoded));
            return;
        }

        // Calculate latency estimate from timestamp
        // timestamp is microseconds since stream start on server
        auto now = std::chrono::steady_clock::now();
        auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        // Simple rolling estimate based on jitter buffer depth
        double bufferLatency = jitterBuffer_.size() * 10.0; // each frame = 10ms
        latencyMs_.store(bufferLatency);

        jitterBuffer_.push(frame);
    }

    double getLatency() const {
        return latencyMs_.load();
    }

    // Oboe callback
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream,
            void* audioData,
            int32_t numFrames) override {

        auto* output = static_cast<int16_t*>(audioData);

        if (!running_) {
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
            return oboe::DataCallbackResult::Stop;
        }

        // If muted, output silence but still consume jitter buffer
        if (muted_.load()) {
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
            AudioFrame discard;
            while (jitterBuffer_.pop(discard)) {}
            return oboe::DataCallbackResult::Continue;
        }

        int framesWritten = 0;
        while (framesWritten < numFrames) {
            int framesToWrite = std::min(FRAME_SIZE, numFrames - framesWritten);

            AudioFrame frame;
            if (jitterBuffer_.pop(frame)) {
                memcpy(output + framesWritten * CHANNELS,
                       frame.samples,
                       framesToWrite * CHANNELS * sizeof(int16_t));
            } else {
                // Underrun - output silence
                memset(output + framesWritten * CHANNELS, 0,
                       framesToWrite * CHANNELS * sizeof(int16_t));

                // Use Opus PLC if decoder available
                if (decoder_) {
                    int16_t plcSamples[SAMPLES_PER_FRAME];
                    int decoded = opus_decode(decoder_, nullptr, 0, plcSamples, FRAME_SIZE, 0);
                    if (decoded > 0) {
                        int copyFrames = std::min(decoded, framesToWrite);
                        memcpy(output + framesWritten * CHANNELS,
                               plcSamples,
                               copyFrames * CHANNELS * sizeof(int16_t));
                    }
                }
            }
            framesWritten += framesToWrite;
        }

        return oboe::DataCallbackResult::Continue;
    }

    // Oboe error callback — handle device disconnection
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOGI("Audio device disconnected, restarting stream...");
            // Restart on a new thread — callbacks must not block
            std::thread([this]() {
                stop();
                start();
            }).detach();
        } else {
            LOGE("Audio stream error: %s", oboe::convertToText(error));
        }
    }

private:
    OpusDecoder* decoder_;
    std::shared_ptr<oboe::AudioStream> stream_;
    JitterBuffer jitterBuffer_;
    std::atomic<bool> running_;
    std::atomic<double> latencyMs_;
    std::atomic<bool> muted_;
};

// --- AudioRecorder (Microphone to PC) ---
// Uses mono (1 channel) since phone mics are typically mono.
// The server-side Opus decoder must also be configured for mono.
static constexpr int MIC_CHANNELS = 1;

class AudioRecorder : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioRecorder() : encoder_(nullptr), running_(false), socket_(-1), sequence_(0) {}

    bool start(const char* serverIp, int serverPort) {
        // Save connection info for restart on device change
        serverIp_ = serverIp;
        serverPort_ = serverPort;

        int error;
        // Use OPUS_APPLICATION_RESTRICTED_LOWDELAY for minimum latency
        // Mono for mic input — phone mics are typically mono
        encoder_ = opus_encoder_create(SAMPLE_RATE, MIC_CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (error != OPUS_OK || !encoder_) {
            LOGE("Failed to create Opus encoder: %s", opus_strerror(error));
            return false;
        }

        // Configure UDP Socket
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) {
            LOGE("Failed to create socket for recording");
            opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
            return false;
        }

        memset(&serverAddr_, 0, sizeof(serverAddr_));
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) <= 0) {
            LOGE("Invalid server IP address: %s", serverIp);
            close(socket_);
            socket_ = -1;
            opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
            return false;
        }

        streamStart_ = std::chrono::steady_clock::now();
        sequence_ = 0;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Exclusive)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(MIC_CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setInputPreset(oboe::InputPreset::VoiceCommunication)
               ->setFramesPerCallback(FRAME_SIZE)
               ->setDataCallback(this)
               ->setErrorCallback(this);

        oboe::Result result = builder.openStream(stream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open recording stream: %s", oboe::convertToText(result));
            cleanup();
            return false;
        }

        // Verify actual channel count matches what we requested
        actualChannels_ = stream_->getChannelCount();
        if (actualChannels_ != MIC_CHANNELS) {
            LOGI("Requested %d channels, got %d — will adapt", MIC_CHANNELS, actualChannels_);
        }

        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
            LOGE("Failed to start recording stream: %s", oboe::convertToText(result));
            cleanup();
            return false;
        }

        running_ = true;
        LOGI("Audio recording started to %s:%d (channels: %d)", serverIp, serverPort, actualChannels_);
        return true;
    }

    void stop() {
        running_ = false;
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        cleanup();
        LOGI("Audio recording stopped");
    }

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream,
            void* audioData,
            int32_t numFrames) override {

        if (!running_ || !encoder_ || socket_ < 0) {
            return oboe::DataCallbackResult::Stop;
        }

        auto* input = static_cast<int16_t*>(audioData);
        int framesRead = 0;

        while (framesRead < numFrames) {
            int framesToRead = std::min(FRAME_SIZE, numFrames - framesRead);
            if (framesToRead < FRAME_SIZE) {
                break;
            }

            // If Oboe gave us more channels than we need, downmix to mono
            int16_t monoBuffer[FRAME_SIZE];
            const int16_t* encodeInput;

            if (actualChannels_ > MIC_CHANNELS) {
                // Downmix to mono by averaging channels
                for (int i = 0; i < FRAME_SIZE; i++) {
                    int32_t sum = 0;
                    for (int ch = 0; ch < actualChannels_; ch++) {
                        sum += input[(framesRead + i) * actualChannels_ + ch];
                    }
                    monoBuffer[i] = static_cast<int16_t>(sum / actualChannels_);
                }
                encodeInput = monoBuffer;
            } else {
                encodeInput = input + (framesRead * MIC_CHANNELS);
            }

            uint8_t packet[1500];

            int opusLen = opus_encode(encoder_, encodeInput, FRAME_SIZE,
                                      packet + HEADER_SIZE, sizeof(packet) - HEADER_SIZE);

            if (opusLen > 0) {
                auto now = std::chrono::steady_clock::now();
                uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - streamStart_).count();

                packet[0] = 0x01; // Version
                packet[1] = PACKET_MIC_AUDIO; // Type = Mic Audio (0x04)

                uint32_t seq = sequence_++;
                uint16_t payloadLen = static_cast<uint16_t>(opusLen);

                memcpy(packet + 2, &seq, 4);
                memcpy(packet + 6, &timestamp, 8);
                memcpy(packet + 14, &payloadLen, 2);

                sendto(socket_, packet, HEADER_SIZE + opusLen, 0,
                       (struct sockaddr*)&serverAddr_, sizeof(serverAddr_));
            }

            framesRead += framesToRead;
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOGI("Recording device disconnected, restarting stream...");
            std::thread([this]() {
                stop();
                if (!serverIp_.empty()) {
                    start(serverIp_.c_str(), serverPort_);
                }
            }).detach();
        } else {
            LOGE("Recording stream error: %s", oboe::convertToText(error));
            stop();
        }
    }

private:
    void cleanup() {
        if (encoder_) {
            opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
        }
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
    }

    OpusEncoder* encoder_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::atomic<bool> running_;
    int socket_;
    struct sockaddr_in serverAddr_;
    std::chrono::steady_clock::time_point streamStart_;
    std::atomic<uint32_t> sequence_;
    int actualChannels_ = MIC_CHANNELS;
    std::string serverIp_;
    int serverPort_ = 0;
};

// Global player instance
static AudioPlayer g_player;
// Global recorder instance
static AudioRecorder g_recorder;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStartAudio(
        JNIEnv* env, jobject thiz) {
    return g_player.start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStopAudio(
        JNIEnv* env, jobject thiz) {
    g_player.stop();
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeFeedAudio(
        JNIEnv* env, jobject thiz, jbyteArray data) {
    jsize len = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    g_player.feedPacket(reinterpret_cast<const uint8_t*>(bytes), len);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

JNIEXPORT jdouble JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetLatency(
        JNIEnv* env, jobject thiz) {
    return g_player.getLatency();
}

JNIEXPORT jboolean JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStartRecording(
        JNIEnv* env, jobject thiz, jstring serverIp, jint port) {
    
    const char* ipStr = env->GetStringUTFChars(serverIp, nullptr);
    bool result = g_recorder.start(ipStr, port);
    env->ReleaseStringUTFChars(serverIp, ipStr);
    
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStopRecording(
        JNIEnv* env, jobject thiz) {
    g_recorder.stop();
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSetMuted(
        JNIEnv* env, jobject thiz, jboolean muted) {
    g_player.setMuted(muted == JNI_TRUE);
}

} // extern "C"
