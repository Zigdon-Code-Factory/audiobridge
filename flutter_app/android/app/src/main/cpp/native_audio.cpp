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
#include <poll.h>
#include <functional>
#include <string>

#include "dtls_session.h"

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
static constexpr uint8_t PACKET_KEEPALIVE = 0x02;
static constexpr uint8_t PACKET_CONTROL = 0x03;
static constexpr uint8_t PACKET_MIC_AUDIO = 0x04;
static constexpr uint8_t PACKET_MEDIA_INFO = 0x05;
static constexpr uint8_t PACKET_PING = 0x06;
static constexpr uint8_t PACKET_PONG = 0x07;
static constexpr uint8_t PACKET_SETTINGS = 0x08;

// Jitter buffer defaults (overridden by server settings)
static constexpr int DEFAULT_MIN_FRAMES = 2;     // 20ms
static constexpr int DEFAULT_MAX_FRAMES = 5;     // 50ms
static constexpr int DEFAULT_INITIAL_FRAMES = 3;  // 30ms

struct AudioFrame {
    int16_t samples[SAMPLES_PER_FRAME];
    uint32_t sequence;
    uint64_t timestamp;
};

class JitterBuffer {
public:
    JitterBuffer()
        : minFrames_(DEFAULT_MIN_FRAMES), maxFrames_(DEFAULT_MAX_FRAMES),
          targetFrames_(DEFAULT_INITIAL_FRAMES), underruns_(0), earlyCount_(0) {}

    void setTarget(int jitterMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        int frames = jitterMs / 10; // 10ms per frame
        if (frames < 1) frames = 1;
        minFrames_ = std::max(1, frames - 1);
        maxFrames_ = frames + 3;
        targetFrames_ = frames;
        LOGI("Jitter buffer configured: min=%d, target=%d, max=%d (from %dms)",
             minFrames_, targetFrames_, maxFrames_, jitterMs);
    }

