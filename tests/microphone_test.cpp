#include <Windows.h>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include "../windows/src/mic_service_session.h"
#include "../windows/src/virtual_microphone.h"
#include "../common/mic_ring.h"

struct FakeDevice final : mic::Backend {
    unsigned opens = 0, closes = 0, writes = 0;
    DWORD openError = 0, pushError = 0;
    bool present = false;
    short last = 0;
    DWORD open(HANDLE) override { ++opens; present = !openError; return openError; }
    DWORD push(const MicPacket& p) override { assert(present); ++writes; last = p.samples[p.frames - 1]; return pushError; }
    void close() override { ++closes; present = false; }
};

struct Session {
    std::wstring name;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE pipe = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    FakeDevice device;
    std::thread server;
    explicit Session(bool connect = true) {
        static unsigned sequence = 0;
        name = L"\\\\.\\pipe\\AudioBridge.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
        pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 1000, nullptr);
        assert(pipe != INVALID_HANDLE_VALUE && stop);
        if (connect) connectClient();
    }
    void connectClient() {
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        assert(client != INVALID_HANDLE_VALUE);
        DWORD mode = PIPE_READMODE_MESSAGE;
        assert(SetNamedPipeHandleState(client, &mode, nullptr, nullptr));
    }
    void start() {
        server = std::thread([&] {
            OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL connected = ConnectNamedPipe(pipe, &ov);
            DWORD error = connected ? 0 : GetLastError();
            if (error == ERROR_IO_PENDING) {
                DWORD done = 0;
                assert(WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0);
                assert(GetOverlappedResult(pipe, &ov, &done, FALSE));
            } else assert(connected || error == ERROR_PIPE_CONNECTED);
            CloseHandle(ov.hEvent);
            mic::serveSession(pipe, stop, device);
        });
    }
    MicReply exchange(const MicPacket& packet) {
        MicReply reply;
        assert(mic::transfer(client, true, const_cast<MicPacket*>(&packet), sizeof(packet), stop, 2000));
        assert(mic::transfer(client, false, &reply, sizeof(reply), stop, 2000));
        assert(reply.magic == AB_MIC_MAGIC && reply.version == AB_MIC_VERSION);
        return reply;
    }
    void disconnect() { CloseHandle(client); client = INVALID_HANDLE_VALUE; }
    void join() { if (server.joinable()) server.join(); }
    ~Session() {
        SetEvent(stop); if (client != INVALID_HANDLE_VALUE) disconnect(); join();
        CloseHandle(pipe); CloseHandle(stop);
    }
};

