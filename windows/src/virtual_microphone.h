#pragma once
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "../../common/audiobridge_mic.h"
#include "../../common/mic_ring.h"

class VirtualMicrophone {
public:
    enum class Status { Starting, Ready, Unavailable };
    VirtualMicrophone() = default;
    ~VirtualMicrophone() { stop(); }
    VirtualMicrophone(const VirtualMicrophone&) = delete;
    VirtualMicrophone& operator=(const VirtualMicrophone&) = delete;
    void start();
    void stop();
    void pushAudio(const float* data, unsigned frames);
    Status status() const { return status_.load(); }
    DWORD error() const { return error_.load(); }
private:
    void run();
    HANDLE stop_ = nullptr;
    HANDLE wake_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    MicRing<AB_MIC_CAPACITY> queue_;
    std::atomic<Status> status_{Status::Unavailable};
    std::atomic<DWORD> error_{ERROR_SERVICE_NOT_ACTIVE};
};
