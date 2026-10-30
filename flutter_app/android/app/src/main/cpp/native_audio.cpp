#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include <opus.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <limits>
#include <fcntl.h>
#include <unistd.h>
#include "playback_frame.h"

#define LOG_TAG "AudioBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Audio constants matching spec
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int FRAME_SIZE = 480; // 10ms default reference at 48kHz
static constexpr int SAMPLES_PER_FRAME = FRAME_SIZE * CHANNELS;
static std::atomic<int> g_frameSizeMs{2};     // 2 => 2.5ms (120 samples), 5, 10, or 20 ms
static std::atomic<int> g_frameSamples{120};  // exact stereo sample frames per server packet
static std::atomic<bool> g_isRawPcm{true};    // server streaming codec mode (true = Raw PCM)
static constexpr int HEADER_SIZE = 16;
static constexpr uint8_t PACKET_AUDIO = 0x01;
static constexpr uint8_t PACKET_MIC_AUDIO = 0x04;
static constexpr uint8_t PACKET_AUDIO_PCM = 0x09;

// Sample-exact jitter buffer defaults (at 48 kHz stereo)
static constexpr int INITIAL_TARGET_SAMPLE_FRAMES = 480; // 10.0 ms default startup target
static constexpr int MAX_TARGET_SAMPLE_FRAMES = 7200;    // 150.0 ms ceiling


class JitterBuffer {
public:
    JitterBuffer() : targetSampleFrames_(INITIAL_TARGET_SAMPLE_FRAMES), underruns_(0), earlyCount_(0) {}

    void push(const AudioFrame& frame) {
        if (frame.frameCount <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);

        // Track sequence gaps (server increments sequence only for audio packets)
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
                    queuedSamples_ += frame.frameCount;
                    queuedSamplesAtomic_.store(queuedSamples_, std::memory_order_relaxed);
                    totalReordered_++;
                    return;
                }
            }
            return;
        }

        if (primed_ && queuedSamples_ < windowMinSamples_) {
            windowMinSamples_ = queuedSamples_;
        }
        frames_.push_back(frame);
        queuedSamples_ += frame.frameCount;

        const int target = std::max(targetSampleFrames_, 96); // >= 2.0ms floor for windowing

        // Dynamic hard cap (at least 25ms = 1200 frames, or 5x target) — bounds extreme Wi-Fi
        // stall bursts immediately rather than letting hundreds of milliseconds queue up.
        const int maxAbsoluteSamples = std::max(1200, target * 5);
        while (queuedSamples_ > maxAbsoluteSamples && !frames_.empty()) {
            queuedSamples_ -= frames_.front().frameCount;
            frames_.pop_front();
            totalOverflows_++;
        }
        if (queuedSamples_ < 0) queuedSamples_ = 0;

        // Sample-exact 1.0-second (48,000 stereo frames) windowed minimum-depth latency catch-up:
        // Observes the true trough queue depth across a full 1.0s window (after pops and before pushes).
        // Because Windows WASAPI loopback delivers 480-frame (10ms) batches at once, keeping a 2.5ms
        // (120-frame) trough floor when target <= 12.5ms lets the average queue depth track target 1:1
        // instead of stacking +10ms of WASAPI burst depth on top of target.
        if (primed_) {
            windowPushedSamples_ += frame.frameCount;
            if (windowPushedSamples_ >= SAMPLE_RATE) {
                const int troughFloor = std::max(120, target - 480);
                if (windowMinSamples_ > troughFloor && queuedSamples_ > troughFloor) {
                    int excessSamples = windowMinSamples_ - troughFloor;
                    size_t droppedPkts = 0;
                    int droppedSamples = 0;
                    while (!frames_.empty() &&
                           frames_.front().frameCount <= excessSamples &&
                           (queuedSamples_ - frames_.front().frameCount) >= troughFloor) {
                        int fc = frames_.front().frameCount;
                        frames_.pop_front();
                        queuedSamples_ -= fc;
                        excessSamples -= fc;
                        droppedSamples += fc;
                        droppedPkts++;
                    }
                    if (droppedPkts > 0) {
                        totalTrimmed_ += droppedPkts;
                        if (totalTrimmed_ <= 200 || (totalTrimmed_ % 100) == 0) {
                            LOGI("[JITTER] Window catch-up trimmed %zu pkts / %.1fms (winMin=%.1fms, floor=%.1fms, target=%.1fms, total trimmed=%llu)",
                                 droppedPkts, droppedSamples * 1000.0 / SAMPLE_RATE,
                                 windowMinSamples_ * 1000.0 / SAMPLE_RATE,
                                 troughFloor * 1000.0 / SAMPLE_RATE,
                                 target * 1000.0 / SAMPLE_RATE,
                                 (unsigned long long)totalTrimmed_);
                        }
                    }
                }
                resetWindowLocked();
            }
        }

        queuedSamplesAtomic_.store(queuedSamples_, std::memory_order_relaxed);
    }

    // Discard stale packets accumulated while AAudioStreamBuilder_openStream() was opening
    void trimPreStartBacklog() {
        std::lock_guard<std::mutex> lock(mutex_);
        const int keep = std::max(targetSampleFrames_, 240);
        while (!frames_.empty() && (queuedSamples_ - frames_.front().frameCount) >= keep) {
            queuedSamples_ -= frames_.front().frameCount;
            frames_.pop_front();
        }
        if (queuedSamples_ < 0) queuedSamples_ = 0;
        queuedSamplesAtomic_.store(queuedSamples_, std::memory_order_relaxed);
        resetWindowLocked();
    }

    bool pop(AudioFrame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            underruns_++;
            totalUnderruns_++;
            primed_ = false; // Re-prime up to targetSampleFrames_ to survive subsequent jitter
            resetWindowLocked();
            queuedSamples_ = 0;
            queuedSamplesAtomic_.store(0, std::memory_order_relaxed);
            return false;
        }

        if (!primed_ && targetSampleFrames_ > 0 && queuedSamples_ < targetSampleFrames_) {
            return false;
        }
        primed_ = true;

        out = frames_.front();
        frames_.pop_front();
        queuedSamples_ -= out.frameCount;
        if (queuedSamples_ < 0) queuedSamples_ = 0;
        queuedSamplesAtomic_.store(queuedSamples_, std::memory_order_relaxed);
        totalPops_++;

        if (queuedSamples_ < windowMinSamples_) {
            windowMinSamples_ = queuedSamples_;
        }

        underruns_ = 0;
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        queuedSamples_ = 0;
        queuedSamplesAtomic_.store(0, std::memory_order_relaxed);
        // Do NOT reset targetSampleFrames_ here; the server controls it explicitly
        underruns_ = 0;
        earlyCount_ = 0;
        primed_ = false;
        resetWindowLocked();
        lastPushSeq_ = 0;
        totalPushes_ = 0;
        totalPops_ = 0;
        totalUnderruns_ = 0;
        totalMissedPackets_ = 0;
        totalDuplicates_ = 0;
        totalReordered_ = 0;
        totalDroppedOld_ = 0;
        totalOverflows_ = 0;
        totalTrimmed_ = 0;
    }

    int size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(frames_.size());
    }

    // Lock-free sample frame depth for UI latency polling and audio callback diagnostics
    int queuedSampleFrames() const {
        return queuedSamplesAtomic_.load(std::memory_order_relaxed);
    }

    void setTargetMs(int jitterMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jitterMs < 0) jitterMs = 0;
        int samples = (jitterMs * SAMPLE_RATE) / 1000;
        if (samples > MAX_TARGET_SAMPLE_FRAMES) samples = MAX_TARGET_SAMPLE_FRAMES;
        targetSampleFrames_ = samples;
        resetWindowLocked();
        LOGI("[JITTER] Server set explicit target to %d ms (%d sample frames)", jitterMs, samples);
    }

    void logStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        LOGI("[JITTER] Stats: pkts=%zu (%.1fms), target=%.1fms, pushes=%llu, pops=%llu, "
             "underruns=%llu, trimmed=%llu, missed=%llu, dupes=%llu, reorder=%llu, "
             "dropped_old=%llu, overflows=%llu",
             frames_.size(), queuedSamples_ * 1000.0 / SAMPLE_RATE,
             targetSampleFrames_ * 1000.0 / SAMPLE_RATE,
             (unsigned long long)totalPushes_, (unsigned long long)totalPops_,
             (unsigned long long)totalUnderruns_, (unsigned long long)totalTrimmed_,
             (unsigned long long)totalMissedPackets_,
             (unsigned long long)totalDuplicates_, (unsigned long long)totalReordered_,
             (unsigned long long)totalDroppedOld_, (unsigned long long)totalOverflows_);
    }

