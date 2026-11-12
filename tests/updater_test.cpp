#include "update_support.h"
#include "app_updater.h"
#include <Windows.h>
#include <winrt/base.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <functional>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
static void rejects(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (...) { failed = true; }
    assert(failed);
}
static void write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}
static std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void integer(std::ostream& output, uint32_t value, unsigned bytes) {
    for (unsigned part = 0; part < bytes; ++part) output.put(static_cast<char>((value >> (part * 8)) & 255));
}
static void zip(const fs::path& path, const std::vector<std::string>& names) {
    std::ofstream output(path, std::ios::binary);
    std::vector<uint32_t> offsets;
    for (const auto& name : names) {
        offsets.push_back(static_cast<uint32_t>(output.tellp()));
        integer(output, 0x04034b50, 4); integer(output, 20, 2); integer(output, 0, 2);
        integer(output, 0, 2); integer(output, 0, 4); integer(output, 0x8cdc1683, 4);
        integer(output, 1, 4); integer(output, 1, 4); integer(output, static_cast<uint32_t>(name.size()), 2); integer(output, 0, 2);
        output << name << 'x';
    }
    const auto start = static_cast<uint32_t>(output.tellp());
    for (size_t entry = 0; entry < names.size(); ++entry) {
        const auto& name = names[entry];
        integer(output, 0x02014b50, 4); integer(output, 20, 2); integer(output, 20, 2); integer(output, 0, 2);
        integer(output, 0, 2); integer(output, 0, 4); integer(output, 0x8cdc1683, 4);
        integer(output, 1, 4); integer(output, 1, 4); integer(output, static_cast<uint32_t>(name.size()), 2);
        integer(output, 0, 2); integer(output, 0, 2); integer(output, 0, 2); integer(output, 0, 2);
        integer(output, 0, 4); integer(output, offsets[entry], 4); output << name;
    }
    const auto end = static_cast<uint32_t>(output.tellp());
    integer(output, 0x06054b50, 4); integer(output, 0, 2); integer(output, 0, 2);
    integer(output, static_cast<uint32_t>(names.size()), 2); integer(output, static_cast<uint32_t>(names.size()), 2);
    integer(output, end - start, 4); integer(output, start, 4); integer(output, 0, 2);
}
int main(int argumentCount, char** arguments) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace app_update;
    if (argumentCount == 2 && std::string(arguments[1]) == "--online") {
        AppUpdater updater([] {});
        updater.check();
        for (unsigned attempt = 0; attempt < 80; ++attempt) {
            const auto state = updater.stateJson();
            if (state.find("\"phase\":\"checking\"") == std::string::npos) {
                std::cout << state << "\n";
                return state.find("\"phase\":\"error\"") == std::string::npos ? 0 : 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 1;
    }
    if (argumentCount == 2) {
        try {
        const auto destination = fs::temp_directory_path() / ("audiobridge-release-check-" + std::to_string(GetTickCount64()));
        fs::create_directory(destination);
        extractBundle(fs::absolute(arguments[1]), destination);
        std::cout << "Release archive verified: " << bundleFiles(destination).size() << " allowed runtime/documentation files\n";
        return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
    }
    assert(versionParts("0.2.10") > versionParts("0.2.9"));
    assert(versionParts("1.0.0") > versionParts("0.99.99"));
    for (const auto& value : {"0.2", "v0.2.0", "0.2.0-beta", "00.2.0", "65536.0.0", "-1.2.3", "0.2.0.1"})
        rejects([&] { versionParts(value); });
    const std::string digest(64, 'a');
    const std::string metadata = "{\"tag_name\":\"v0.2.1\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"AudioBridge-Windows-x64.zip\",\"size\":123,\"digest\":\"sha256:" +
        digest + "\",\"browser_download_url\":\"https://github.com/Zigdon-Code-Factory/audiobridge/releases/download/v0.2.1/AudioBridge-Windows-x64.zip\"}]}";
    assert(parseRelease(metadata, "0.2.0")->sha256 == digest);
    assert(!parseRelease(metadata, "0.2.1"));
    auto invalid = metadata; invalid.replace(invalid.find("github.com/"), 10, "evil.test/");
    rejects([&] { parseRelease(invalid, "0.2.0"); });
    invalid = metadata; invalid.replace(invalid.find("sha256:"), 7, "sha512:");
    rejects([&] { parseRelease(invalid, "0.2.0"); });
    invalid = metadata; invalid.replace(invalid.find("\"draft\":false"), 13, "\"draft\":true");
    rejects([&] { parseRelease(invalid, "0.2.0"); });
    assert(trustedUrl(L"https://github.com/Zigdon-Code-Factory/audiobridge/releases/latest"));
    assert(trustedUrl(L"https://release-assets.githubusercontent.com/download?signature=123"));
    for (const auto* address : {L"http://github.com/file", L"https://github.com.evil.test/file", L"https://github.com:444/file",
                               L"https://user@github.com/file", L"https://github.com/file#fragment", L"file:///tmp/update"})
        assert(!trustedUrl(address));
    for (const auto& name : {"../audiobridge.exe", "/audiobridge.exe", "approved_peers.txt", "audiobridge_settings.txt",
                             "licenses/../audiobridge.exe", "licenses/./file", "licenses/path./file", "licenses/file:stream",
                             "licenses\\a.dll", "other.exe"})
        assert(!allowedBundlePath(name));
    assert(allowedBundlePath("licenses/onnxruntime/LICENSE"));
    const auto root = fs::temp_directory_path() / ("audiobridge-updater-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directory(root);
    write(root / "hash", "abc");
    assert(sha256File(root / "hash") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    rejects([&] { sha256File(root / "missing"); });
    const std::vector<std::string> names = {"audiobridge.exe", "audiobridge-updater.exe", "onnxruntime.dll",
        "voice-models/deepfilternet3_streaming.onnx", "voice-models/silero_vad.onnx", "licenses/onnxruntime/LICENSE"};
    for (const auto& name : names) {
        write(root / "new" / name, "new");
        write(root / "installed" / name, "old");
    }
    write(root / "installed/approved_peers.txt", "private pairing");
    write(root / "installed/audiobridge_settings.txt", "preferences");
    applyBundle(root / "new", root / "installed", root / "backup");
    for (const auto& name : names) assert(read(root / "installed" / name) == "new");
    assert(read(root / "installed/approved_peers.txt") == "private pairing");
    assert(read(root / "installed/audiobridge_settings.txt") == "preferences");
    for (const auto& name : names) write(root / "installed" / name, "old");
    const auto locked = CreateFileW((root / "installed/onnxruntime.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    rejects([&] { applyBundle(root / "new", root / "installed", root / "rollback"); });
    CloseHandle(locked);
    for (const auto& name : names) assert(read(root / "installed" / name) == "old");
    assert(read(root / "installed/approved_peers.txt") == "private pairing");
    zip(root / "valid.zip", names);
    fs::create_directory(root / "unpacked");
    extractBundle(root / "valid.zip", root / "unpacked");
    for (const auto& name : names) assert(read(root / "unpacked" / name) == "x");
    zip(root / "unsafe.zip", {"../outside.exe"});
    fs::create_directory(root / "unsafe");
    rejects([&] { extractBundle(root / "unsafe.zip", root / "unsafe"); });
    assert(!fs::exists(root / "outside.exe"));
    zip(root / "duplicate.zip", {"audiobridge.exe", "AUDIOBRIDGE.EXE"});
    fs::create_directory(root / "duplicate");
    rejects([&] { extractBundle(root / "duplicate.zip", root / "duplicate"); });
    std::cout << "Updater policy, verified metadata, hashing, extraction, rollback and pairing preservation passed\n";
}
