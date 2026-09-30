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
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "playback_frame.h"

#define LOG_TAG "AudioBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Audio constants matching spec
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int FRAME_SIZE = 480; // 10ms at 48kHz
static constexpr int SAMPLES_PER_FRAME = FRAME_SIZE * CHANNELS;
static std::atomic<int> g_frameSizeMs{10}; // server-synced frame duration
static constexpr int HEADER_SIZE = 16;
static constexpr uint8_t PACKET_AUDIO = 0x01;
static constexpr uint8_t PACKET_MIC_AUDIO = 0x04;

// Jitter buffer constants — aggressive low-latency mode
static constexpr int MIN_BUFFER_FRAMES = 0;  // 0 = pass-through, play immediately
static constexpr int MAX_BUFFER_FRAMES = 3;  // 30ms max — accept some glitches over huge latency
static constexpr int INITIAL_BUFFER_FRAMES = 1; // 10ms — single-frame startup


class JitterBuffer {
public:
    JitterBuffer() : targetFrames_(INITIAL_BUFFER_FRAMES), underruns_(0), earlyCount_(0) {}

    void push(const AudioFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Track sequence gaps (missing packets)
        if (lastPushSeq_ > 0 && frame.sequence > lastPushSeq_ + 1) {
            uint32_t missed = frame.sequence - lastPushSeq_ - 1;
            totalMissedPackets_ += missed;
            if (totalMissedPackets_ <= 20 || (totalMissedPackets_ % 50) == 0) {
                LOGI("[JITTER] SEQ GAP: expected %u, got %u (missed %u packets, total missed=%llu)",
                     lastPushSeq_ + 1, frame.sequence, missed, (unsigned long long)totalMissedPackets_);
            }
        }
        lastPushSeq_ = frame.sequence;
        totalPushes_++;

        // Insert sorted by sequence number, drop duplicates
        if (!frames_.empty() && frame.sequence <= frames_.back().sequence) {
            if (frame.sequence < frames_.front().sequence) {
                totalDroppedOld_++;
                return;
            }
            for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                if (it->sequence == frame.sequence) { totalDuplicates_++; return; }
                if (it->sequence > frame.sequence) {
                    frames_.insert(it, frame);
                    totalReordered_++;
                    return;
                }
            }
            return;
        }
        frames_.push_back(frame);

        // Hard cap at 80 frames (800ms) regardless of target — prevents unbounded growth
        // from clock drift while absorbing large Wi-Fi bursts without dropping packets.
        // targetFrames_ only controls priming (fast startup after a gap), not this cap.
        static constexpr size_t MAX_ABSOLUTE_FRAMES = 80;
        while (frames_.size() > MAX_ABSOLUTE_FRAMES) {
            frames_.pop_front();
            totalOverflows_++;
        }
    }

    bool pop(AudioFrame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            underruns_++;
            totalUnderruns_++;
            primed_ = false; // Re-prime the buffer up to targetFrames_ to survive subsequent jitter
            return false;
        }

        if (!primed_ && targetFrames_ > 0 && frames_.size() < static_cast<size_t>(targetFrames_)) {
            return false;
        }
        primed_ = true;

        out = frames_.front();
        frames_.pop_front();
        totalPops_++;

        underruns_ = 0;
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        // Do NOT reset targetFrames_ here; the server controls it explicitly
        underruns_ = 0;
        earlyCount_ = 0;
        primed_ = false;
        lastPushSeq_ = 0;
        totalPushes_ = 0;
        totalPops_ = 0;
        totalUnderruns_ = 0;
        totalMissedPackets_ = 0;
        totalDuplicates_ = 0;
        totalReordered_ = 0;
        totalDroppedOld_ = 0;
        totalOverflows_ = 0;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(frames_.size());
    }

    int getTargetFrames() const { return targetFrames_; }

    void setTargetFrames(int frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames < MIN_BUFFER_FRAMES) frames = MIN_BUFFER_FRAMES;
        const int frameMs = std::max(5, g_frameSizeMs.load());
        const int maxFrames = (150 + frameMs - 1) / frameMs;
        if (frames > maxFrames) frames = maxFrames;
        targetFrames_ = frames;
        LOGI("[JITTER] Server set explicit target to %d frames (%d ms)", frames, frames * frameMs);
    }

    void logStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        LOGI("[JITTER] Stats: size=%zu, target=%d, pushes=%llu, pops=%llu, "
             "underruns=%llu, missed=%llu, dupes=%llu, reorder=%llu, "
             "dropped_old=%llu, overflows=%llu",
             frames_.size(), targetFrames_,
             (unsigned long long)totalPushes_, (unsigned long long)totalPops_,
             (unsigned long long)totalUnderruns_, (unsigned long long)totalMissedPackets_,
             (unsigned long long)totalDuplicates_, (unsigned long long)totalReordered_,
             (unsigned long long)totalDroppedOld_, (unsigned long long)totalOverflows_);
    }