private:
    void resetWindowLocked() {
        windowMinSamples_ = std::numeric_limits<int>::max();
        windowPushedSamples_ = 0;
    }

    std::deque<AudioFrame> frames_;
    mutable std::mutex mutex_;
    int queuedSamples_ = 0;
    std::atomic<int> queuedSamplesAtomic_{0};
    int targetSampleFrames_;
    int underruns_;
    int earlyCount_;
    bool primed_ = false;
    int windowMinSamples_ = std::numeric_limits<int>::max();
    int windowPushedSamples_ = 0;
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
    uint64_t totalTrimmed_ = 0;
};

class AudioPlayer : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioPlayer() : decoder_(nullptr), running_(false), muted_(false), volume_(1.0f), systemVolume_(1.0f) {}

    bool isRunning() const { return running_.load(); }
    bool isMuted() const { return muted_.load(); }
    float volume() const { return volume_.load(); }
    float systemVolume() const { return systemVolume_.load(); }

    void setMuted(bool muted) { muted_.store(muted); }
    void setVolume(float vol) { volume_.store(vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol)); }
    void setSystemVolume(float vol) { systemVolume_.store(vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol)); }

    void setJitterBufferMs(int jitterMs) {
        jitterBuffer_.setTargetMs(jitterMs);
    }

    bool start() {
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        if (running_) return true;
        int error;
        decoder_ = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
        if (error != OPUS_OK || !decoder_) {
            LOGE("Failed to create Opus decoder: %s", opus_strerror(error));
            return false;
        }

        jitterBuffer_.reset();
        playbackFrame_.reset();
        remainingPlaybackFrames_.store(0, std::memory_order_relaxed);
        lastSampleL_ = 0;
        lastSampleR_ = 0;
        framesSinceLog_ = 0;
        callbackCount_ = 0;

        // Request SharingMode::Exclusive for direct 1.0ms (48-frame) hardware MMAP access,
        // bypassing AAudioService's 6.0ms shared mixer ring buffer! Oboe automatically falls
        // back to SharingMode::Shared if Exclusive MMAP is unavailable; when Exclusive is
        // granted, we apply Android's STREAM_MUSIC volume curve in software so hardware
        // volume keys work identically.
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Exclusive)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setDataCallback(this)
               ->setErrorCallback(this)
               ->setUsage(oboe::Usage::Media)
               ->setContentType(oboe::ContentType::Music);

        std::shared_ptr<oboe::AudioStream> newStream;
        oboe::Result result = builder.openStream(newStream);
        if (result != oboe::Result::OK || !newStream) {
            LOGE("Failed to open stream: %s", oboe::convertToText(result));
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
            return false;
        }

        const bool exclusiveGranted = (newStream->getSharingMode() == oboe::SharingMode::Exclusive);
        isExclusiveMmap_.store(exclusiveGranted, std::memory_order_relaxed);

        // Pin active AAudio ring buffer threshold:
        // - Exclusive MMAP (48-frame 1.0ms burst): 3x burst = 144 frames (3.0ms) gives 2.0ms of
        //   SCHED_FIFO scheduling slack for zero XRuns while halving output latency vs Shared!
        // - Shared MMAP (96-frame 2.0ms burst): 2x burst = 192 frames (4.0ms).
        int32_t burst = newStream->getFramesPerBurst();
        if (burst > 0) {
            int32_t targetBuf = (burst <= 48)
                ? std::max<int32_t>(burst * 3, 144)
                : std::max<int32_t>(burst * 2, 144);
            int32_t cap = newStream->getBufferCapacityInFrames();
            if (cap > 0 && targetBuf > cap) targetBuf = cap;
            newStream->setBufferSizeInFrames(targetBuf);
        }

        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            stream_ = newStream;
        }

        // Trim any packets accumulated while AAudioStreamBuilder_openStream() was opening
        jitterBuffer_.trimPreStartBacklog();

        running_ = true;
        result = newStream->requestStart();
        if (result != oboe::Result::OK) {
            running_ = false;
            LOGE("Failed to start stream: %s", oboe::convertToText(result));
            {
                std::lock_guard<std::mutex> lock(streamMutex_);
                stream_.reset();
            }
            newStream->close();
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
            return false;
        }

        LOGI("Audio player started: api=%s, perf=%d, sharing=%s, rate=%d, burst=%d, bufSize=%d, cap=%d",
             newStream->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSLES",
             (int)newStream->getPerformanceMode(),
             exclusiveGranted ? "EXCLUSIVE" : "SHARED",
             newStream->getSampleRate(),
             newStream->getFramesPerBurst(),
             newStream->getBufferSizeInFrames(),
             newStream->getBufferCapacityInFrames());
        return true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        running_ = false;
        isExclusiveMmap_.store(false, std::memory_order_relaxed);
        std::shared_ptr<oboe::AudioStream> oldStream;
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            oldStream = std::move(stream_);
        }
        if (oldStream) {
            oldStream->requestStop();
            oldStream->close();
            oldStream.reset();
        }
        if (decoder_) {
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
        }
        jitterBuffer_.reset();
        remainingPlaybackFrames_.store(0, std::memory_order_relaxed);
    }

    void feedPacket(const uint8_t* data, int length) {
        // Serialize decoder lifetime with start/stop without blocking the receive
        // worker during device teardown. The real-time output callback never takes this lock.
        std::unique_lock<std::recursive_mutex> lifecycle(lifecycleMutex_, std::try_to_lock);
        if (!lifecycle.owns_lock()) return;
        if (!running_ || length < HEADER_SIZE) return;

        uint8_t version = data[0];
        uint8_t type = data[1];

        if (version != 0x01 || (type != PACKET_AUDIO && type != PACKET_AUDIO_PCM)) return;

        uint32_t seq;
        uint64_t timestamp;
        uint16_t payloadLen;

        memcpy(&seq, data + 2, 4);
        memcpy(&timestamp, data + 6, 8);
        memcpy(&payloadLen, data + 14, 2);

        if (payloadLen == 0 || length < HEADER_SIZE + payloadLen) return;

        const uint8_t* payload = data + HEADER_SIZE;

        // Track feed timing
        auto now = std::chrono::steady_clock::now();
        double feedGapMs = std::chrono::duration<double, std::milli>(now - lastFeedTime_).count();
        lastFeedTime_ = now;
        feedCount_++;

        const int cfgSamples = std::max(48, g_frameSamples.load(std::memory_order_relaxed));
        const double expectedFeedMs = (cfgSamples * 1000.0) / SAMPLE_RATE;
        if (feedGapMs > std::max(15.0, expectedFeedMs * 2.5) && feedCount_ > 1) {
            largeFeedGapCount_++;
            if (largeFeedGapCount_ <= 20 || (largeFeedGapCount_ % 50) == 0) {
                LOGI("[FEED] LARGE GAP #%llu: %.1fms between feeds (seq=%u, jbufMs=%.1f)",
                     (unsigned long long)largeFeedGapCount_, feedGapMs, seq,
                     jitterBuffer_.queuedSampleFrames() * 1000.0 / SAMPLE_RATE);
            }
        }

        AudioFrame frame;
        frame.sequence = seq;
        frame.timestamp = timestamp;
        double decodeMs = 0.0;

        if (type == PACKET_AUDIO_PCM) {
            // Zero-lookahead Raw 16-bit Stereo PCM fast path (4 bytes per stereo frame)
            if ((payloadLen % 4) != 0) return;
            int pcmFrames = payloadLen / 4;
            if (pcmFrames <= 0 || pcmFrames > AudioFrame::maxFrames) return;
            memcpy(frame.samples, payload, static_cast<size_t>(pcmFrames) * 4);
            frame.frameCount = pcmFrames;
        } else {
            // Opus decode path (exclusively on recvThread_ — never touched from onAudioReady)
            if (!decoder_) return;
            auto beforeDecode = std::chrono::steady_clock::now();
            int decoded = opus_decode(decoder_, payload, payloadLen,
                                      frame.samples, AudioFrame::maxFrames, 0);
            auto afterDecode = std::chrono::steady_clock::now();
            decodeMs = std::chrono::duration<double, std::milli>(afterDecode - beforeDecode).count();

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
        }

        jitterBuffer_.push(frame);

        // Periodic feed stats (every ~5 seconds)
        const uint64_t logInterval = static_cast<uint64_t>(std::max(100.0, 5000.0 / expectedFeedMs));
        if (feedCount_ <= 5 || (logInterval > 0 && (feedCount_ % logInterval) == 0)) {
            LOGI("[FEED] #%llu (%s): seq=%u, len=%u, frames=%d, "
                 "decodeMs=%.2f, gap=%.1fms, jbufMs=%.1f, "
                 "largeGaps=%llu, slowDec=%llu, decErr=%llu",
                 (unsigned long long)feedCount_,
                 type == PACKET_AUDIO_PCM ? "PCM" : "OPUS",
                 seq, payloadLen, frame.frameCount,
                 decodeMs, feedGapMs,
                 jitterBuffer_.queuedSampleFrames() * 1000.0 / SAMPLE_RATE,
                 (unsigned long long)largeFeedGapCount_,
                 (unsigned long long)slowDecodes_,
                 (unsigned long long)decodeErrors_);
            jitterBuffer_.logStats();
        }
    }

    double getLatency() const {
        const int totalFrames = jitterBuffer_.queuedSampleFrames() +
                                remainingPlaybackFrames_.load(std::memory_order_relaxed);
        return (totalFrames * 1000.0) / SAMPLE_RATE;
    }

    double getOutputBufferMs() const {
        if (!running_.load(std::memory_order_relaxed)) return 0.0;
        std::shared_ptr<oboe::AudioStream> s;
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            s = stream_;
        }
        if (!s) return 0.0;
        auto latResult = s->calculateLatencyMillis();
        if (latResult) {
            double ms = latResult.value();
            if (ms > 0.0 && ms < 500.0) {
                return ms;
            }
        }
        int32_t rate = s->getSampleRate();
        if (rate <= 0) rate = SAMPLE_RATE;
        return s->getBufferSizeInFrames() * 1000.0 / rate;
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

        auto callbackStart = std::chrono::steady_clock::now();
        double callbackGapMs = std::chrono::duration<double, std::milli>(
            callbackStart - lastCallbackTime_).count();
        lastCallbackTime_ = callbackStart;
        callbackCount_++;

        const double expectedCallbackMs = (numFrames * 1000.0) / SAMPLE_RATE;
        if (callbackGapMs > std::max(10.0, expectedCallbackMs * 2.5) && callbackCount_ > 1) {
            largeCallbackGapCount_++;
            if (largeCallbackGapCount_ <= 20 || (largeCallbackGapCount_ % 50) == 0) {
                LOGI("[OBOE] LARGE CALLBACK GAP #%llu: %.1fms (numFrames=%d, jbufFrames=%d)",
                     (unsigned long long)largeCallbackGapCount_, callbackGapMs, numFrames,
                     jitterBuffer_.queuedSampleFrames());
            }
        }

        auto* output = static_cast<int16_t*>(audioData);

        if (muted_.load(std::memory_order_relaxed)) {
            // Still drain jitter buffer so it doesn't overflow while muted
            int drained = 0;
            while (drained < numFrames) {
                if (playbackFrame_.empty()) {
                    playbackFrame_.reset();
                    if (!jitterBuffer_.pop(playbackFrame_.frame)) break;
                }
                drained += playbackFrame_.copyTo(output + drained * CHANNELS, numFrames - drained);
            }
            remainingPlaybackFrames_.store(playbackFrame_.remaining(), std::memory_order_relaxed);
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
            lastSampleL_ = 0;
            lastSampleR_ = 0;
            return oboe::DataCallbackResult::Continue;
        }

        int framesWritten = 0;
        int underrunsThisCallback = 0;

        while (framesWritten < numFrames) {
            if (playbackFrame_.empty()) {
                playbackFrame_.reset();
                jitterBuffer_.pop(playbackFrame_.frame);
            }
            if (!playbackFrame_.empty()) {
                int copied = playbackFrame_.copyTo(
                    output + framesWritten * CHANNELS, numFrames - framesWritten);

                // Track peak output level and last stereo sample for click-free underrun decay
                for (int i = 0; i < copied * CHANNELS; i++) {
                    int16_t s = output[framesWritten * CHANNELS + i];
                    int16_t abs_s = s < 0 ? -s : s;
                    if (abs_s > outputPeak_) outputPeak_ = abs_s;
                }
                if (copied > 0) {
                    int lastIdx = (framesWritten + copied - 1) * CHANNELS;
                    lastSampleL_ = output[lastIdx];
                    lastSampleR_ = output[lastIdx + 1];
                }
                framesWritten += copied;
            } else {
                // Jitter buffer empty/re-priming: smoothly decay last stereo sample to zero.
                // Avoids calling opus_decode(decoder_, nullptr, ...) concurrently with feedPacket().
                int missingFrames = numFrames - framesWritten;
                underrunsThisCallback++;
                totalPlaybackUnderruns_++;
                totalSilenceFrames_ += missingFrames;

                int16_t* dst = output + framesWritten * CHANNELS;
                for (int i = 0; i < missingFrames; i++) {
                    lastSampleL_ = static_cast<int16_t>((static_cast<int32_t>(lastSampleL_) * 248) / 256);
                    lastSampleR_ = static_cast<int16_t>((static_cast<int32_t>(lastSampleR_) * 248) / 256);
                    dst[i * 2]     = lastSampleL_;
                    dst[i * 2 + 1] = lastSampleR_;
                }
                framesWritten += missingFrames;
            }
        }

        remainingPlaybackFrames_.store(playbackFrame_.remaining(), std::memory_order_relaxed);

        // Log underruns
        if (underrunsThisCallback > 0) {
            if (totalPlaybackUnderruns_ <= 30 || (totalPlaybackUnderruns_ % 50) == 0) {
                LOGI("[OBOE] *** UNDERRUN #%llu in callback #%llu: %d empty, "
                     "jbufFrames=%d, gap=%.1fms",
                     (unsigned long long)totalPlaybackUnderruns_,
                     (unsigned long long)callbackCount_,
                     underrunsThisCallback,
                     jitterBuffer_.queuedSampleFrames(), callbackGapMs);
            }
        }

        // Apply app volume * Android STREAM_MUSIC volume (in Exclusive MMAP mode where HAL bypasses AudioFlinger volume)
        const float appVol = volume_.load(std::memory_order_relaxed);
        const float sysVol = isExclusiveMmap_.load(std::memory_order_relaxed)
            ? systemVolume_.load(std::memory_order_relaxed)
            : 1.0f;
        const float vol = appVol * sysVol;
        if (vol <= 0.0001f) {
            memset(output, 0, numFrames * CHANNELS * sizeof(int16_t));
        } else if (vol < 0.999f) {
            int totalSamples = numFrames * CHANNELS;
            for (int i = 0; i < totalSamples; i++) {
                output[i] = static_cast<int16_t>(output[i] * vol);
            }
        }

        // Periodic Oboe callback stats (every ~3 seconds regardless of callback size)
        framesSinceLog_ += numFrames;
        if (callbackCount_ <= 3 || framesSinceLog_ >= SAMPLE_RATE * 3) {
            framesSinceLog_ = 0;
            auto callbackEnd = std::chrono::steady_clock::now();
            double callbackDurationMs = std::chrono::duration<double, std::milli>(
                callbackEnd - callbackStart).count();
            int32_t xruns = stream ? stream->getXRunCount().value() : 0;
            LOGI("[OBOE] Callback #%llu (%s): numFrames=%d, gap=%.1fms, dur=%.2fms, "
                 "jbufMs=%.1f, rem=%d, peak=%d, xruns=%d, underruns=%llu, fadeFrames=%llu",
                 (unsigned long long)callbackCount_,
                 isExclusiveMmap_.load(std::memory_order_relaxed) ? "EXCL" : "SHRD",
                 numFrames, callbackGapMs, callbackDurationMs,
                 jitterBuffer_.queuedSampleFrames() * 1000.0 / SAMPLE_RATE,
                 playbackFrame_.remaining(), (int)outputPeak_, xruns,
                 (unsigned long long)totalPlaybackUnderruns_,
                 (unsigned long long)totalSilenceFrames_);
            outputPeak_ = 0;
        }

        return oboe::DataCallbackResult::Continue;
    }

    // Oboe error callback — handle device disconnection
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOGI("Audio device disconnected, restarting stream...");
            // Restart on a new thread — callbacks must not block
            std::thread([this, stream]() {
                std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
                {
                    std::lock_guard<std::mutex> lock(streamMutex_);
                    if (!running_ || stream_.get() != stream) return;
                }
                stop();
                start();
            }).detach();
        } else {
            LOGE("Audio stream error: %s", oboe::convertToText(error));
        }
    }