    int getTargetMs() const { return targetFrames_ * 10; }

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
        while (frames_.size() > static_cast<size_t>(maxFrames_ * 2)) {
            frames_.pop_front();
        }
    }

    bool pop(AudioFrame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            underruns_++;
            // Grow buffer on underrun
            if (underruns_ >= 3 && targetFrames_ < maxFrames_) {
                targetFrames_++;
                underruns_ = 0;
                LOGI("Jitter buffer grown to %d frames", targetFrames_);
            }
            return false;
        }

        // Only start playing once we have enough buffered frames
        if (!primed_ && targetFrames_ > 0 && frames_.size() < static_cast<size_t>(targetFrames_)) {
            return false;
        }
        primed_ = true;

        out = frames_.front();
        frames_.pop_front();

        // Adapt: shrink if consistently have excess
        if (frames_.size() > static_cast<size_t>(targetFrames_)) {
            earlyCount_++;
            if (earlyCount_ >= 30 && targetFrames_ > minFrames_) {
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
        targetFrames_ = std::max(minFrames_, DEFAULT_INITIAL_FRAMES);
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
    int minFrames_;
    int maxFrames_;
    int targetFrames_;
    int underruns_;
    int earlyCount_;
    bool primed_ = false;
};

class AudioPlayer : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioPlayer() : decoder_(nullptr), running_(false), latencyMs_(0.0), muted_(false), volume_(1.0f), peakLevel_(0.0f) {}

    void setMuted(bool muted) { muted_.store(muted); }
    void setVolume(float vol) { volume_.store(vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol)); }

    bool start() {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
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
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setFramesPerCallback(FRAME_SIZE)
               ->setDataCallback(this)
               ->setErrorCallback(this)
               ->setUsage(oboe::Usage::Media)
               ->setContentType(oboe::ContentType::Music)
               ->setBufferCapacityInFrames(FRAME_SIZE * 8);

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
        LOGI("Audio player started: sampleRate=%d, channelCount=%d, format=%d, framesPerBurst=%d, bufferSize=%d",
             stream_->getSampleRate(), stream_->getChannelCount(),
             (int)stream_->getFormat(), stream_->getFramesPerBurst(),
             stream_->getBufferSizeInFrames());
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
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
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
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

        // Diagnostic: log first packets and periodically
        static int feedCount = 0;
        feedCount++;
        if (feedCount <= 5 || (feedCount % 500) == 0) {
            LOGI("[DIAG] feedPacket #%d: len=%d, seq=%u, payloadLen=%u, opus[0..3]=%02x %02x %02x %02x, jbuf=%d",
                 feedCount, length, seq, payloadLen,
                 payloadLen > 0 ? opusData[0] : 0,
                 payloadLen > 1 ? opusData[1] : 0,
                 payloadLen > 2 ? opusData[2] : 0,
                 payloadLen > 3 ? opusData[3] : 0,
                 jitterBuffer_.size());
        }

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

        // Diagnostic: log decoded PCM stats
        if (feedCount <= 5 || (feedCount % 500) == 0) {
            int16_t maxSample = 0, minSample = 0;
            for (int i = 0; i < decoded * CHANNELS; i++) {
                if (frame.samples[i] > maxSample) maxSample = frame.samples[i];
                if (frame.samples[i] < minSample) minSample = frame.samples[i];
            }
            LOGI("[DIAG] decoded=%d frames, PCM range=[%d, %d], samples[0..3]=%d %d %d %d",
                 decoded, minSample, maxSample,
                 frame.samples[0], frame.samples[1], frame.samples[2], frame.samples[3]);
        }

        // Calculate latency estimate from timestamp
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

    double getOutputBufferMs() const {
        if (!stream_) return 0.0;
        return stream_->getBufferSizeInFrames() * 1000.0 / SAMPLE_RATE;
    }

    float getPeakLevel() const { return peakLevel_.load(); }

    void setJitterTarget(int jitterMs) {
        jitterBuffer_.setTarget(jitterMs);
    }

    int getJitterTargetMs() const {
        return jitterBuffer_.getTargetMs();
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
            // If we have leftover samples from a previous frame, use those first
            if (residualCount_ > 0) {
                int framesToCopy = std::min(residualCount_, numFrames - framesWritten);
                int residualOffset = (FRAME_SIZE - residualCount_) * CHANNELS;
                memcpy(output + framesWritten * CHANNELS,
                       residualSamples_ + residualOffset,
                       framesToCopy * CHANNELS * sizeof(int16_t));
                residualCount_ -= framesToCopy;
                framesWritten += framesToCopy;
                continue;
            }

            // Try to pop a complete frame from the jitter buffer
            AudioFrame frame;
            if (jitterBuffer_.pop(frame)) {
                int framesToCopy = std::min(FRAME_SIZE, numFrames - framesWritten);
                memcpy(output + framesWritten * CHANNELS,
                       frame.samples,
                       framesToCopy * CHANNELS * sizeof(int16_t));
                framesWritten += framesToCopy;

                // If we couldn't fit the entire frame, save the remainder
                if (framesToCopy < FRAME_SIZE) {
                    residualCount_ = FRAME_SIZE - framesToCopy;
                    memcpy(residualSamples_, frame.samples, FRAME_SIZE * CHANNELS * sizeof(int16_t));
                }
            } else {
                // Underrun — fill with silence.
                // Note: Opus PLC (opus_decode with nullptr) cannot be used here
                // because the decoder is accessed from the recv thread in feedPacket().
                // Opus is not thread-safe, and concurrent access corrupts decoder
                // state, producing static/garbage audio.
                int framesToFill = std::min(FRAME_SIZE, numFrames - framesWritten);
                memset(output + framesWritten * CHANNELS, 0,
                       framesToFill * CHANNELS * sizeof(int16_t));
                framesWritten += framesToFill;
            }
        }

        // Diagnostic: log Oboe callback stats
        {
            static int cbCount = 0;
            cbCount++;
            if (cbCount <= 5 || (cbCount % 500) == 0) {
                int16_t maxS = 0, minS = 0;
                int totalSamples = numFrames * CHANNELS;
                for (int i = 0; i < totalSamples; i++) {
                    if (output[i] > maxS) maxS = output[i];
                    if (output[i] < minS) minS = output[i];
                }
                LOGI("[DIAG] oboe #%d: numFrames=%d, framesWritten=%d, PCM=[%d,%d], out[0..3]=%d %d %d %d",
                     cbCount, numFrames, framesWritten, minS, maxS,
                     output[0], output[1], output[2], output[3]);
            }
        }

        // Apply volume scaling
        float vol = volume_.load();
        if (vol < 1.0f) {
            int totalSamples = numFrames * CHANNELS;
            for (int i = 0; i < totalSamples; i++) {
                output[i] = static_cast<int16_t>(output[i] * vol);
            }
        }

        // Compute peak level for UI meter
        {
            int totalSamples = numFrames * CHANNELS;
            int16_t maxSample = 0;
            for (int i = 0; i < totalSamples; i++) {
                int16_t abs = output[i] < 0 ? -output[i] : output[i];
                if (abs > maxSample) maxSample = abs;
            }
            float peak = maxSample / 32768.0f;
            float prev = peakLevel_.load();
            // Fast attack, slow decay
            peakLevel_.store(peak > prev ? peak : prev * 0.85f + peak * 0.15f);
        }

        return oboe::DataCallbackResult::Continue;
    }

    // Oboe error callback — handle device disconnection
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOGI("Audio device disconnected, restarting stream...");
            // Restart on a new thread — callbacks must not block
            // lifecycleMutex_ in stop/start prevents races with feedPacket
            std::thread([this]() {
                stop();
                start();
            }).detach();
        } else {
            LOGE("Audio stream error: %s", oboe::convertToText(error));
        }
    }