private:
    std::deque<AudioFrame> frames_;
    std::mutex mutex_;
    int targetFrames_;
    int underruns_;
    int earlyCount_;
    bool primed_ = false;
    uint32_t lastPushSeq_ = 0;

    // Cumulative diagnostic counters
    uint64_t totalPushes_ = 0;
    uint64_t totalPops_ = 0;
    uint64_t totalUnderruns_ = 0;
    uint64_t totalMissedPackets_ = 0;
    uint64_t totalDuplicates_ = 0;
    uint64_t totalReordered_ = 0;
    uint64_t totalDroppedOld_ = 0;
    uint64_t totalOverflows_ = 0;
};

class AudioPlayer : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioPlayer() : decoder_(nullptr), running_(false), latencyMs_(0.0), muted_(false), volume_(1.0f) {}

    void setMuted(bool muted) { muted_.store(muted); }
    void setVolume(float vol) { volume_.store(vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol)); }

    void setJitterBufferMs(int jitterMs) {
        int fms = g_frameSizeMs.load();
        if (fms <= 0) fms = 10;
        int frames = (jitterMs + fms - 1) / fms;
        jitterBuffer_.setTargetFrames(frames);
    }

    bool start() {
        int error;
        decoder_ = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
        if (error != OPUS_OK || !decoder_) {
            LOGE("Failed to create Opus decoder: %s", opus_strerror(error));
            return false;
        }

        jitterBuffer_.reset();
        playbackFrame_.reset();

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

        // Track feed timing
        auto now = std::chrono::steady_clock::now();
        double feedGapMs = std::chrono::duration<double, std::milli>(now - lastFeedTime_).count();
        lastFeedTime_ = now;
        feedCount_++;

        if (feedGapMs > 15.0 && feedCount_ > 1) {
            largeFeedGapCount_++;
            if (largeFeedGapCount_ <= 20 || (largeFeedGapCount_ % 50) == 0) {
                LOGI("[FEED] LARGE GAP #%llu: %.1fms between feeds (seq=%u, jbuf=%d)",
                     (unsigned long long)largeFeedGapCount_, feedGapMs, seq, jitterBuffer_.size());
            }
        }

        // Decode Opus  
        AudioFrame frame;
        frame.sequence = seq;
        frame.timestamp = timestamp;

        auto beforeDecode = std::chrono::steady_clock::now();
        int decoded = opus_decode(decoder_, opusData, payloadLen,
                                  frame.samples, AudioFrame::maxFrames, 0);
        auto afterDecode = std::chrono::steady_clock::now();
        double decodeMs = std::chrono::duration<double, std::milli>(afterDecode - beforeDecode).count();

        if (decoded <= 0) {
            LOGE("Opus decode error: %s (payloadLen=%u)", opus_strerror(decoded), payloadLen);
            decodeErrors_++;
            return;
        }
        frame.frameCount = decoded;

        if (decodeMs > 2.0) {
            slowDecodes_++;
            if (slowDecodes_ <= 10 || (slowDecodes_ % 50) == 0) {
                LOGI("[FEED] SLOW decode #%llu: %.2fms", (unsigned long long)slowDecodes_, decodeMs);
            }
        }

        double bufferLatency = jitterBuffer_.size() * (double)g_frameSizeMs.load();
        latencyMs_.store(bufferLatency);

        jitterBuffer_.push(frame);

        // Periodic feed stats (every 500 packets = ~5 seconds)
        if (feedCount_ <= 5 || (feedCount_ % 500) == 0) {
            LOGI("[FEED] #%llu: seq=%u, opusLen=%u, decoded=%d, "
                 "decodeMs=%.2f, gap=%.1fms, jbuf=%d, "
                 "largeGaps=%llu, slowDec=%llu, decErr=%llu",
                 (unsigned long long)feedCount_, seq, payloadLen, decoded,
                 decodeMs, feedGapMs, jitterBuffer_.size(),
                 (unsigned long long)largeFeedGapCount_,
                 (unsigned long long)slowDecodes_,
                 (unsigned long long)decodeErrors_);
            jitterBuffer_.logStats();
        }
    }

    double getLatency() const {
        return latencyMs_.load();
    }

    double getOutputBufferMs() const {
        if (!stream_) return 0.0;
        return stream_->getBufferSizeInFrames() * 1000.0 / SAMPLE_RATE;
    }

    float getOutputPeakLevel() {
        int16_t peak = outputPeak_;
        return peak / 32768.0f;
    }

    // Oboe callback
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream,
            void* audioData,
            int32_t numFrames) override {

        auto* output = static_cast<int16_t*>(audioData);
        auto callbackStart = std::chrono::steady_clock::now();

        // Track callback timing
        double callbackGapMs = std::chrono::duration<double, std::milli>(
            callbackStart - lastCallbackTime_).count();
        lastCallbackTime_ = callbackStart;
        callbackCount_++;

        if (callbackGapMs > 15.0 && callbackCount_ > 1) {
            largeCallbackGapCount_++;
            if (largeCallbackGapCount_ <= 20 || (largeCallbackGapCount_ % 50) == 0) {
                LOGI("[OBOE] LARGE callback gap #%llu: %.1fms (expected ~10ms, numFrames=%d, jbuf=%d)",
                     (unsigned long long)largeCallbackGapCount_, callbackGapMs,
                     numFrames, jitterBuffer_.size());
            }
        }

        if (!running_) {
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
            return oboe::DataCallbackResult::Stop;
        }

        // If muted, output silence but still consume jitter buffer
        if (muted_.load()) {
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
            playbackFrame_.reset();
            AudioFrame discard;
            while (jitterBuffer_.pop(discard)) {}
            return oboe::DataCallbackResult::Continue;
        }

        int framesWritten = 0;
        int underrunsThisCallback = 0;
        int plcFramesThisCallback = 0;

        while (framesWritten < numFrames) {
            int framesToWrite = std::min(FRAME_SIZE, numFrames - framesWritten);

            if (playbackFrame_.empty()) {
                playbackFrame_.reset();
                jitterBuffer_.pop(playbackFrame_.frame);
            }
            if (!playbackFrame_.empty()) {
                framesToWrite = playbackFrame_.copyTo(
                    output + framesWritten * CHANNELS, numFrames - framesWritten);

                // Track peak output level for diagnostics
                for (int i = 0; i < framesToWrite * CHANNELS; i++) {
                    int16_t s = output[framesWritten * CHANNELS + i];
                    int16_t abs_s = s < 0 ? -s : s;
                    if (abs_s > outputPeak_) outputPeak_ = abs_s;
                }
            } else {
                underrunsThisCallback++;
                totalPlaybackUnderruns_++;

                // Use Opus PLC if decoder available
                if (decoder_) {
                    int16_t plcSamples[SAMPLES_PER_FRAME];
                    int decoded = opus_decode(decoder_, nullptr, 0, plcSamples, FRAME_SIZE, 0);
                    if (decoded > 0) {
                        int copyFrames = std::min(decoded, framesToWrite);
                        memcpy(output + framesWritten * CHANNELS,
                               plcSamples,
                               copyFrames * CHANNELS * sizeof(int16_t));
                        plcFramesThisCallback++;
                        totalPlcFrames_++;
                    } else {
                        memset(output + framesWritten * CHANNELS, 0,
                               framesToWrite * CHANNELS * sizeof(int16_t));
                        totalSilenceFrames_++;
                    }
                } else {
                    memset(output + framesWritten * CHANNELS, 0,
                           framesToWrite * CHANNELS * sizeof(int16_t));
                    totalSilenceFrames_++;
                }
            }
            framesWritten += framesToWrite;
        }

        // Log underruns (these are the pops!)
        if (underrunsThisCallback > 0) {
            if (totalPlaybackUnderruns_ <= 30 || (totalPlaybackUnderruns_ % 50) == 0) {
                LOGI("[OBOE] *** UNDERRUN #%llu in callback #%llu: %d sub-frames empty, "
                     "plc=%d, jbuf=%d, gap=%.1fms — THIS CAUSES A POP",
                     (unsigned long long)totalPlaybackUnderruns_,
                     (unsigned long long)callbackCount_,
                     underrunsThisCallback, plcFramesThisCallback,
                     jitterBuffer_.size(), callbackGapMs);
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

        // Periodic Oboe callback stats (every ~3 seconds)
        if (callbackCount_ <= 3 || (callbackCount_ % 300) == 0) {
            auto callbackEnd = std::chrono::steady_clock::now();
            double callbackDurationMs = std::chrono::duration<double, std::milli>(
                callbackEnd - callbackStart).count();
            LOGI("[OBOE] Callback #%llu: numFrames=%d, gap=%.1fms, duration=%.2fms, "
                 "jbuf=%d, peak=%d, underruns=%llu, plc=%llu, silence=%llu, "
                 "largeGaps=%llu",
                 (unsigned long long)callbackCount_, numFrames,
                 callbackGapMs, callbackDurationMs,
                 jitterBuffer_.size(), (int)outputPeak_,
                 (unsigned long long)totalPlaybackUnderruns_,
                 (unsigned long long)totalPlcFrames_,
                 (unsigned long long)totalSilenceFrames_,
                 (unsigned long long)largeCallbackGapCount_);
            outputPeak_ = 0;
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
    PlaybackFrame playbackFrame_;
    std::atomic<bool> running_;
    std::atomic<double> latencyMs_;
    std::atomic<bool> muted_;
    std::atomic<float> volume_;

    // Feed path diagnostics
    std::chrono::steady_clock::time_point lastFeedTime_ = std::chrono::steady_clock::now();
    uint64_t feedCount_ = 0;
    uint64_t largeFeedGapCount_ = 0;
    uint64_t slowDecodes_ = 0;
    uint64_t decodeErrors_ = 0;

    // Oboe callback diagnostics
    std::chrono::steady_clock::time_point lastCallbackTime_ = std::chrono::steady_clock::now();
    uint64_t callbackCount_ = 0;
    uint64_t largeCallbackGapCount_ = 0;
    uint64_t totalPlaybackUnderruns_ = 0;
    uint64_t totalPlcFrames_ = 0;
    uint64_t totalSilenceFrames_ = 0;
    int16_t outputPeak_ = 0;
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

// --- ConnectionManager (DTLS + Connection Protocol) ---

#include "dtls_session.h"
#include <mbedtls/error.h>

// Global instances — declared before ConnectionManager so it can use g_player
static AudioPlayer g_player;
static AudioRecorder g_recorder;

static constexpr uint8_t PACKET_KEEPALIVE = 0x02;
static constexpr uint8_t PACKET_CONTROL = 0x03;
static constexpr uint8_t PACKET_MEDIA_INFO = 0x05;
static constexpr uint8_t PACKET_PING = 0x06;
static constexpr uint8_t PACKET_PONG = 0x07;
static constexpr uint8_t PACKET_SETTINGS = 0x08;

static constexpr uint8_t CTRL_DISCONNECT = 0x03;

// Forward reference for JNI callback
static JavaVM* g_jvm = nullptr;
static jobject g_mainActivity = nullptr;

class ConnectionManager {
public:
    ConnectionManager() {}

    ~ConnectionManager() {
        disconnect();
    }

    // Blocking connect — runs on background thread from JNI
    std::string connect(const char* serverIp, int serverPort,
                        const char* deviceName, const char* deviceId,
                        const char* pskHex) {
        LOGI("[CONN] Connecting to %s:%d as '%s' (id=%s, psk=%s)",
             serverIp, serverPort, deviceName, deviceId,
             strlen(pskHex) > 0 ? "present" : "empty");

        // Create UDP socket
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) {
            LOGE("[CONN] Failed to create socket");
            return "error|socket_failed";
        }

        memset(&serverAddr_, 0, sizeof(serverAddr_));
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) <= 0) {
            LOGE("[CONN] Invalid server IP: %s", serverIp);
            close(socket_);
            socket_ = -1;
            return "error|invalid_ip";
        }

        // Set receive timeout for blocking recvfrom
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Send AB_CONNECT
        std::string connectMsg = std::string("AB_CONNECT|") + deviceName + "|" + deviceId;
        sendto(socket_, connectMsg.c_str(), connectMsg.size(), 0,
               (struct sockaddr*)&serverAddr_, sizeof(serverAddr_));
        LOGI("[CONN] Sent AB_CONNECT (%zu bytes)", connectMsg.size());

        // Wait for response
        char buf[2048];
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        int received = recvfrom(socket_, buf, sizeof(buf) - 1, 0,
                                (struct sockaddr*)&fromAddr, &fromLen);

        if (received <= 0) {
            LOGE("[CONN] No response from server (timeout)");
            close(socket_);
            socket_ = -1;
            return "timeout";
        }

        buf[received] = '\0';
        LOGI("[CONN] Got response: '%.*s' (%d bytes)", std::min(received, 100), buf, received);

        if (strncmp(buf, "AB_REJECT", 9) == 0) {
            std::string reason = received > 10 ? std::string(buf + 10) : "rejected";
            close(socket_);
            socket_ = -1;
            return "rejected|" + reason;
        }

        if (strncmp(buf, "AB_PAIR_PENDING", 15) == 0) {
            LOGI("[CONN] Pair pending, notifying UI and waiting...");
            // Notify Dart via JNI callback
            notifyDart("onPairPending", "");

            // Wait up to 35 seconds for the actual accept/reject
            tv.tv_sec = 35;
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            received = recvfrom(socket_, buf, sizeof(buf) - 1, 0,
                                (struct sockaddr*)&fromAddr, &fromLen);
            if (received <= 0) {
                close(socket_);
                socket_ = -1;
                return "pair_pending";
            }
            buf[received] = '\0';
            LOGI("[CONN] After pair pending, got: '%.*s'", std::min(received, 100), buf);

            if (strncmp(buf, "AB_REJECT", 9) == 0) {
                std::string reason = received > 10 ? std::string(buf + 10) : "rejected";
                close(socket_);
                socket_ = -1;
                return "rejected|" + reason;
            }
        }

        if (strncmp(buf, "AB_ACCEPT", 9) != 0) {
            LOGE("[CONN] Unexpected response: %s", buf);
            close(socket_);
            socket_ = -1;
            return "error|unexpected_response";
        }

        // Extract PSK if provided (AB_ACCEPT|<base64_psk>)
        std::string newPskHex;
        if (received > 10 && buf[9] == '|') {
            std::string b64Psk(buf + 10, received - 10);
            // Trim whitespace
            while (!b64Psk.empty() && (b64Psk.back() == '\n' || b64Psk.back() == '\r' || b64Psk.back() == '\0'))
                b64Psk.pop_back();
            newPskHex = DtlsSession::base64ToPskHex(b64Psk);
            LOGI("[CONN] Got PSK from server (hex len=%zu)", newPskHex.size());
        }

        // Determine which PSK to use for DTLS
        std::string activePsk = newPskHex.empty() ? std::string(pskHex) : newPskHex;

        // DTLS disabled: the server completes its handshake before the client can,
        // then floods the client with encrypted app-data (type=0x01) which the client
        // sees instead of handshake records (type=0x16).  Until this race is fixed,
        // connect unencrypted so both sides agree on the same protocol.
        if (false && !activePsk.empty()) {

            // Attempt DTLS handshake
            LOGI("[CONN] Starting DTLS handshake...");

            // Switch to short timeout for DTLS
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            try {
                dtlsSession_ = std::make_unique<DtlsSession>();
                auto pskBytes = DtlsSession::hexToBytes(activePsk);
                if (!dtlsSession_->initClient(socket_, serverAddr_, std::string(deviceId), pskBytes)) {
                    LOGE("[CONN] DTLS init failed");
                    dtlsSession_.reset();
                } else {
                    // Drive handshake — single-threaded push model:
                    // 1. continueHandshake() sends outgoing records via bioSend
                    // 2. recvfrom() collects the server's reply from the socket
                    // 3. pushReceivedData() feeds it to mbedtls via bioRecvNonBlocking
                    // bioRecvNonBlocking is always non-blocking so we never deadlock here.
                    auto handshakeStart = std::chrono::steady_clock::now();
                    bool handshakeOk = false;
                    int totalRecvd = 0;
                    int totalWantRead = 0;

                    LOGI("[CONN] DTLS handshake loop starting (psk=%zu bytes)", pskBytes.size());

                    for (int attempt = 0; attempt < 500; attempt++) {
                        int ret = dtlsSession_->continueHandshake();
                        if (ret == 0) {
                            handshakeOk = true;
                            break;
                        }
                        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                            char errBuf[128];
                            mbedtls_strerror(ret, errBuf, sizeof(errBuf));
                            LOGE("[CONN] DTLS handshake fatal error at attempt %d: -0x%04x (%s)",
                                 attempt, -ret, errBuf);
                            break;
                        }
                        if (ret == MBEDTLS_ERR_SSL_WANT_READ) totalWantRead++;

                        // Feed incoming DTLS records from the socket
                        char dtlsBuf[2048];
                        int n = recvfrom(socket_, dtlsBuf, sizeof(dtlsBuf), 0,
                                         (struct sockaddr*)&fromAddr, &fromLen);
                        if (n > 0) {
                            totalRecvd++;
                            LOGI("[CONN] DTLS attempt %d: recvfrom got %d bytes (type=0x%02x), feeding to mbedtls",
                                 attempt, n, (uint8_t)dtlsBuf[0]);
                            dtlsSession_->pushReceivedData((const uint8_t*)dtlsBuf, n);
                        } else if (n < 0) {
                            // EAGAIN/timeout — expected, just loop
                            if (attempt % 20 == 0) {
                                LOGI("[CONN] DTLS attempt %d: no data yet (wantRead=%d, totalRecvd=%d)",
                                     attempt, totalWantRead, totalRecvd);
                            }
                        }

                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - handshakeStart).count();
                        if (elapsed > 10) {
                            LOGE("[CONN] DTLS handshake timeout (>10s) after %d attempts, recvd=%d packets",
                                 attempt, totalRecvd);
                            break;
                        }
                    }

                    if (handshakeOk) {
                        dtlsActive_ = true;
                        LOGI("[CONN] DTLS handshake complete — encrypted");
                    } else {
                        LOGE("[CONN] DTLS handshake failed, falling back to unencrypted");
                        try {
                            if (dtlsSession_) {
                                dtlsSession_->teardown();
                            }
                        } catch (...) {
                            LOGE("[CONN] Exception during DTLS teardown (ignored)");
                        }
                        dtlsSession_.reset();
                    }
                }
            } catch (const std::exception& e) {
                LOGE("[CONN] DTLS exception: %s", e.what());
                try { dtlsSession_.reset(); } catch (...) {}
            } catch (...) {
                LOGE("[CONN] Unknown DTLS exception");
                try { dtlsSession_.reset(); } catch (...) {}
            }
        }

        // Connection established
        connected_ = true;
        streamStart_ = std::chrono::steady_clock::now();
        lastServerPacket_ = streamStart_;
        sequence_ = 0;

        // Switch to short timeout for recv thread
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Start recv thread
        recvThread_ = std::thread(&ConnectionManager::recvLoop, this);

        LOGI("[CONN] Connected! dtls=%s", dtlsActive_ ? "YES" : "NO");

        return newPskHex.empty() ? "accepted" : ("accepted|" + newPskHex);
    }

    void disconnect() {
        if (!connected_) return;
        LOGI("[CONN] Disconnecting...");

        // Send disconnect control
        if (connected_) {
            uint8_t packet[17];
            writeHeader(packet, PACKET_CONTROL, 1);
            packet[16] = CTRL_DISCONNECT;
            sendPacket(packet, 17);
        }

        connected_ = false;

        if (recvThread_.joinable()) {
            recvThread_.join();
        }

        if (dtlsSession_) {
            dtlsSession_->teardown();
            dtlsSession_.reset();
        }
        dtlsActive_ = false;

        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }

        LOGI("[CONN] Disconnected");
    }

    void sendControl(uint8_t cmd) {
        if (!connected_) return;
        uint8_t packet[17];
        writeHeader(packet, PACKET_CONTROL, 1);
        packet[16] = cmd;
        sendPacket(packet, 17);
    }

    void sendKeepalive() {
        if (!connected_) return;
        uint8_t packet[16];
        writeHeader(packet, PACKET_KEEPALIVE, 0);
        sendPacket(packet, 16);
    }

    void sendPing() {
        if (!connected_) return;
        uint8_t packet[16];
        writeHeader(packet, PACKET_PING, 0);
        lastPingSent_ = std::chrono::steady_clock::now();
        pingPending_ = true;
        sendPacket(packet, 16);
    }

    double getRtt() const { return rttMs_.load(); }
    bool isConnected() const { return connected_.load(); }
    bool isDtlsActive() const { return dtlsActive_.load(); }