private:
    OpusDecoder* decoder_;
    std::recursive_mutex lifecycleMutex_;
    mutable std::mutex streamMutex_;
    std::shared_ptr<oboe::AudioStream> stream_;
    JitterBuffer jitterBuffer_;
    PlaybackFrame playbackFrame_;
    std::atomic<int> remainingPlaybackFrames_{0};
    std::atomic<bool> running_;
    std::atomic<bool> muted_;
    std::atomic<bool> isExclusiveMmap_{false};
    std::atomic<float> volume_;
    std::atomic<float> systemVolume_;
    int16_t lastSampleL_ = 0;
    int16_t lastSampleR_ = 0;

    // Feed path diagnostics
    std::chrono::steady_clock::time_point lastFeedTime_ = std::chrono::steady_clock::now();
    uint64_t feedCount_ = 0;
    uint64_t largeFeedGapCount_ = 0;
    uint64_t slowDecodes_ = 0;
    uint64_t decodeErrors_ = 0;

    // Oboe callback diagnostics
    std::chrono::steady_clock::time_point lastCallbackTime_ = std::chrono::steady_clock::now();
    uint64_t callbackCount_ = 0;
    int32_t framesSinceLog_ = 0;
    uint64_t largeCallbackGapCount_ = 0;
    uint64_t totalPlaybackUnderruns_ = 0;
    uint64_t totalSilenceFrames_ = 0;
    int16_t outputPeak_ = 0;
};

