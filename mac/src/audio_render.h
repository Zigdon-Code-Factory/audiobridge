#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

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
    std::atomic<bool> running_{false};

    std::mutex bufferMutex_;
    std::vector<float> audioBuffer_;

    uint32_t deviceId_ = 0;
    void* audioUnit_ = nullptr;
    DeviceInvalidatedCallback onDeviceInvalidated_;

    static int renderCallback(void* inRefCon, unsigned int inActionFlags,
                              const void* inTimeStamp, unsigned int inBusNumber,
                              unsigned int inNumberFrames, void* ioData);

    friend OSStatus caRenderCallback(void*, AudioUnitRenderActionFlags*,
                                      const AudioTimeStamp*, UInt32, UInt32,
                                      AudioBufferList*);
};