void ringTest() {
    MicRing<1920> ring;
    std::deque<short> model;
    std::mt19937 random(42);
    for (unsigned step = 0; step < 20000; ++step) {
        if (random() % 3) {
            short values[2400]; const unsigned count = random() % 2401;
            for (unsigned i = 0; i < count; ++i) { values[i] = static_cast<short>(random()); model.push_back(values[i]); }
            while (model.size() > 1920) model.pop_front();
            ring.push(values, count);
        } else {
            const unsigned count = random() % 2400;
            for (unsigned i = 0; i < count; ++i) {
                const short expected = model.empty() ? 0 : model.front();
                if (!model.empty()) model.pop_front();
                assert(ring.pop() == expected);
            }
        }
        assert(ring.size() == model.size());
    }
    ring.clear(); assert(ring.size() == 0 && ring.pop() == 0);
    MicPlaybackQueue playback;
    short block[576]; for (auto& sample : block) sample = 123;
    playback.push(block, 480);
    assert(playback.pop() == 0 && playback.size() == 480);
    playback.push(block, 96);
    for (unsigned i = 0; i < 576; ++i) assert(playback.pop() == 123);
    assert(playback.pop() == 0);
    playback.push(block, 100);
    for (unsigned i = 0; i < 4800; ++i) assert(playback.pop() == 0);
    assert(playback.size() == 0);
    playback.push(block, 576); assert(playback.pop() == 123);
    playback.clear(); assert(playback.pop() == 0 && playback.size() == 0);
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--child") {
        HANDLE pipe = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        assert(pipe != INVALID_HANDLE_VALUE);
        MicPacket packet;
        MicReply reply;
        assert(mic::transfer(pipe, true, &packet, sizeof(packet), stop, 2000));
        assert(mic::transfer(pipe, false, &reply, sizeof(reply), stop, 2000));
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        assert(ready && SetEvent(ready)); CloseHandle(ready);
        // Parent terminates this disposable child to exercise abrupt pipe EOF.
        Sleep(INFINITE); return 0;
    }
    ringTest();
    {
        Session s; s.start(); MicPacket p;
        assert(s.exchange(p).error == 0);
        p.frames = 960; p.samples[959] = -1234;
        assert(s.exchange(p).error == 0);
        p.frames = 0; assert(s.exchange(p).error == 0);
        s.disconnect(); s.join();
        assert(s.device.opens == 1 && s.device.closes == 1 && s.device.writes == 1 && s.device.last == -1234 && !s.device.present);
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        Session s; s.start(); MicPacket p;
        if (invalid != 4) assert(s.exchange(p).error == 0);
        if (invalid == 0) p.version++;
        if (invalid == 1) p.frames = AB_MIC_MAX_FRAMES + 1;
        if (invalid == 2) p.reserved = 1;
        if (invalid == 4) p.frames = 1; // First message must be an empty hello.
        const DWORD bytes = invalid == 3 ? 12 : sizeof(p);
        assert(mic::transfer(s.client, true, &p, bytes, s.stop, 2000));
        s.join(); assert(!s.device.present && s.device.closes == 1 && !s.device.writes);
        assert(s.device.opens == (invalid == 4 ? 0u : 1u));
    }
    {
        Session s; s.device.openError = ERROR_DEVICE_NOT_CONNECTED; s.start();
        assert(s.exchange(MicPacket{}).error == ERROR_DEVICE_NOT_CONNECTED);
        s.join(); assert(!s.device.present && s.device.closes == 1);
    }
    {
        Session s; s.device.pushError = ERROR_DEVICE_NOT_CONNECTED; s.start();
        assert(!s.exchange(MicPacket{}).error);
        MicPacket p; p.frames = 48;
        assert(s.exchange(p).error == ERROR_DEVICE_NOT_CONNECTED);
        s.join(); assert(!s.device.present && s.device.closes == 1);
    }
    {
        Session s; s.start(); assert(!s.exchange(MicPacket{}).error);
        const auto before = std::chrono::steady_clock::now();
        SetEvent(s.stop); s.join();
        assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
        assert(!s.device.present && s.device.closes == 1);
    }
    {
        Session s(false); s.start();
        wchar_t exe[MAX_PATH]; assert(GetModuleFileNameW(nullptr, exe, MAX_PATH));
        const std::wstring eventName = L"Local\\AudioBridge.MicTest." + std::to_wstring(GetCurrentProcessId());
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        assert(ready);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --child " + s.name + L" " + eventName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        assert(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
        assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
        CloseHandle(ready);
        assert(TerminateProcess(child.hProcess, 23));
        assert(WaitForSingleObject(child.hProcess, 2000) == WAIT_OBJECT_0);
        s.join(); CloseHandle(child.hThread); CloseHandle(child.hProcess);
        assert(s.device.opens == 1 && s.device.closes == 1 && !s.device.present);
    }
    // Real-time IPC soak uses the production session code with a fake driver;
    // it deliberately does not claim to validate Windows audio endpoint behavior.
    if (argc == 3 && std::wstring(argv[1]) == L"--soak") {
        const int seconds = _wtoi(argv[2]);
        Session s; s.start(); assert(!s.exchange(MicPacket{}).error);
        MicPacket p; p.frames = 960;
        auto deadline = std::chrono::steady_clock::now();
        for (int packet = 0; packet < seconds * 50; ++packet) {
            p.samples[959] = static_cast<short>(packet);
            assert(!s.exchange(p).error);
            deadline += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(deadline);
        }
        s.disconnect(); s.join();
        assert(s.device.writes == static_cast<unsigned>(seconds * 50) && !s.device.present);
        std::cout << "IPC soak passed: " << seconds << " seconds, " << s.device.writes << " packets\n";
    }
    std::cout << "Microphone queue, protocol, failure, cancellation, and crash-cleanup tests passed\n";
}