// --- AudioRecorder (Microphone to PC) ---
// Uses mono (1 channel) since phone mics are typically mono.
// The server-side Opus decoder must also be configured for mono.
static constexpr int MIC_CHANNELS = 1;

// Enqueue into the authenticated connection; never perform socket I/O on Oboe.
static void sendEncryptedMic(const uint8_t* data, size_t len);

class AudioRecorder : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioRecorder() : encoder_(nullptr), running_(false), sequence_(0) {}

    bool isRunning() const { return running_.load(); }

    bool start(const char* serverIp, int serverPort) {
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        if (running_) return true;
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

        streamStart_ = std::chrono::steady_clock::now();
        sequence_ = 0;
        accumFrames_ = 0;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setFormat(oboe::AudioFormat::I16)
               ->setChannelCount(MIC_CHANNELS)
               ->setSampleRate(SAMPLE_RATE)
               ->setInputPreset(oboe::InputPreset::Unprocessed)
               ->setDataCallback(this)
               ->setErrorCallback(this);

        oboe::Result result = builder.openStream(stream_);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open recording stream: %s", oboe::convertToText(result));
            cleanup();
            return false;
        }

        int32_t burst = stream_->getFramesPerBurst();
        if (burst > 0) {
            int32_t targetBuf = std::max<int32_t>(burst * 2, 96);
            int32_t cap = stream_->getBufferCapacityInFrames();
            if (cap > 0 && targetBuf > cap) targetBuf = cap;
            stream_->setBufferSizeInFrames(targetBuf);
        }

        // Verify actual channel count matches what we requested
        actualChannels_ = stream_->getChannelCount();
        if (actualChannels_ != MIC_CHANNELS) {
            LOGI("Requested %d channels, got %d — will adapt", MIC_CHANNELS, actualChannels_);
        }

        running_ = true;
        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
            running_ = false;
            LOGE("Failed to start recording stream: %s", oboe::convertToText(result));
            stream_->close();
            stream_.reset();
            cleanup();
            return false;
        }

        LOGI("Audio recording started to %s:%d (channels=%d, burst=%d, bufSize=%d)",
             serverIp, serverPort, actualChannels_,
             stream_->getFramesPerBurst(), stream_->getBufferSizeInFrames());
        return true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        running_ = false;
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        cleanup();
        accumFrames_ = 0;
        LOGI("Audio recording stopped");
    }

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream,
            void* audioData,
            int32_t numFrames) override {

        if (!running_ || !encoder_) {
            return oboe::DataCallbackResult::Stop;
        }

        auto* input = static_cast<int16_t*>(audioData);
        int fms = g_frameSizeMs.load(std::memory_order_relaxed);
        if (fms != 5 && fms != 10 && fms != 20) fms = 10;
        const int encodeFrames = fms * (SAMPLE_RATE / 1000); // 240 (5ms), 480 (10ms), or 960 (20ms)

        int framesRead = 0;
        static constexpr int ACCUM_CAPACITY = AudioFrame::maxFrames * 2;

        while (framesRead < numFrames) {
            int space = ACCUM_CAPACITY - accumFrames_;
            if (space <= 0) {
                accumFrames_ = 0;
                space = ACCUM_CAPACITY;
            }
            int toCopy = std::min<int>(numFrames - framesRead, space);

            if (actualChannels_ > MIC_CHANNELS) {
                for (int i = 0; i < toCopy; i++) {
                    int32_t sum = 0;
                    for (int ch = 0; ch < actualChannels_; ch++) {
                        sum += input[(framesRead + i) * actualChannels_ + ch];
                    }
                    accumBuf_[accumFrames_ + i] = static_cast<int16_t>(sum / actualChannels_);
                }
            } else {
                memcpy(accumBuf_ + accumFrames_, input + framesRead, toCopy * sizeof(int16_t));
            }

            accumFrames_ += toCopy;
            framesRead += toCopy;

            while (accumFrames_ >= encodeFrames) {
                uint8_t packet[1500];
                int opusLen = opus_encode(encoder_, accumBuf_, encodeFrames,
                                          packet + HEADER_SIZE, 1200);

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

                    sendEncryptedMic(packet, HEADER_SIZE + opusLen);
                }

                int leftover = accumFrames_ - encodeFrames;
                if (leftover > 0) {
                    memmove(accumBuf_, accumBuf_ + encodeFrames, leftover * sizeof(int16_t));
                }
                accumFrames_ = leftover;
            }
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOGI("Recording device disconnected, restarting stream...");
            std::thread([this, stream]() {
                std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
                if (!running_ || stream_.get() != stream) return;
                stop();
                if (!serverIp_.empty()) {
                    start(serverIp_.c_str(), serverPort_);
                }
            }).detach();
        } else {
            LOGE("Recording stream error: %s", oboe::convertToText(error));
            std::thread([this, stream]() {
                std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
                if (stream_.get() == stream) stop();
            }).detach();
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
    std::recursive_mutex lifecycleMutex_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::atomic<bool> running_;
    std::chrono::steady_clock::time_point streamStart_;
    std::atomic<uint32_t> sequence_;
    int actualChannels_ = MIC_CHANNELS;
    int16_t accumBuf_[AudioFrame::maxFrames * 2]{};
    int accumFrames_ = 0;
    std::string serverIp_;
    int serverPort_ = 0;
};

