#include "virtual_microphone.h"
#include "mic_pipe.h"
#include <algorithm>
#include <cmath>

namespace {
bool trustedServer(HANDLE pipe) {
    // Do not disclose microphone samples to a process squatting on the pipe name.
    ULONG pipePid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pipePid)) return false;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, AUDIOBRIDGE_MIC_SERVICE, SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS state{};
    DWORD bytes = 0;
    bool trusted = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&state), sizeof(state), &bytes) &&
        state.dwCurrentState == SERVICE_RUNNING && state.dwProcessId == pipePid;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return trusted;
}
}

void VirtualMicrophone::start() {
    if (worker_.joinable()) return;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_ || !wake_) { stop(); return; }
    status_ = Status::Starting;
    worker_ = std::thread(&VirtualMicrophone::run, this);
}

void VirtualMicrophone::stop() {
    if (stop_) SetEvent(stop_);
    if (worker_.joinable()) worker_.join();
    if (wake_) CloseHandle(wake_);
    if (stop_) CloseHandle(stop_);
    wake_ = stop_ = nullptr;
    status_ = Status::Unavailable;
    std::lock_guard<std::mutex> guard(mutex_);
    queue_.clear();
}

void VirtualMicrophone::pushAudio(const float* data, unsigned frames) {
    if (status_ != Status::Ready || !data || !frames) return;
    // The network callback must never wait for service I/O or an allocating queue.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    short samples[AB_MIC_MAX_FRAMES];
    if (frames > AB_MIC_MAX_FRAMES) { data += frames - AB_MIC_MAX_FRAMES; frames = AB_MIC_MAX_FRAMES; }
    for (unsigned i = 0; i < frames; ++i) {
        const float value = std::isfinite(data[i]) ? std::clamp(data[i], -1.f, 1.f) : 0.f;
        samples[i] = static_cast<short>(value * 32767.f);
    }
    queue_.push(samples, frames);
    SetEvent(wake_);
}

void VirtualMicrophone::run() {
    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateFileW(AUDIOBRIDGE_MIC_PIPE, FILE_READ_DATA | FILE_WRITE_DATA |
            FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
            SECURITY_IDENTIFICATION, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            error_ = GetLastError();
        } else {
            DWORD mode = PIPE_READMODE_MESSAGE;
            MicPacket hello;
            MicReply reply;
            if (trustedServer(pipe) && SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) &&
                mic::transfer(pipe, true, &hello, sizeof(hello), stop_, 2000) &&
                mic::transfer(pipe, false, &reply, sizeof(reply), stop_, 15000) &&
                reply.magic == AB_MIC_MAGIC && reply.version == AB_MIC_VERSION) {
                error_ = reply.error;
                if (reply.error == ERROR_SUCCESS) {
                    { std::lock_guard<std::mutex> guard(mutex_); queue_.clear(); }
                    status_ = Status::Ready;
                    HANDLE waits[] = {stop_, wake_};
                    while (WaitForMultipleObjects(2, waits, FALSE, 500) != WAIT_OBJECT_0) {
                        MicPacket packet;
                        { std::lock_guard<std::mutex> guard(mutex_);
                          packet.frames = (std::min<ULONG>)(queue_.size(), AB_MIC_MAX_FRAMES);
                          for (ULONG i = 0; i < packet.frames; ++i) packet.samples[i] = queue_.pop();
                          if (queue_.size()) SetEvent(wake_);
                        }
                        if (!mic::transfer(pipe, true, &packet, sizeof(packet), stop_, 1000) ||
                            !mic::transfer(pipe, false, &reply, sizeof(reply), stop_, 1000) ||
                            reply.magic != AB_MIC_MAGIC || reply.version != AB_MIC_VERSION || reply.error) {
                            error_ = ERROR_DEVICE_NOT_CONNECTED;
                            break;
                        }
                    }
                }
            } else error_ = ERROR_SERVICE_NOT_ACTIVE;
            CloseHandle(pipe); // Service observes EOF even after an abnormal process exit.
        }
        status_ = Status::Unavailable;
        if (WaitForSingleObject(stop_, 3000) != WAIT_TIMEOUT) break;
    }
}
