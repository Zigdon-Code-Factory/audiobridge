#include "app_updater.h"
#include "app_version.h"
#include <Windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <winrt/base.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <stdexcept>

namespace {
struct InternetHandle {
    HINTERNET value;
    explicit InternetHandle(HINTERNET handle) : value(handle) {
        if (!value) throw std::runtime_error("Cannot connect to GitHub");
    }
    ~InternetHandle() { WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
};

void get(const std::wstring& initial, uint64_t limit, const std::atomic<bool>& closing,
         const std::function<void(const char*, size_t)>& consume) {
    InternetHandle session(WinHttpOpen(L"AudioBridge/" AUDIOBRIDGE_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 5000, 10000, 15000, 15000);
    auto address = initial;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    for (unsigned redirect = 0; redirect < 5; ++redirect) {
        if (!app_update::trustedUrl(address)) throw std::runtime_error("Untrusted update address");
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) throw std::runtime_error("Invalid update address");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        InternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
        InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        if (closing || !WinHttpSendRequest(request.value, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr))
            throw std::runtime_error("GitHub could not be reached; check your connection and retry");
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
            throw std::runtime_error("Cannot read the GitHub response");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            length = 0;
            WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &length, WINHTTP_NO_HEADER_INDEX);
            if (!length || length > 32768) throw std::runtime_error("Invalid update redirect");
            std::wstring location(length / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &length, WINHTTP_NO_HEADER_INDEX)) throw std::runtime_error("Invalid update redirect");
            while (!location.empty() && location.back() == L'\0') location.pop_back();
            address = location;
            continue;
        }
        if (status != 200) throw std::runtime_error("GitHub returned HTTP " + std::to_string(status) + "; retry later");
        std::array<char, 65536> buffer;
        uint64_t total = 0;
        for (;;) {
            if (closing || std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Update download interrupted or timed out");
            DWORD received = 0;
            if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received))
                throw std::runtime_error("Update download was interrupted; retry the update");
            if (!received) break;
            total += received;
            if (total > limit) throw std::runtime_error("Update exceeds the published download size");
            consume(buffer.data(), received);
        }
        return;
    }
    throw std::runtime_error("Too many update redirects");
}

std::string jsonText(const std::string& text) {
    std::string result;
    for (const auto character : text) {
        if (character == '"' || character == '\\') result += '\\';
        if (static_cast<unsigned char>(character) >= 32) result += character;
    }
    return "\"" + result + "\"";
}
}

AppUpdater::AppUpdater(std::function<void()> restart) : restart_(std::move(restart)) {}
AppUpdater::~AppUpdater() { closing_ = true; if (worker_.joinable()) worker_.join(); }

void AppUpdater::state(const std::string& phase, const std::string& message, double progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = phase; message_ = message; progress_ = progress;
}

std::string AppUpdater::stateJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream json;
    json << "{\"currentVersion\":\"" AUDIOBRIDGE_VERSION "\",\"phase\":" << jsonText(phase_)
         << ",\"message\":" << jsonText(message_) << ",\"progress\":" << progress_;
    if (release_) json << ",\"version\":" << jsonText(release_->version);
    return json.str() + "}";
}

void AppUpdater::check() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == "checking" || phase_ == "downloading" || phase_ == "restarting") return;
        phase_ = "checking"; message_ = "Checking the official GitHub release..."; progress_ = 0; release_.reset();
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) { state("error", "Cannot initialize the update service; restart AudioBridge and retry."); return; }
        try {
            std::string json;
            get(L"https://api.github.com/repos/Zigdon-Code-Factory/audiobridge/releases/latest", 2 * 1024 * 1024, closing_,
                [&](const char* bytes, size_t length) { json.append(bytes, length); });
            const auto release = app_update::parseRelease(json, AUDIOBRIDGE_VERSION);
            { std::lock_guard<std::mutex> lock(mutex_); release_ = release; }
            state(release ? "available" : "current", release ? "Version " + release->version + " is available. Your pairings and settings are kept." :
                  "You are up to date. Installed version: " AUDIOBRIDGE_VERSION ".");
        } catch (const std::exception& error) { state("error", std::string("Could not check for updates: ") + error.what() + ". Installed files are unchanged."); }
        catch (...) { state("error", "Could not validate the release information. Installed files are unchanged; retry later."); }
        CoUninitialize();
    });
}