// --- ConnectionManager (DTLS + Connection Protocol) ---

#include "dtls_session.h"
#include "secure_pairing.h"
#include <deque>
#include <condition_variable>
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

    // Blocking connect runs on the JNI worker, with bounded network deadlines.
    std::string connect(const char* serverIp, int serverPort, const char* deviceName,
                        const char* deviceId, const char* pskHex) {
        cancelled_ = true;
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        disconnect();
        cancelled_ = false;
        const std::string name(deviceName), id(deviceId);
        if (!SecurePairing::validField(name, 128) || !SecurePairing::validField(id, 128))
            return "error|Invalid device identity";
        SecurePairing exchange;
        if (!exchange.generate()) return "error|Key generation failed";
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) return "error|socket_failed";
        auto fail = [&](const std::string& reason) {
            dtlsSession_.reset();
            dtlsActive_ = false;
            close(socket_);
            socket_ = -1;
            return reason;
        };
        serverAddr_ = {};
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) != 1) return fail("error|invalid_ip");
        int sockBufBytes = 262144, tos = 0xB8;
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &sockBufBytes, sizeof(sockBufBytes));
        setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &sockBufBytes, sizeof(sockBufBytes));
        setsockopt(socket_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        timeval tv{0, 100000};
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        auto sendText = [&](const std::string& msg) {
            sendto(socket_, msg.data(), msg.size(), 0, (sockaddr*)&serverAddr_, sizeof(serverAddr_));
        };
        auto receive = [&](uint8_t* data, size_t size) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(socket_, data, size, 0, (sockaddr*)&from, &fromLen);
            return n > 0 && from.sin_addr.s_addr == serverAddr_.sin_addr.s_addr &&
                from.sin_port == serverAddr_.sin_port ? n : 0;
        };
        std::string key = SecurePairing::validStoredKey(pskHex) ? pskHex : "";
        const std::string hello = "AB_CONNECT2|" + name + "|" + id + "|" +
            SecurePairing::commitment(name, id, exchange.publicKey()) + "|" + (key.empty() ? "pair" : "resume");
        sendText(hello);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        std::string serverPublic, newKey;
        bool accepted = false;
        uint8_t packet[4096];
        while (!cancelled_ && std::chrono::steady_clock::now() < deadline) {
            int n = receive(packet, sizeof(packet));
            if (n > 0) {
                const std::string msg(reinterpret_cast<char*>(packet), n);
                if (msg.rfind("AB_REJECT|", 0) == 0) return fail("rejected|" + msg.substr(10));
                if (msg.rfind("AB_KEY2|", 0) == 0) {
                    const auto pub = msg.substr(8);
                    // Never allow a challenge to change after the public key reveal.
                    if (!serverPublic.empty() && serverPublic != pub) return fail("error|Pairing key changed");
                    if (serverPublic.empty()) {
                        std::string code;
                        if (!exchange.derive(pub, name, id, false, newKey, code)) return fail("error|Invalid pairing key");
                        serverPublic = pub;
                        key = newKey;
                        notifyDart("onPairPending", code.c_str());
                        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
                    }
                    sendText("AB_KEY2|" + exchange.publicKey());
                } else if (msg == "AB_ACCEPT2") {
                    if (!SecurePairing::validStoredKey(key)) return fail("error|Pairing required");
                    accepted = true;
                    break;
                }
            }
            if (serverPublic.empty() && std::chrono::steady_clock::now() >= retry) {
                sendText(hello);
                retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
        }
        if (!accepted) return fail("timeout");
        dtlsSession_ = std::make_unique<DtlsSession>();
        if (!dtlsSession_->initClient(socket_, serverAddr_, id, DtlsSession::hexToBytes(key.substr(3))))
            return fail("dtls_failed");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool ready = false;
        retry = std::chrono::steady_clock::now();
        while (!cancelled_ && std::chrono::steady_clock::now() < deadline) {
            if (!dtlsSession_->isEstablished()) {
                int ret = dtlsSession_->continueHandshake();
                if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                    return fail("dtls_failed");
            } else {
                if (std::chrono::steady_clock::now() >= retry) {
                    if (dtlsSession_->send(reinterpret_cast<const uint8_t*>("AB_READY2"), 9) != 9) return fail("dtls_failed");
                    retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                }
                uint8_t plain[4096];
                int n = dtlsSession_->recv(plain, sizeof(plain));
                if (n == 9 && memcmp(plain, "AB_READY2", 9) == 0) { ready = true; break; }
                if (n < 0) return fail("dtls_failed");
            }
            int n = receive(packet, sizeof(packet));
            if (n > 0 && packet[0] >= 20 && packet[0] <= 25) dtlsSession_->pushReceivedData(packet, n);
        }
        if (!ready || cancelled_) return fail("dtls_failed");
        dtlsActive_ = true;
        streamStart_ = std::chrono::steady_clock::now();
        lastServerPacket_ = streamStart_;
        sequence_ = 0;
        connected_ = true;
        micThread_ = std::thread(&ConnectionManager::micSendLoop, this);
        recvThread_ = std::thread(&ConnectionManager::recvLoop, this);
        return newKey.empty() ? "accepted" : "accepted|" + newKey;
    }

    void enqueueMic(const uint8_t* data, size_t len) {
        if (!connected_ || len > 1500) return;
        // The real-time callback must never wait for encryption or a network send.
        std::unique_lock<std::mutex> lock(micMutex_, std::try_to_lock);
        if (!lock || !connected_ || micCount_ == 8) return;
        auto& packet = micPackets_[(micHead_ + micCount_) % 8];
        memcpy(packet.data, data, len);
        packet.len = len;
        ++micCount_;
        micCv_.notify_one();
    }

    void disconnect() {
        // Cancel an in-flight handshake before waiting for its owner. Receive
        // deadlines are 100 ms, so teardown never frees a live TLS context.
        cancelled_ = true;
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        const bool wasConnected = connected_.exchange(false);
        const bool hasResources = wasConnected || recvThread_.joinable() || (socket_ >= 0) || (dtlsSession_ != nullptr);
        if (!hasResources) return;

        LOGI("[CONN] Disconnecting (wasConnected=%d)...", wasConnected ? 1 : 0);

        // Send disconnect control if session was still active
        if (wasConnected && socket_ >= 0) {
            uint8_t packet[17];
            writeHeader(packet, PACKET_CONTROL, 1);
            packet[16] = CTRL_DISCONNECT;
            sendPacket(packet, 17);
        }

        micCv_.notify_all();
        if (micThread_.joinable()) micThread_.join();
        {
            std::lock_guard<std::mutex> lock(micMutex_);
            micCount_ = 0;
            micHead_ = 0;
        }

        if (recvThread_.joinable()) {
            if (recvThread_.get_id() == std::this_thread::get_id()) {
                recvThread_.detach();
            } else {
                recvThread_.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(dtlsMutex_);
            dtlsActive_ = false;
            dtlsSession_.reset();
        }

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
        std::lock_guard<std::mutex> lock(dtlsMutex_);
        if (dtlsActive_ && dtlsSession_) dtlsSession_->send(data, len);
    }

    void micSendLoop() {
        while (connected_) {
            MicPacket packet;
            {
                std::unique_lock<std::mutex> lock(micMutex_);
                micCv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return !connected_ || micCount_ > 0; });
                if (!connected_) break;
                if (micCount_ == 0) continue;
                packet = micPackets_[micHead_];
                micHead_ = (micHead_ + 1) % 8;
                --micCount_;
            }
            sendPacket(packet.data, packet.len);
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
        char buf[4096];
        struct sockaddr_in fromAddr;
        socklen_t fromLen;

        while (connected_) {
            // Unauthenticated traffic must not keep a dead session alive.
            if (std::chrono::steady_clock::now() - lastServerPacket_ > std::chrono::seconds(5)) {
                connected_ = false;
                notifyDart("onDisconnected", "timeout");
                break;
            }
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

            if (fromAddr.sin_addr.s_addr != serverAddr_.sin_addr.s_addr ||
                fromAddr.sin_port != serverAddr_.sin_port) continue;
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
                    lastServerPacket_ = std::chrono::steady_clock::now();
                    processPacket(plainBuf, n);
                } else if (n < 0 && n != MBEDTLS_ERR_SSL_WANT_READ) {
                    LOGE("[RECV] DTLS recv error: -0x%04x", -n);
                    connected_ = false;
                    notifyDart("onDisconnected", "dtls_error");
                    break;
                }
            }
        }
        LOGI("[RECV] Thread exiting");
    }

    void processPacket(const uint8_t* data, int len) {
        if (len < 16 || data[0] != 1) return;
        uint16_t declaredLen;
        memcpy(&declaredLen, data + 14, 2);
        if (len != 16 + declaredLen) return;

        uint8_t type = data[1];

        switch (type) {
            case PACKET_AUDIO:
            case PACKET_AUDIO_PCM:
                // Feed Opus or zero-lookahead Raw PCM packet to player
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
                    uint16_t jitterMs = 10;
                    memcpy(&jitterMs, data + 16, 2);
                    uint16_t frameMs = 5;
                    if (len >= 20) {
                        memcpy(&frameMs, data + 18, 2);
                        if (frameMs == 2 || frameMs == 5 || frameMs == 10 || frameMs == 20) {
                            g_frameSizeMs.store((int)frameMs);
                            notifyDart("onFrameSizeUpdate", std::to_string(frameMs).c_str());
                        }
                    }
                    uint16_t frameSamples = (frameMs == 2) ? 120 : (uint16_t)(frameMs * 48);
                    bool isPcm = false;
                    if (len >= 24) {
                        uint16_t wireSamples = 0;
                        memcpy(&wireSamples, data + 20, 2);
                        if (wireSamples >= 48 && wireSamples <= 960) {
                            frameSamples = wireSamples;
                        }
                        isPcm = (data[22] != 0);
                    }
                    g_frameSamples.store((int)frameSamples);
                    g_isRawPcm.store(isPcm);
                    LOGI("[RECV] Settings: jitter=%u ms, frame=%u (%u samples = %.2f ms), codec=%s",
                         jitterMs, frameMs, frameSamples, frameSamples * 1000.0 / SAMPLE_RATE,
                         isPcm ? "PCM" : "Opus");

                    g_player.setJitterBufferMs(jitterMs);
                    notifyDart("onSettingsUpdate", std::to_string(jitterMs).c_str());

                    char cfgJson[128];
                    snprintf(cfgJson, sizeof(cfgJson),
                             "{\"jitter\":%u,\"samples\":%u,\"pcm\":%s}",
                             (unsigned)jitterMs, (unsigned)frameSamples, isPcm ? "true" : "false");
                    notifyDart("onStreamConfig", cfgJson);
                }
                break;
            }
        }
    }

    std::recursive_mutex lifecycleMutex_;
    std::atomic<bool> cancelled_{false};
    struct MicPacket { uint8_t data[1500]; size_t len = 0; };
    MicPacket micPackets_[8];
    size_t micHead_ = 0, micCount_ = 0;
    std::mutex micMutex_;
    std::condition_variable micCv_;
    std::thread micThread_;
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

static void sendEncryptedMic(const uint8_t* data, size_t len) {
    g_connMgr.enqueueMic(data, len);
}

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

JNIEXPORT void JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeSetSystemVolume(
        JNIEnv* env, jobject thiz, jfloat volume) {
    g_player.setSystemVolume(volume);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeGetAudioState(JNIEnv* env, jobject) {
    jdouble values[] = {g_player.isRunning() ? 1.0 : 0.0,
                       g_recorder.isRunning() ? 1.0 : 0.0,
                       g_player.isMuted() ? 1.0 : 0.0,
                       g_player.volume(), g_player.systemVolume()};
    jdoubleArray result = env->NewDoubleArray(5);
    if (result) env->SetDoubleArrayRegion(result, 0, 5, values);
    return result;
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
    return 0.0f;
}

// --- ConnectionManager JNI ---

JNIEXPORT jstring JNICALL
Java_com_audiobridge_audiobridge_MainActivity_nativeConnect(
        JNIEnv* env, jobject thiz, jstring serverIp, jint serverPort,
        jstring deviceName, jstring deviceId, jstring pskHex) {
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