private:
    void writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen) {
        auto now = std::chrono::steady_clock::now();
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            now - streamStart_).count();
        buf[0] = 0x01;
        buf[1] = type;
        uint32_t seq = sequence_++;
        memcpy(buf + 2, &seq, 4);
        memcpy(buf + 6, &timestamp, 8);
        memcpy(buf + 14, &payloadLen, 2);
    }

    void sendPacket(const uint8_t* data, size_t len) {
        if (dtlsActive_ && dtlsSession_) {
            std::lock_guard<std::mutex> lock(dtlsMutex_);
            dtlsSession_->send(data, len);
        } else {
            sendto(socket_, data, len, 0,
                   (struct sockaddr*)&serverAddr_, sizeof(serverAddr_));
        }
    }

    void notifyDart(const char* method, const char* data) {
        if (!g_jvm || !g_mainActivity) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            g_jvm->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
        if (env) {
            jclass cls = env->GetObjectClass(g_mainActivity);
            jmethodID mid = env->GetMethodID(cls, "onNativeEvent",
                "(Ljava/lang/String;Ljava/lang/String;)V");
            if (!mid) {
                env->ExceptionClear();
                LOGE("notifyDart: MainActivity.onNativeEvent not found");
            } else {
                jstring jMethod = env->NewStringUTF(method);
                jstring jData = env->NewStringUTF(data);
                env->CallVoidMethod(g_mainActivity, mid, jMethod, jData);
                env->DeleteLocalRef(jMethod);
                env->DeleteLocalRef(jData);
            }
            env->DeleteLocalRef(cls);
        }
        if (attached) g_jvm->DetachCurrentThread();
    }

    void recvLoop() {
        LOGI("[RECV] Thread started");
        // Boost receive thread priority to AUDIO level (-16) so Android's scheduler
        // doesn't preempt us for 50-100ms during a UI frame, causing packet gaps.
        struct sched_param sp;
        sp.sched_priority = 0;
        if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) != 0) {
            // SCHED_RR requires CAP_SYS_NICE, fall back to nice value
            setpriority(PRIO_PROCESS, 0, -16); // ANDROID_PRIORITY_AUDIO
        }
        char buf[2048];
        struct sockaddr_in fromAddr;
        socklen_t fromLen;

        while (connected_) {
            fromLen = sizeof(fromAddr);
            int received = recvfrom(socket_, buf, sizeof(buf), 0,
                                     (struct sockaddr*)&fromAddr, &fromLen);

            if (received <= 0) {
                // Check timeout
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - lastServerPacket_).count();
                if (elapsed > 5000) {
                    LOGI("[RECV] Server timeout (%lldms)", (long long)elapsed);
                    connected_ = false;
                    notifyDart("onDisconnected", "timeout");
                    break;
                }
                continue;
            }

            lastServerPacket_ = std::chrono::steady_clock::now();
            uint8_t firstByte = (uint8_t)buf[0];

            // DTLS record
            if (dtlsActive_ && dtlsSession_ && firstByte >= 20 && firstByte <= 25) {
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    dtlsSession_->pushReceivedData((const uint8_t*)buf, received);
                }

                uint8_t plainBuf[2048];
                int n;
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    n = dtlsSession_->recv(plainBuf, sizeof(plainBuf));
                }

                if (n > 0) {
                    processPacket(plainBuf, n);
                } else if (n < 0 && n != MBEDTLS_ERR_SSL_WANT_READ) {
                    LOGE("[RECV] DTLS recv error: -0x%04x", -n);
                    connected_ = false;
                    notifyDart("onDisconnected", "dtls_error");
                    break;
                }
            } else if (!dtlsActive_ && received >= 16) {
                // Unencrypted packet
                processPacket((const uint8_t*)buf, received);
            }
        }
        LOGI("[RECV] Thread exiting");
    }

    void processPacket(const uint8_t* data, int len) {
        if (len < 16) return;

        uint8_t type = data[1];

        switch (type) {
            case PACKET_AUDIO:
                // Feed to player
                g_player.feedPacket(data, len);
                break;

            case PACKET_KEEPALIVE:
                // Just alive check — lastServerPacket_ already updated
                break;

            case PACKET_CONTROL:
                if (len >= 17) {
                    uint8_t cmd = data[16];
                    if (cmd == CTRL_DISCONNECT) {
                        LOGI("[RECV] Server sent disconnect");
                        connected_ = false;
                        notifyDart("onDisconnected", "server_disconnect");
                    }
                }
                break;

            case PACKET_MEDIA_INFO: {
                uint16_t payloadLen;
                memcpy(&payloadLen, data + 14, 2);
                if (len >= 16 + payloadLen) {
                    std::string json((const char*)data + 16, payloadLen);
                    notifyDart("onMediaInfo", json.c_str());
                }
                break;
            }

            case PACKET_PING: {
                // Respond with pong
                uint8_t pong[16];
                writeHeader(pong, PACKET_PONG, 0);
                memcpy(pong + 6, data + 6, 8); // echo timestamp
                sendPacket(pong, 16);
                break;
            }

            case PACKET_PONG: {
                if (pingPending_) {
                    auto now = std::chrono::steady_clock::now();
                    double rtt = std::chrono::duration<double, std::milli>(
                        now - lastPingSent_).count();
                    pingPending_ = false;
                    double prev = rttMs_.load();
                    rttMs_.store(prev == 0.0 ? rtt : (prev * 0.7 + rtt * 0.3));
                }
                break;
            }

            case PACKET_SETTINGS: {
                if (len >= 18) {
                    uint16_t jitterMs;
                    memcpy(&jitterMs, data + 16, 2);
                    if (len >= 20) {
                        uint16_t frameMs;
                        memcpy(&frameMs, data + 18, 2);
                        if (frameMs == 5 || frameMs == 10 || frameMs == 20) {
                            g_frameSizeMs.store((int)frameMs);
                            LOGI("[RECV] Settings: jitter=%u ms, frameSize=%u ms", jitterMs, frameMs);
                            notifyDart("onFrameSizeUpdate", std::to_string(frameMs).c_str());
                        } else {
                            LOGI("[RECV] Settings: jitter=%u ms", jitterMs);
                        }
                    } else {
                        LOGI("[RECV] Settings: jitter=%u ms", jitterMs);
                    }
                    g_player.setJitterBufferMs(jitterMs);
                    notifyDart("onSettingsUpdate", std::to_string(jitterMs).c_str());
                }
                break;
            }
        }
    }

    int socket_ = -1;
    struct sockaddr_in serverAddr_{};
    std::unique_ptr<DtlsSession> dtlsSession_;
    std::mutex dtlsMutex_;
    std::atomic<bool> dtlsActive_{false};
    std::atomic<bool> connected_{false};
    std::thread recvThread_;
    std::chrono::steady_clock::time_point streamStart_;
    std::chrono::steady_clock::time_point lastServerPacket_;
    std::chrono::steady_clock::time_point lastPingSent_;
    std::atomic<bool> pingPending_{false};
    std::atomic<double> rttMs_{0.0};
    std::atomic<uint32_t> sequence_{0};
};