private:
    std::mutex lifecycleMutex_; // protects start/stop/feedPacket against concurrent access
    OpusDecoder* decoder_;
    std::shared_ptr<oboe::AudioStream> stream_;
    JitterBuffer jitterBuffer_;
    std::atomic<bool> running_;
    std::atomic<double> latencyMs_;
    std::atomic<bool> muted_;
    std::atomic<float> volume_;
    std::atomic<float> peakLevel_;

    // Residual buffer for frames that didn't fit in the previous callback
    int16_t residualSamples_[SAMPLES_PER_FRAME];
    int residualCount_ = 0;
};

// --- AudioRecorder (Microphone to PC) ---
// Uses mono (1 channel) since phone mics are typically mono.
static constexpr int MIC_CHANNELS = 1;

// Send callback type: sends raw data through DTLS
using SendCallback = std::function<int(const uint8_t* data, size_t len)>;

class AudioRecorder : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioRecorder() : encoder_(nullptr), running_(false), sequence_(0), peakLevel_(0.0f) {}

    void setSendCallback(SendCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        sendCallback_ = std::move(cb);
    }

    void clearSendCallback() {
        std::lock_guard<std::mutex> lock(cbMutex_);
        sendCallback_ = nullptr;
    }

    float getPeakLevel() const { return peakLevel_.load(); }

    bool start() {
        int error;
        // Use OPUS_APPLICATION_RESTRICTED_LOWDELAY for minimum latency
        // Mono for mic input — phone mics are typically mono
        encoder_ = opus_encoder_create(SAMPLE_RATE, MIC_CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (error != OPUS_OK || !encoder_) {
            LOGE("Failed to create Opus encoder: %s", opus_strerror(error));
            return false;
        }

        streamStart_ = std::chrono::steady_clock::now();
        sequence_ = 0;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(MIC_CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setInputPreset(oboe::InputPreset::Unprocessed)
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
        LOGI("Audio recording started (channels: %d)", actualChannels_);
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

        if (!running_ || !encoder_) {
            return oboe::DataCallbackResult::Stop;
        }

        // Check if we have a send callback
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (!sendCallback_) {
            return oboe::DataCallbackResult::Continue;
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

                sendCallback_(packet, HEADER_SIZE + opusLen);
            }

            // Compute peak level for mic meter
            {
                int16_t maxSample = 0;
                for (int i = 0; i < FRAME_SIZE; i++) {
                    int16_t abs = encodeInput[i] < 0 ? -encodeInput[i] : encodeInput[i];
                    if (abs > maxSample) maxSample = abs;
                }
                float peak = maxSample / 32768.0f;
                float prev = peakLevel_.load();
                peakLevel_.store(peak > prev ? peak : prev * 0.85f + peak * 0.15f);
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
                start();
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
    }

    OpusEncoder* encoder_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::atomic<bool> running_;
    std::chrono::steady_clock::time_point streamStart_;
    std::atomic<uint32_t> sequence_;
    int actualChannels_ = MIC_CHANNELS;

    std::mutex cbMutex_;
    SendCallback sendCallback_;
    std::atomic<float> peakLevel_;
};

// --- ConnectionManager ---
// Handles the full connection lifecycle: AB_CONNECT, AB_ACCEPT/PSK, DTLS handshake, encrypted streaming

class ConnectionManager {
public:
    ConnectionManager() : socket_(-1), running_(false), connected_(false),
                          rttMs_(0.0), pingPending_(false), serverJitterMs_(0),
                          player_(nullptr), jvm_(nullptr), callbackObj_(nullptr) {}

    ~ConnectionManager() {
        running_ = false;
        connected_ = false;
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
        if (recvThread_.joinable()) {
            recvThread_.join();
        }
    }

    void setJvm(JavaVM* jvm) { jvm_ = jvm; }
    void setCallbackObj(JNIEnv* env, jobject obj) {
        if (callbackObj_) {
            env->DeleteGlobalRef(callbackObj_);
            callbackObj_ = nullptr;
        }
        callbackObj_ = obj ? env->NewGlobalRef(obj) : nullptr;
    }

    void setAudioPlayer(AudioPlayer* player) { player_ = player; }

    void setRecorderSendCallback(AudioRecorder* recorder) {
        if (recorder) {
            recorder->setSendCallback([this](const uint8_t* data, size_t len) -> int {
                return sendEncrypted(data, len);
            });
        }
    }

    void clearRecorderSendCallback(AudioRecorder* recorder) {
        if (recorder) {
            recorder->clearSendCallback();
        }
    }

    // connect() — blocking call, returns result string:
    //   "accepted" — connected, no new PSK
    //   "accepted|<psk_hex>" — connected, new PSK received
    //   "pair_pending" — waiting for server approval (will keep waiting)
    //   "rejected|<reason>" — server rejected
    //   "timeout" — no response
    //   "dtls_failed" — DTLS handshake failed
    //   "error|<msg>" — other error
    std::string connect(const char* serverIp, int serverPort,
                        const char* deviceName, const char* deviceId,
                        const char* pskHex) {
        disconnect();

        // Create UDP socket
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) {
            LOGE("ConnectionManager: failed to create socket");
            return "error|socket_failed";
        }

        memset(&serverAddr_, 0, sizeof(serverAddr_));
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) <= 0) {
            LOGE("ConnectionManager: invalid IP %s", serverIp);
            ::close(socket_);
            socket_ = -1;
            return "error|invalid_ip";
        }

        serverIp_ = serverIp;
        serverPort_ = serverPort;
        deviceId_ = deviceId;
        receivedPsk_.clear();

        // Send AB_CONNECT
        std::string connectMsg = std::string("AB_CONNECT|") + deviceName + "|" + deviceId;
        ssize_t sent = sendto(socket_, connectMsg.c_str(), connectMsg.size(), 0,
                              (struct sockaddr*)&serverAddr_, sizeof(serverAddr_));
        if (sent < 0) {
            LOGE("ConnectionManager: sendto failed");
            ::close(socket_);
            socket_ = -1;
            return "error|send_failed";
        }

        LOGI("ConnectionManager: sent AB_CONNECT to %s:%d", serverIp, serverPort);

        // Wait for AB_ACCEPT / AB_PAIR_PENDING / AB_REJECT
        // Total timeout: 35 seconds (to cover pair_pending wait on server side)
        uint8_t buf[2048];
        bool pairPending = false;
        std::string pskBase64;

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);

        while (true) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                ::close(socket_);
                socket_ = -1;
                return "timeout";
            }

            struct pollfd pfd;
            pfd.fd = socket_;
            pfd.events = POLLIN;
            int pollRet = poll(&pfd, 1, std::min((int)remaining.count(), 1000));

            if (pollRet < 0) {
                LOGE("ConnectionManager: poll error");
                ::close(socket_);
                socket_ = -1;
                return "error|poll_failed";
            }

            if (pollRet == 0) {
                // Timeout on this poll iteration, but overall deadline not reached
                // If pair_pending, notify Dart
                if (pairPending) {
                    notifyDart("onPairPending", "");
                }
                continue;
            }

            struct sockaddr_in fromAddr;
            socklen_t fromLen = sizeof(fromAddr);
            ssize_t received = recvfrom(socket_, buf, sizeof(buf), 0,
                                        (struct sockaddr*)&fromAddr, &fromLen);
            if (received <= 0) continue;

            // Check if this is a text message (not a DTLS record)
            // DTLS records start with content type 20-25
            if (buf[0] >= 20 && buf[0] <= 25) {
                // DTLS record received before we expected it — ignore during handshake setup
                continue;
            }

            // Parse as text
            std::string msg(reinterpret_cast<char*>(buf), received);

            if (msg == "AB_ACCEPT" || msg.rfind("AB_ACCEPT|", 0) == 0) {
                // Check for PSK in the accept message
                if (msg.size() > 10 && msg[9] == '|') {
                    pskBase64 = msg.substr(10);
                    LOGI("ConnectionManager: received AB_ACCEPT with PSK");
                } else {
                    LOGI("ConnectionManager: received bare AB_ACCEPT");
                }
                break;
            } else if (msg == "AB_PAIR_PENDING") {
                pairPending = true;
                notifyDart("onPairPending", "");
                LOGI("ConnectionManager: pair pending, waiting for approval...");
                continue;
            } else if (msg.rfind("AB_REJECT", 0) == 0) {
                std::string reason = "Connection denied";
                if (msg.size() > 10 && msg[9] == '|') {
                    reason = msg.substr(10);
                }
                LOGI("ConnectionManager: rejected: %s", reason.c_str());
                ::close(socket_);
                socket_ = -1;
                return "rejected|" + reason;
            }
        }

        // Determine the PSK to use for DTLS
        std::string pskHexToUse;
        if (!pskBase64.empty()) {
            // New PSK from server (first pairing)
            pskHexToUse = DtlsSession::base64ToPskHex(pskBase64);
            receivedPsk_ = pskHexToUse;
            LOGI("ConnectionManager: new PSK received (hex len=%zu)", pskHexToUse.size());
        } else if (pskHex != nullptr && strlen(pskHex) > 0) {
            // Stored PSK for reconnection
            pskHexToUse = pskHex;
            LOGI("ConnectionManager: using stored PSK (hex len=%zu)", pskHexToUse.size());
        } else {
            // No PSK available — legacy server, bare AB_ACCEPT without encryption
            // Fall back to unencrypted mode
            LOGI("ConnectionManager: no PSK available, falling back to unencrypted mode");
            connected_ = true;
            running_ = true;
            lastPacketTime_ = std::chrono::steady_clock::now();
            recvThread_ = std::thread(&ConnectionManager::recvThreadUnencrypted, this);
            if (!receivedPsk_.empty()) {
                return "accepted|" + receivedPsk_;
            }
            return "accepted";
        }

        // Perform DTLS handshake
        auto pskBytes = DtlsSession::hexToBytes(pskHexToUse);
        if (pskBytes.empty()) {
            LOGE("ConnectionManager: invalid PSK hex");
            ::close(socket_);
            socket_ = -1;
            return "error|invalid_psk";
        }

        bool dtlsOk = false;
        if (dtls_.initClient(socket_, serverAddr_, deviceId_, pskBytes)) {
            LOGI("ConnectionManager: starting DTLS handshake...");

            auto hsDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (true) {
                int hsRet = dtls_.continueHandshake();
                if (hsRet == 0) {
                    LOGI("ConnectionManager: DTLS handshake complete!");
                    dtlsOk = true;
                    break;
                } else if (hsRet == MBEDTLS_ERR_SSL_WANT_READ) {
                    auto hsRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        hsDeadline - std::chrono::steady_clock::now());
                    if (hsRemaining.count() <= 0) {
                        LOGE("ConnectionManager: DTLS handshake timeout");
                        break;
                    }

                    struct pollfd pfd;
                    pfd.fd = socket_;
                    pfd.events = POLLIN;
                    int pollRet = poll(&pfd, 1, std::min((int)hsRemaining.count(), 1000));
                    if (pollRet > 0) {
                        struct sockaddr_in fromAddr;
                        socklen_t fromLen = sizeof(fromAddr);
                        ssize_t received = recvfrom(socket_, buf, sizeof(buf), 0,
                                                    (struct sockaddr*)&fromAddr, &fromLen);
                        if (received > 0) {
                            dtls_.pushReceivedData(buf, received);
                        }
                    }
                } else {
                    LOGE("ConnectionManager: DTLS handshake error: -0x%04X", -hsRet);
                    break;
                }
            }

            if (!dtlsOk) {
                dtls_.teardown();
            }
        } else {
            LOGE("ConnectionManager: DTLS init failed");
        }

        // Start recv thread (encrypted if DTLS succeeded, unencrypted otherwise)
        connected_ = true;
        running_ = true;
        lastPacketTime_ = std::chrono::steady_clock::now();
        if (dtlsOk) {
            LOGI("ConnectionManager: streaming with DTLS encryption");
            recvThread_ = std::thread(&ConnectionManager::recvThreadEncrypted, this);
        } else {
            LOGI("ConnectionManager: DTLS failed, falling back to unencrypted");
            recvThread_ = std::thread(&ConnectionManager::recvThreadUnencrypted, this);
        }

        if (!receivedPsk_.empty()) {
            return "accepted|" + receivedPsk_;
        }
        return "accepted";
    }

    void disconnect() {
        bool wasConnected = connected_.exchange(false);
        running_ = false;

        // Send disconnect control if DTLS is up
        if (wasConnected && dtls_.isEstablished() && socket_ >= 0) {
            uint8_t pkt[17];
            memset(pkt, 0, sizeof(pkt));
            pkt[0] = 0x01;
            pkt[1] = PACKET_CONTROL;
            pkt[14] = 1; // payload length
            pkt[16] = 0x03; // disconnect command
            dtls_.send(pkt, 17);
        }

        dtls_.teardown();

        // Close socket to unblock any poll/recvfrom in recv thread
        int sock = socket_;
        socket_ = -1;
        if (sock >= 0) {
            ::close(sock);
        }

        if (recvThread_.joinable()) {
            recvThread_.join();
        }

        rttMs_ = 0.0;
        pingPending_ = false;
        receivedPsk_.clear();

        LOGI("ConnectionManager: disconnected");
    }

    int sendEncrypted(const uint8_t* data, size_t len) {
        int sock = socket_;
        if (!connected_ || sock < 0) return -1;
        if (dtls_.isEstablished()) {
            return dtls_.send(data, len);
        }
        // Unencrypted fallback
        return sendto(sock, data, len, 0,
                      (struct sockaddr*)&serverAddr_, sizeof(serverAddr_));
    }

    void sendKeepalive() {
        if (!connected_ || socket_ < 0) return;
        uint8_t packet[16];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x01;
        packet[1] = PACKET_KEEPALIVE;
        sendEncrypted(packet, 16);
    }

    void sendPing() {
        if (!connected_ || socket_ < 0) return;
        uint8_t packet[16];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x01;
        packet[1] = PACKET_PING;
        lastPingSent_ = std::chrono::steady_clock::now();
        pingPending_ = true;
        sendEncrypted(packet, 16);
    }

    void sendControl(uint8_t cmd) {
        if (!connected_ || socket_ < 0) return;
        uint8_t packet[17];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x01;
        packet[1] = PACKET_CONTROL;
        packet[14] = 1; // payload length
        packet[16] = cmd;
        sendEncrypted(packet, 17);
    }

    bool isConnected() const { return connected_.load(); }
    bool isDtlsActive() const { return dtls_.isEstablished(); }
    double getRtt() const { return rttMs_.load(); }
    std::string getReceivedPsk() const { return receivedPsk_; }

