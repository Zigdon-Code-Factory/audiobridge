#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

class AudioRender {
public:
    using DeviceInvalidatedCallback = std::function<void()>;

    AudioRender();
    ~AudioRender();

    static std::vector<std::pair<std::wstring, std::wstring>> getDevices();

    bool initialize(const std::wstring& deviceId = L"");
    void cleanup();
    bool start();
    void stop();

    void pushAudio(const float* data, uint32_t frameCount);

    void setOnDeviceInvalidated(DeviceInvalidatedCallback cb) { onDeviceInvalidated_ = cb; }

private:
    void renderThread();

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex bufferMutex_;
    std::vector<float> audioBuffer_;

    std::string pulseDeviceName_;
    DeviceInvalidatedCallback onDeviceInvalidated_;
};
