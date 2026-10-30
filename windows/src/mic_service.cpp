#include <Windows.h>
#include <swdevice.h>
#include <sddl.h>
#include <cfgmgr32.h>
#include <string>
#include "mic_service_session.h"

namespace {
HANDLE stopEvent = nullptr;
SERVICE_STATUS_HANDLE statusHandle = nullptr;
SERVICE_STATUS serviceStatus{};

void report(DWORD state, DWORD error = ERROR_SUCCESS) {
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = state;
    serviceStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    serviceStatus.dwWin32ExitCode = error;
    serviceStatus.dwWaitHint = state == SERVICE_STOP_PENDING ? 10000 : 0;
    SetServiceStatus(statusHandle, &serviceStatus);
}
DWORD WINAPI control(DWORD code, DWORD, void*, void*) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        SetEvent(stopEvent);
    }
    return NO_ERROR;
}

class Device final : public mic::Backend {
public:
    ~Device() override { close(); }
    void setOwner(HANDLE owner) { owner_ = owner; }
    DWORD open(HANDLE stop) override {
        created_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!created_) return GetLastError();
        SW_DEVICE_CREATE_INFO info{};
        info.cbSize = sizeof(info);
        info.pszInstanceId = L"Microphone"; // Stable identity retains other apps' selection.
        const wchar_t ids[] = AUDIOBRIDGE_MIC_HARDWARE_ID L"\0";
        info.pszzHardwareIds = ids;
        info.pszDeviceDescription = L"AudioBridge Microphone";
        info.CapabilityFlags = SWDeviceCapabilitiesDriverRequired | SWDeviceCapabilitiesSilentInstall;
        HRESULT hr = SwDeviceCreate(L"AudioBridge", L"HTREE\\ROOT\\0", &info,
                                    0, nullptr, onCreated, this, &device_);
        if (FAILED(hr)) return HRESULT_CODE(hr);
        HANDLE waits[] = {stop, created_, owner_};
        if (WaitForMultipleObjects(3, waits, FALSE, 10000) != WAIT_OBJECT_0 + 1) return ERROR_CANCELLED;
        if (FAILED(createResult_)) return HRESULT_CODE(createResult_);
        // Callback completion means the devnode exists, not that its driver has started.
        for (unsigned attempt = 0; attempt < 40; ++attempt) {
            DEVINST node = 0;
            ULONG status = 0, problem = 0;
            const bool started = CM_Locate_DevNodeW(&node, instanceId_.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
                CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS &&
                (status & DN_STARTED) && !(status & DN_HAS_PROBLEM);
            if (started) driver_ = CreateFileW(AUDIOBRIDGE_MIC_DEVICE, GENERIC_WRITE, 0,
                                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (driver_ != INVALID_HANDLE_VALUE) {
                DWORD bytes = 0;
                if (DeviceIoControl(driver_, IOCTL_AB_MIC_RESET, nullptr, 0, nullptr, 0, &bytes, nullptr))
                    return ERROR_SUCCESS;
                return GetLastError();
            }
            HANDLE cancelled[] = {stop, owner_};
            if (WaitForMultipleObjects(2, cancelled, FALSE, 100) != WAIT_TIMEOUT) return ERROR_CANCELLED;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD push(const MicPacket& packet) override {
        DWORD bytes = 0;
        return DeviceIoControl(driver_, IOCTL_AB_MIC_PUSH, const_cast<MicPacket*>(&packet),
            sizeof(packet), nullptr, 0, &bytes, nullptr) ? ERROR_SUCCESS : GetLastError();
    }
    void close() override {
        if (driver_ != INVALID_HANDLE_VALUE) { CloseHandle(driver_); driver_ = INVALID_HANDLE_VALUE; }
        // Default SwDeviceLifetimeHandle: close initiates PnP surprise removal.
        // It also waits for any create callback, protecting this object's lifetime.
        if (device_) { SwDeviceClose(device_); device_ = nullptr; }
        if (created_) { CloseHandle(created_); created_ = nullptr; }
        if (owner_) { CloseHandle(owner_); owner_ = nullptr; }
    }
private:
    static void CALLBACK onCreated(HSWDEVICE, HRESULT result, void* context, PCWSTR instanceId) {
        auto* self = static_cast<Device*>(context);
        self->createResult_ = result;
        if (SUCCEEDED(result) && instanceId) self->instanceId_ = instanceId;
        SetEvent(self->created_);
    }
    HSWDEVICE device_ = nullptr;
    HANDLE driver_ = INVALID_HANDLE_VALUE;
    HANDLE created_ = nullptr;
    HANDLE owner_ = nullptr;
    HRESULT createResult_ = E_PENDING;
    std::wstring instanceId_;
};

bool interactiveClient(HANDLE pipe) {
    // Only a local interactive user may lease the fixed microphone. The service
    // accepts no paths, executable names, hardware IDs, or arbitrary IOCTLs.
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE token = nullptr;
    bool allowed = false;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        BYTE sid[SECURITY_MAX_SID_SIZE];
        DWORD size = sizeof(sid);
        BOOL member = FALSE;
        if (CreateWellKnownSid(WinInteractiveSid, nullptr, sid, &size) &&
            CheckTokenMembership(token, sid, &member)) allowed = member != FALSE;
        CloseHandle(token);
    }
    RevertToSelf();
    return allowed;
}

void WINAPI serviceMain(DWORD, wchar_t**) {
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    statusHandle = RegisterServiceCtrlHandlerExW(AUDIOBRIDGE_MIC_SERVICE, control, nullptr);
    if (!statusHandle) { if (stopEvent) CloseHandle(stopEvent); return; }
    if (!stopEvent) { report(SERVICE_STOPPED, GetLastError()); return; }
    report(SERVICE_START_PENDING);
    PSECURITY_DESCRIPTOR sd = nullptr;
    // LocalSystem + interactive users; low-integrity callers and remote clients
    // cannot write. FR/FW omit FILE_CREATE_PIPE_INSTANCE (unlike generic write).
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;0x12019b;;;IU)S:(ML;;NW;;;ME)", SDDL_REVISION_1, &sd, nullptr)) {
        report(SERVICE_STOPPED, GetLastError()); CloseHandle(stopEvent); return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    DWORD failure = ERROR_SUCCESS;
    report(SERVICE_RUNNING);
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(AUDIOBRIDGE_MIC_PIPE,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(MicReply), sizeof(MicPacket) * 2, 1000, &sa);
        if (pipe == INVALID_HANDLE_VALUE) { failure = GetLastError(); break; }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) { failure = GetLastError(); CloseHandle(pipe); break; }
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            HANDLE waits[] = {stopEvent, ov.hEvent};
            DWORD transferred = 0;
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                connected = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
            else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &transferred, TRUE); }
        } else if (error == ERROR_PIPE_CONNECTED) connected = TRUE;
        CloseHandle(ov.hEvent);
        // Impersonation identifies the client's most recent write. Read one
        // fixed hello first, then put authorization before device creation.
        if (connected) {
            class AuthorizedDevice final : public mic::Backend {
                HANDLE pipe_; Device device_;
            public:
                explicit AuthorizedDevice(HANDLE pipe) : pipe_(pipe) {}
                DWORD open(HANDLE stop) override {
                    if (!interactiveClient(pipe_)) return ERROR_ACCESS_DENIED;
                    ULONG pid = 0;
                    if (!GetNamedPipeClientProcessId(pipe_, &pid)) return GetLastError();
                    HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, pid);
                    if (!owner) return GetLastError();
                    device_.setOwner(owner);
                    return device_.open(stop);
                }
                DWORD push(const MicPacket& p) override { return device_.push(p); }
                void close() override { device_.close(); }
            } device(pipe);
            mic::serveSession(pipe, stopEvent, device);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    LocalFree(sd);
    report(SERVICE_STOPPED, failure);
    CloseHandle(stopEvent);
}
}

int wmain() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(AUDIOBRIDGE_MIC_SERVICE), serviceMain}, {nullptr, nullptr}
    };
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
