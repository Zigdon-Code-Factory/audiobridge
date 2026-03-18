#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class AudioCapture {
public:
    using FrameCallback = std::function<void(const float* data, uint32_t frameCount)>;

    AudioCapture();
    ~AudioCapture();

    static std::vector<AudioDeviceInfo> getDevices();

    bool initialize(const std::wstring& deviceId = L"");
    void cleanup();
    bool start(FrameCallback callback);
    void stop();

    void setOnDeviceInvalidated(std::function<void()> cb) { onDeviceInvalidated_ = cb; }

    uint32_t getSampleRate() const { return sampleRate_; }
    uint32_t getChannels() const { return channels_; }

private:
    void captureThread();

    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;

    FrameCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::function<void()> onDeviceInvalidated_;

    std::string pulseDeviceName_; // PulseAudio source name
};
