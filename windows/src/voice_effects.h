#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VoiceSettings {
    bool noise = true;
    int strength = 100;
    bool gate = true;
    int threshold = 50;
    int holdMs = 300;
    bool lowCut = false;
    bool operator==(const VoiceSettings& b) const {
        return noise == b.noise && strength == b.strength && gate == b.gate &&
            threshold == b.threshold && holdMs == b.holdMs && lowCut == b.lowCut;
    }
    static VoiceSettings parse(const std::string& text);
    std::string serialize() const;
};

// Stateful 48 kHz mono processing. No Windows audio endpoint or phone required.
class VoiceDsp {
public:
    explicit VoiceDsp(const std::wstring& modelDir);
    ~VoiceDsp();
    void reset();
    std::vector<float> process(const float* samples, size_t count, VoiceSettings settings);
    std::vector<float> render(const std::vector<float>& clip, VoiceSettings settings);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Inference stays off the encrypted network callback. Queue bounded to 100 ms;
// stale work is discarded and model history reset instead of accumulating latency.
class VoiceEffects {
public:
    using Sink = std::function<void(const float*, unsigned)>;
    VoiceEffects(const std::wstring& modelDir, VoiceSettings settings, Sink sink);
    ~VoiceEffects();
    void push(const float* samples, unsigned count);
    void reset();
    void setSettings(VoiceSettings settings);
    VoiceSettings settings() const;
    void record(bool enabled);
    std::string stateJson() const;
    std::string clipJson(bool processed) const;
    void stop();
private:
    void run();
    void previewRun();
    void requestPreview();
    void deliver(const std::vector<float>& samples);
    std::wstring modelDir_;
    Sink sink_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, previewWake_;
    VoiceSettings settings_;
    std::deque<float> queue_;
    std::vector<float> original_, processed_;
    bool running_ = true, reset_ = false, recording_ = false, renderPending_ = false;
    bool ready_ = false, rendering_ = false;
    unsigned generation_ = 0, clipRevision_ = 0, dropped_ = 0;
    unsigned streamGeneration_ = 0;
    std::string error_, previewError_;
    std::thread worker_, previewWorker_;
};