// Global connection manager instance
static ConnectionManager g_connMgr;

// Save JVM reference for JNI callbacks
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: native_audio loaded");
    return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeStartAudio(
        JNIEnv* env, jobject thiz) {
    // Save activity reference for JNI callbacks
    if (g_mainActivity) env->DeleteGlobalRef(g_mainActivity);
    g_mainActivity = env->NewGlobalRef(thiz);
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

    // Allow Oboe callback to organically handle any necessary restarts on failure.
    // (We removed the force-restart here because it creates a 250ms gap and causes pops).
    if (result) {
        LOGI("Audio recorder successfully started");
    }

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
    return g_player.getOutputPeakLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetInputPeakLevel(
        JNIEnv* env, jobject thiz) {
    // TODO: add mic recording level tracking
    return 0.0f;
}

// --- ConnectionManager JNI ---

JNIEXPORT jstring JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeConnect(
        JNIEnv* env, jobject thiz, jstring serverIp, jint serverPort,
        jstring deviceName, jstring deviceId, jstring pskHex) {
    // Save activity reference
    if (g_mainActivity) env->DeleteGlobalRef(g_mainActivity);
    g_mainActivity = env->NewGlobalRef(thiz);

    const char* ip = env->GetStringUTFChars(serverIp, nullptr);
    const char* name = env->GetStringUTFChars(deviceName, nullptr);
    const char* id = env->GetStringUTFChars(deviceId, nullptr);
    const char* psk = env->GetStringUTFChars(pskHex, nullptr);

    std::string result = g_connMgr.connect(ip, serverPort, name, id, psk);

    env->ReleaseStringUTFChars(serverIp, ip);
    env->ReleaseStringUTFChars(deviceName, name);
    env->ReleaseStringUTFChars(deviceId, id);
    env->ReleaseStringUTFChars(pskHex, psk);

    return env->NewStringUTF(result.c_str());
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeDisconnect(
        JNIEnv* env, jobject thiz) {
    g_connMgr.disconnect();
}

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSendControl(
        JNIEnv* env, jobject thiz, jint cmd) {
    g_connMgr.sendControl((uint8_t)cmd);
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