private:
    void recvThreadEncrypted() {
        LOGI("ConnectionManager: encrypted recv thread started");
        uint8_t rawBuf[2048];
        uint8_t decBuf[2048];

        while (running_) {
            int sock = socket_;
            if (sock < 0) break;

            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            int pollRet = poll(&pfd, 1, 1000);

            if (pollRet <= 0) {
                if (!running_) break;
                // Check for timeout
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - lastPacketTime_).count();
                if (elapsed >= TIMEOUT_SECONDS) {
                    LOGI("ConnectionManager: server timeout (%lld seconds)", (long long)elapsed);
                    notifyDart("onDisconnected", "timeout");
                    running_ = false;
                    connected_ = false;
                    break;
                }
                continue;
            }

            sock = socket_;
            if (sock < 0) break;

            struct sockaddr_in fromAddr;
            socklen_t fromLen = sizeof(fromAddr);
            ssize_t received = recvfrom(sock, rawBuf, sizeof(rawBuf), 0,
                                        (struct sockaddr*)&fromAddr, &fromLen);
            if (received <= 0) continue;

            // Check if DTLS record (content type 20-25)
            if (received > 0 && rawBuf[0] >= 20 && rawBuf[0] <= 25) {
                // Push to DTLS engine
                dtls_.pushReceivedData(rawBuf, received);

                // Try to read decrypted data
                int decLen = dtls_.recv(decBuf, sizeof(decBuf));

                // Diagnostic logging
                static int recvCount = 0;
                recvCount++;
                if (recvCount <= 5 || (recvCount % 500) == 0) {
                    LOGI("[DIAG] recv #%d: raw=%zd, dtlsContentType=%d, decLen=%d, dec[0..3]=%02x %02x %02x %02x",
                         recvCount, received, rawBuf[0],
                         decLen,
                         decLen > 0 ? decBuf[0] : 0,
                         decLen > 1 ? decBuf[1] : 0,
                         decLen > 2 ? decBuf[2] : 0,
                         decLen > 3 ? decBuf[3] : 0);
                }

                if (decLen > 0) {
                    dispatchDecrypted(decBuf, decLen);
                } else if (decLen < 0 && decLen != 0) {
                    // DTLS error — connection may be broken
                    if (!dtls_.isEstablished()) {
                        LOGE("ConnectionManager: DTLS session lost");
                        notifyDart("onDisconnected", "dtls_error");
                        running_ = false;
                        connected_ = false;
                        break;
                    }
                }
            }
            // Non-DTLS data during encrypted session is ignored
        }

        LOGI("ConnectionManager: encrypted recv thread ended");
    }

    void recvThreadUnencrypted() {
        LOGI("ConnectionManager: unencrypted recv thread started");
        uint8_t buf[2048];

        while (running_) {
            int sock = socket_;
            if (sock < 0) break;

            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            int pollRet = poll(&pfd, 1, 1000);

            if (pollRet <= 0) {
                if (!running_) break;
                // Check for timeout
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - lastPacketTime_).count();
                if (elapsed >= TIMEOUT_SECONDS) {
                    LOGI("ConnectionManager: server timeout (%lld seconds)", (long long)elapsed);
                    notifyDart("onDisconnected", "timeout");
                    running_ = false;
                    connected_ = false;
                    break;
                }
                continue;
            }

            sock = socket_;
            if (sock < 0) break;

            struct sockaddr_in fromAddr;
            socklen_t fromLen = sizeof(fromAddr);
            ssize_t received = recvfrom(sock, buf, sizeof(buf), 0,
                                        (struct sockaddr*)&fromAddr, &fromLen);
            if (received <= 0) continue;

            dispatchDecrypted(buf, received);
        }

        LOGI("ConnectionManager: unencrypted recv thread ended");
    }

    void dispatchDecrypted(const uint8_t* data, int length) {
        if (length < 2) return;

        uint8_t version = data[0];
        uint8_t type = data[1];
        if (version != 0x01) return;

        lastPacketTime_ = std::chrono::steady_clock::now();

        switch (type) {
            case PACKET_AUDIO:
                if (player_) {
                    player_->feedPacket(data, length);
                }
                break;

            case PACKET_KEEPALIVE:
                // Just a heartbeat, nothing to do
                break;

            case PACKET_CONTROL:
                if (length >= 17 && data[16] == 0x03) {
                    // Server disconnect
                    LOGI("ConnectionManager: server sent disconnect");
                    notifyDart("onDisconnected", "server_disconnect");
                    running_ = false;
                    connected_ = false;
                }
                break;

            case PACKET_MEDIA_INFO:
                if (length > HEADER_SIZE) {
                    std::string json(reinterpret_cast<const char*>(data + HEADER_SIZE),
                                     length - HEADER_SIZE);
                    notifyDart("onMediaInfo", json.c_str());
                }
                break;

            case PACKET_PING: {
                // Server sent PING, respond with PONG echoing the header
                uint8_t pong[16];
                memset(pong, 0, sizeof(pong));
                pong[0] = 0x01;
                pong[1] = PACKET_PONG;
                // Copy seq and timestamp from ping
                if (length >= 16) {
                    memcpy(pong + 2, data + 2, 12);
                }
                sendEncrypted(pong, 16);
                break;
            }

            case PACKET_PONG: {
                // Response to our PING
                if (pingPending_) {
                    auto now = std::chrono::steady_clock::now();
                    double rtt = std::chrono::duration<double, std::milli>(now - lastPingSent_).count();
                    double current = rttMs_.load();
                    if (current == 0.0) {
                        rttMs_ = rtt;
                    } else {
                        rttMs_ = current * 0.7 + rtt * 0.3; // EWMA
                    }
                    pingPending_ = false;
                }
                break;
            }

            case PACKET_SETTINGS: {
                if (length >= HEADER_SIZE + 2) {
                    uint16_t jitterMs;
                    memcpy(&jitterMs, data + HEADER_SIZE, 2);
                    LOGI("Received server settings: jitter=%dms", jitterMs);
                    if (player_) {
                        player_->setJitterTarget(jitterMs);
                    }
                    serverJitterMs_ = jitterMs;
                    // Notify Dart of the settings change
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d", jitterMs);
                    notifyDart("onSettingsUpdate", buf);
                }
                break;
            }

            default:
                break;
        }
    }

    void notifyDart(const char* method, const char* arg) {
        if (!jvm_ || !callbackObj_) return;

        JNIEnv* env = nullptr;
        bool needDetach = false;
        int getEnvResult = jvm_->GetEnv((void**)&env, JNI_VERSION_1_6);

        if (getEnvResult == JNI_EDETACHED) {
            if (jvm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                LOGE("ConnectionManager: failed to attach thread to JVM");
                return;
            }
            needDetach = true;
        } else if (getEnvResult != JNI_OK) {
            LOGE("ConnectionManager: GetEnv failed");
            return;
        }

        jclass cls = env->GetObjectClass(callbackObj_);
        if (cls) {
            jmethodID mid = env->GetMethodID(cls, "onNativeEvent",
                                             "(Ljava/lang/String;Ljava/lang/String;)V");
            if (mid) {
                jstring jMethod = env->NewStringUTF(method);
                jstring jArg = env->NewStringUTF(arg);
                env->CallVoidMethod(callbackObj_, mid, jMethod, jArg);
                // Check for exceptions
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe();
                    env->ExceptionClear();
                }
                env->DeleteLocalRef(jMethod);
                env->DeleteLocalRef(jArg);
            }
            env->DeleteLocalRef(cls);
        }

        if (needDetach) {
            jvm_->DetachCurrentThread();
        }
    }

    int socket_;
    struct sockaddr_in serverAddr_;
    std::string serverIp_;
    int serverPort_ = 0;
    std::string deviceId_;
    DtlsSession dtls_;
    std::thread recvThread_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;

    AudioPlayer* player_;

    // RTT
    std::atomic<double> rttMs_;
    std::chrono::steady_clock::time_point lastPingSent_;
    bool pingPending_;
    std::atomic<int> serverJitterMs_;

    // Timeout detection — disconnect if no packets for 5 seconds
    static constexpr int TIMEOUT_SECONDS = 5;
    std::chrono::steady_clock::time_point lastPacketTime_;

    // PSK received from server on first pairing
    std::string receivedPsk_;

    // JNI callback
    JavaVM* jvm_;
    jobject callbackObj_;
};

