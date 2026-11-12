#include "update_support.h"
#include <Windows.h>
#include <shellapi.h>
#include <fstream>
#include <stdexcept>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 8) { if (arguments) LocalFree(arguments); return 2; }
    const std::filesystem::path target = arguments[1];
    const std::filesystem::path archive = arguments[2];
    const std::wstring expectedWide = arguments[3];
    std::string expected;
    for (const auto character : expectedWide) {
        if ((character < L'0' || character > L'9') && (character < L'a' || character > L'f')) { LocalFree(arguments); return 2; }
        expected += static_cast<char>(character);
    }
    const auto parent = wcstoul(arguments[4], nullptr, 10);
    const std::filesystem::path executable = arguments[5];
    const std::wstring readyName = arguments[6], proceedName = arguments[7];
    LocalFree(arguments);
    HANDLE process = nullptr;
    try {
        if (executable.filename() != L"audiobridge.exe" || std::filesystem::canonical(executable.parent_path()) != std::filesystem::canonical(target) ||
            archive.filename() != L"update.zip" || expected.size() != 64) throw std::runtime_error("Invalid update request");
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (!process) throw std::runtime_error("Cannot confirm the AudioBridge process; reopen the app and retry");
        wchar_t image[32768]; DWORD length = 32768;
        if (!QueryFullProcessImageNameW(process, 0, image, &length) || std::filesystem::canonical(image) != std::filesystem::canonical(executable))
            throw std::runtime_error("Update process identity did not match");
        const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
        const auto proceed = OpenEventW(SYNCHRONIZE, FALSE, proceedName.c_str());
        if (!ready || !proceed) {
            if (ready) CloseHandle(ready);
            if (proceed) CloseHandle(proceed);
            throw std::runtime_error("Update handoff expired; no installed files were changed");
        }
        SetEvent(ready); CloseHandle(ready);
        const auto confirmed = WaitForSingleObject(proceed, 15000);
        CloseHandle(proceed);
        if (confirmed != WAIT_OBJECT_0) throw std::runtime_error("Update was canceled; no installed files were changed");
        if (app_update::sha256File(archive) != expected) throw std::runtime_error("Update verification failed");
        const auto unpacked = archive.parent_path() / L"unpacked";
        if (!std::filesystem::create_directory(unpacked)) throw std::runtime_error("Update extraction folder already exists");
        app_update::extractBundle(archive, unpacked);
        if (WaitForSingleObject(process, 60000) != WAIT_OBJECT_0) throw std::runtime_error("AudioBridge did not close. No installed files were changed.");
        CloseHandle(process); process = nullptr;
        app_update::applyBundle(unpacked, target, archive.parent_path() / L"backup");
        if (!app_update::startProcess(executable, L"", target, false)) throw std::runtime_error("Update installed. Open audiobridge.exe to restart it.");
        return 0;
    } catch (const std::exception& error) {
        if (process) CloseHandle(process);
        std::ofstream log(archive.parent_path() / L"update-error.txt");
        log << error.what() << "\nUpdate folder: " << archive.parent_path().u8string();
        MessageBoxA(nullptr, error.what(), "AudioBridge update needs attention", MB_OK | MB_ICONERROR);
        return 1;
    }
}