void AppUpdater::install() {
    app_update::Release release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != "available" || !release_) return;
        release = *release_; phase_ = "downloading"; message_ = "Downloading and verifying the update..."; progress_ = 0;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, release] {
        HANDLE ready = nullptr, proceed = nullptr;
        try {
            wchar_t image[32768], temporary[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, image, 32768);
            if (!length || length >= 32768 || !GetTempPathW(MAX_PATH, temporary)) throw std::runtime_error("Cannot locate the installation or update cache");
            const std::filesystem::path executable(image), target = executable.parent_path();
            if (!std::filesystem::is_regular_file(target / L"audiobridge-updater.exe")) throw std::runtime_error("Keep audiobridge-updater.exe beside audiobridge.exe; restore the complete Windows release ZIP");
            GUID identifier;
            if (FAILED(CoCreateGuid(&identifier))) throw std::runtime_error("Cannot create an update transaction");
            wchar_t unique[40]; StringFromGUID2(identifier, unique, 40);
            const auto folder = std::filesystem::path(temporary) / (std::wstring(L"AudioBridge-Update-") + unique);
            if (!std::filesystem::create_directory(folder)) throw std::runtime_error("Cannot create a private update folder");
            const auto writable = target / (std::wstring(L".audiobridge-update-") + unique);
            { std::ofstream test(writable); if (!test) throw std::runtime_error("This installation folder is not writable. Move the complete app folder to your user folder and retry"); }
            std::filesystem::remove(writable);
            const auto archive = folder / L"update.zip";
            std::ofstream output(archive, std::ios::binary);
            if (!output) throw std::runtime_error("Cannot save the update download");
            uint64_t received = 0;
            get(std::wstring(release.url.begin(), release.url.end()), release.size, closing_, [&](const char* bytes, size_t count) {
                output.write(bytes, count);
                if (!output) throw std::runtime_error("Cannot save the update; check free disk space");
                received += count;
                state("downloading", "Downloading and verifying the update...", static_cast<double>(received) / release.size);
            });
            output.close();
            if (!output || received != release.size || app_update::sha256File(archive) != release.sha256)
                throw std::runtime_error("Download verification failed; check again and retry");
            if (closing_) throw std::runtime_error("Update canceled while closing the app");
            const auto helper = folder / L"audiobridge-updater.exe";
            std::filesystem::copy_file(target / L"audiobridge-updater.exe", helper);
            for (const auto* runtime : {L"msvcp140.dll", L"msvcp140_1.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
                if (std::filesystem::is_regular_file(target / runtime))
                    std::filesystem::copy_file(target / runtime, folder / runtime);
            }
            const std::wstring readyName = std::wstring(L"Local\\AudioBridge-Update-Ready-") + unique;
            const std::wstring proceedName = std::wstring(L"Local\\AudioBridge-Update-Proceed-") + unique;
            ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
            proceed = CreateEventW(nullptr, TRUE, FALSE, proceedName.c_str());
            if (!ready || !proceed) throw std::runtime_error("Cannot prepare the update handoff");
            const auto arguments = app_update::quoteArgument(target.wstring()) + L" " + app_update::quoteArgument(archive.wstring()) +
                L" " + app_update::quoteArgument(std::wstring(release.sha256.begin(), release.sha256.end())) + L" " + std::to_wstring(GetCurrentProcessId()) +
                L" " + app_update::quoteArgument(executable.wstring()) + L" " + app_update::quoteArgument(readyName) + L" " + app_update::quoteArgument(proceedName);
            if (!app_update::startProcess(helper, arguments, folder, true) || WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0 || closing_)
                throw std::runtime_error("The update helper did not become ready. Installed files are unchanged; retry later");
            state("restarting", "Download verified. Stopping audio and restarting to install the update...", 1);
            SetEvent(proceed);
            restart_();
        } catch (const std::exception& error) { state("error", std::string("Could not prepare the update: ") + error.what() + ". Your pairings and settings are unchanged."); }
        catch (...) { state("error", "Could not prepare the update. Your installed app, pairings and settings are unchanged."); }
        if (ready) CloseHandle(ready);
        if (proceed) CloseHandle(proceed);
    });
}