// Global instances
static AudioPlayer g_player;
static AudioRecorder g_recorder;
static ConnectionManager g_connMgr;

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_connMgr.setJvm(vm);
    return JNI_VERSION_1_6;
}

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

    // Recording now uses ConnectionManager's DTLS session for sending.
    // serverIp and port are ignored — the connection is already established.
    // We just need to set the send callback and start the Oboe recording stream.

    g_connMgr.setRecorderSendCallback(&g_recorder);
    bool result = g_recorder.start();

    // Opening an input stream can disrupt the output stream on some devices.
    // Force-restart the player to ensure playback continues.
    if (result) {
        LOGI("Restarting player after recording start to ensure playback");
        g_player.stop();
        g_player.start();
    }

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStopRecording(
        JNIEnv* env, jobject thiz) {
    g_recorder.stop();
    g_connMgr.clearRecorderSendCallback(&g_recorder);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSetMuted(
        JNIEnv* env, jobject thiz, jboolean muted) {
    g_player.setMuted(muted == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSetVolume(
        JNIEnv* env, jobject thiz, jfloat volume) {
    g_player.setVolume(volume);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetLatencyBreakdown(
        JNIEnv* env, jobject thiz) {
    jdoubleArray result = env->NewDoubleArray(2);
    double values[2] = {
        g_player.getLatency(),        // [0] jitter buffer ms
        g_player.getOutputBufferMs()  // [1] output buffer ms
    };
    env->SetDoubleArrayRegion(result, 0, 2, values);
    return result;
}

JNIEXPORT jfloat JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetOutputPeakLevel(
        JNIEnv* env, jobject thiz) {
    return g_player.getPeakLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetInputPeakLevel(
        JNIEnv* env, jobject thiz) {
    return g_recorder.getPeakLevel();
}

// --- ConnectionManager JNI methods ---

JNIEXPORT jstring JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeConnect(
        JNIEnv* env, jobject thiz,
        jstring serverIp, jint serverPort,
        jstring deviceName, jstring deviceId,
        jstring pskHex) {

    // Store callback object for Dart notifications
    g_connMgr.setCallbackObj(env, thiz);
    g_connMgr.setAudioPlayer(&g_player);

    const char* ipStr = env->GetStringUTFChars(serverIp, nullptr);
    const char* nameStr = env->GetStringUTFChars(deviceName, nullptr);
    const char* idStr = env->GetStringUTFChars(deviceId, nullptr);
    const char* pskStr = env->GetStringUTFChars(pskHex, nullptr);

    std::string result = g_connMgr.connect(ipStr, serverPort, nameStr, idStr, pskStr);

    env->ReleaseStringUTFChars(serverIp, ipStr);
    env->ReleaseStringUTFChars(deviceName, nameStr);
    env->ReleaseStringUTFChars(deviceId, idStr);
    env->ReleaseStringUTFChars(pskHex, pskStr);

    return env->NewStringUTF(result.c_str());
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeDisconnect(
        JNIEnv* env, jobject thiz) {
    g_connMgr.clearRecorderSendCallback(&g_recorder);
    g_connMgr.disconnect();
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSendControl(
        JNIEnv* env, jobject thiz, jint cmd) {
    g_connMgr.sendControl(static_cast<uint8_t>(cmd));
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSendKeepalive(
        JNIEnv* env, jobject thiz) {
    g_connMgr.sendKeepalive();
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSendPing(
        JNIEnv* env, jobject thiz) {
    g_connMgr.sendPing();
}

JNIEXPORT jdouble JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetRtt(
        JNIEnv* env, jobject thiz) {
    return g_connMgr.getRtt();
}

JNIEXPORT jboolean JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeIsConnected(
        JNIEnv* env, jobject thiz) {
    return g_connMgr.isConnected() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeIsDtlsActive(
        JNIEnv* env, jobject thiz) {
    return g_connMgr.isDtlsActive() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
