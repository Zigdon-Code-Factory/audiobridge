#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VoiceGpuDevice {
    int id = -1;
    std::string name;
    uint64_t vramMb = 0;
};

struct VoiceSettings {
    static constexpr int ModelDeepFilterNet3 = 0;
    static constexpr int ModelFastEnhancerL = 1;
    static constexpr int ModelDpdfNet8 = 2;
    static constexpr int ModelInworldGpu = 3;
    static constexpr int ModelCount = 4;

    bool noise = true;
    int strength = 100;
    bool gate = true;
    int threshold = 50;
    int holdMs = 300;
    bool lowCut = false;
    int makeupDb = 0;
    int model = ModelDeepFilterNet3;
    int gpuDevice = -1; // -1 = Auto (highest-VRAM hardware GPU), >= 0 = DXGI adapter index
    bool operator==(const VoiceSettings& b) const {
        return noise == b.noise && strength == b.strength && gate == b.gate &&
            threshold == b.threshold && holdMs == b.holdMs && lowCut == b.lowCut &&
            makeupDb == b.makeupDb && model == b.model && gpuDevice == b.gpuDevice;
    }
    static VoiceSettings parse(const std::string& text);
    std::string serialize() const;
};

// Stateful 48 kHz mono processing. No Windows audio endpoint or phone required.
class VoiceDsp {
public:
    explicit VoiceDsp(const std::wstring& modelDir, int initialModel = VoiceSettings::ModelDeepFilterNet3, int initialGpu = -1);
    ~VoiceDsp();
    static std::vector<VoiceGpuDevice> enumerateGpuDevices();
    void reset();
    void releaseGpu();
    int activeGpuDevice() const;
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
    bool ready_ = false, recoverable_ = false, rendering_ = false;
    unsigned generation_ = 0, clipRevision_ = 0, dropped_ = 0;
    unsigned streamGeneration_ = 0;
    int activeGpu_ = -1;
    std::vector<VoiceGpuDevice> gpuDevices_;
    std::chrono::steady_clock::time_point oldestPushAt_{};
    bool hasQueuedPush_ = false;
    std::chrono::steady_clock::time_point lastProcessedAt_{};
    bool hasProcessed_ = false;
    std::chrono::steady_clock::time_point lastOverflowAt_{};
    bool hasOverflow_ = false;
    double modelDelayMs_ = 0.0, processMs_ = 0.0, pipelineMs_ = 0.0, rtf_ = 0.0;
    std::string error_, previewError_;
    std::thread worker_, previewWorker_;
};
