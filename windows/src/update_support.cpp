#include "update_support.h"
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <algorithm>

namespace app_update {
static const std::vector<std::string> requiredFiles = {
    "audiobridge.exe", "audiobridge-updater.exe", "onnxruntime.dll",
    "voice-models/deepfilternet3_streaming.onnx", "voice-models/silero_vad.onnx"
};

std::array<unsigned, 3> versionParts(const std::string& version) {
    std::smatch match;
    if (!std::regex_match(version, match, std::regex("(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})")))
        throw std::runtime_error("Unsupported release version");
    std::array<unsigned, 3> result{};
    for (size_t part = 0; part < result.size(); ++part) {
        result[part] = static_cast<unsigned>(std::stoul(match[part + 1]));
        if (result[part] > 65535) throw std::runtime_error("Unsupported release version");
    }
    return result;
}

std::optional<Release> parseRelease(const std::string& json, const std::string& currentVersion) {
    using namespace winrt::Windows::Data::Json;
    const auto root = JsonObject::Parse(winrt::to_hstring(json));
    if (root.GetNamedBoolean(L"draft", true) || root.GetNamedBoolean(L"prerelease", true))
        throw std::runtime_error("No stable release is available");
    const auto tag = winrt::to_string(root.GetNamedString(L"tag_name"));
    if (tag.empty() || tag[0] != 'v') throw std::runtime_error("Unsupported release tag");
    const auto version = tag.substr(1);
    if (versionParts(version) <= versionParts(currentVersion)) return std::nullopt;
    std::optional<Release> result;
    for (const auto& value : root.GetNamedArray(L"assets")) {
        const auto asset = value.GetObject();
        if (asset.GetNamedString(L"name") != L"AudioBridge-Windows-x64.zip") continue;
        if (result) throw std::runtime_error("Duplicate release download");
        Release release;
        release.version = version;
        release.url = winrt::to_string(asset.GetNamedString(L"browser_download_url"));
        const auto digest = winrt::to_string(asset.GetNamedString(L"digest"));
        const auto size = asset.GetNamedNumber(L"size");
        if (size < 1 || size > 300 * 1024 * 1024 || size != static_cast<uint64_t>(size) ||
            !std::regex_match(digest, std::regex("sha256:[0-9a-f]{64}")))
            throw std::runtime_error("Release has no verified download");
        if (release.url != "https://github.com/Zigdon-Code-Factory/audiobridge/releases/download/" + tag + "/AudioBridge-Windows-x64.zip")
            throw std::runtime_error("Unexpected release download");
        release.size = static_cast<uint64_t>(size);
        release.sha256 = digest.substr(7);
        result = release;
    }
    if (!result) throw std::runtime_error("This release has no Windows update");
    return result;
}

bool trustedUrl(const std::wstring& address) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.nPort != 443 || parts.dwUserNameLength || parts.dwPasswordLength || address.find(L'#') != std::wstring::npos)
        return false;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::transform(host.begin(), host.end(), host.begin(), towlower);
    return host == L"api.github.com" || host == L"github.com" ||
           host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com";
}

std::string sha256File(const std::filesystem::path& file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize download verification");
    try {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Cannot initialize download verification");
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read update download");
        std::array<char, 65536> buffer;
        while (input) {
            input.read(buffer.data(), buffer.size());
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0) < 0)
                throw std::runtime_error("Cannot verify update download");
        }
        if (!input.eof()) throw std::runtime_error("Cannot read complete update download");
        std::array<unsigned char, 32> digest;
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Cannot finish download verification");
        std::string result;
        const char* digits = "0123456789abcdef";
        for (const auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

bool allowedBundlePath(const std::string& name) {
    if (std::find(requiredFiles.begin(), requiredFiles.end(), name) != requiredFiles.end() ||
        name == "msvcp140.dll" || name == "msvcp140_1.dll" || name == "vcruntime140.dll" || name == "vcruntime140_1.dll" ||
        name == "README.md" || name == "VOICE_EFFECTS.md" || name == "DEVICE_PERMISSIONS.md" || name == "UPDATING.md") return true;
    if (name.size() > 200 || name.rfind("licenses/", 0) != 0 || name.back() == '/' ||
        !std::regex_match(name, std::regex("licenses/[A-Za-z0-9_. /-]+"))) return false;
    const auto path = std::filesystem::path(name);
    for (const auto& segment : path) {
        const auto text = segment.string();
        if (text == "." || text == ".." || text.back() == '.' || text.back() == ' ') return false;
    }
    return true;
}

std::vector<std::filesystem::path> bundleFiles(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Update contains a linked path");
        if (entry.is_directory()) continue;
        const auto relative = std::filesystem::relative(entry.path(), source);
        if (!entry.is_regular_file() || !allowedBundlePath(relative.generic_string()))
            throw std::runtime_error("Update contains unexpected files");
        files.push_back(relative);
    }
    for (const auto& required : requiredFiles) {
        if (std::find(files.begin(), files.end(), std::filesystem::path(required)) == files.end())
            throw std::runtime_error("Update is missing required runtime files");
    }
    return files;
}

void applyBundle(const std::filesystem::path& source, const std::filesystem::path& target,
                 const std::filesystem::path& backup) {
    const auto files = bundleFiles(source);
    std::vector<std::filesystem::path> replaced;
    for (const auto& relative : files) {
        for (auto parent = (target / relative).parent_path(); parent != target; parent = parent.parent_path()) {
            if (parent.empty() || (std::filesystem::exists(parent) &&
                (GetFileAttributesW(parent.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("Installation contains a linked update destination");
        }
        if (std::filesystem::exists(target / relative)) {
            if (GetFileAttributesW((target / relative).c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("Installation contains a linked update file");
            std::filesystem::create_directories((backup / relative).parent_path());
            std::filesystem::copy_file(target / relative, backup / relative);
        }
    }
    try {
        for (const auto& relative : files) {
            std::filesystem::create_directories((target / relative).parent_path());
            replaced.push_back(relative);
            std::filesystem::copy_file(source / relative, target / relative, std::filesystem::copy_options::overwrite_existing);
        }
    } catch (...) {
        bool restored = true;
        for (auto entry = replaced.rbegin(); entry != replaced.rend(); ++entry) {
            try {
                if (std::filesystem::exists(backup / *entry)) {
                    if (!std::filesystem::exists(target / *entry) || sha256File(backup / *entry) != sha256File(target / *entry))
                        std::filesystem::copy_file(backup / *entry, target / *entry, std::filesystem::copy_options::overwrite_existing);
                }
                else std::filesystem::remove(target / *entry);
            } catch (...) { restored = false; }
        }
        if (!restored) throw std::runtime_error("Update failed and rollback was incomplete. Restore the files from the update backup before restarting.");
        throw std::runtime_error("Update failed. The previous runtime files were restored; close other AudioBridge instances and retry.");
    }
}

std::wstring quoteArgument(const std::wstring& value) {
    if (value.find_first_of(L"\"\r\n") != std::wstring::npos || (!value.empty() && value.back() == L'\\'))
        throw std::runtime_error("Unsupported update path");
    return L"\"" + value + L"\"";
}

bool startProcess(const std::filesystem::path& program, const std::wstring& arguments,
                  const std::filesystem::path& directory, bool hidden) {
    std::wstring command = quoteArgument(program.wstring()) + L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        hidden ? CREATE_NO_WINDOW : 0, nullptr, directory.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static std::wstring literal(std::wstring text) {
    size_t position = 0;
    while ((position = text.find(L'\'', position)) != std::wstring::npos) { text.insert(position, 1, L'\''); position += 2; }
    return L"'" + text + L"'";
}

void extractBundle(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    const std::wstring script = L"$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        L"$archive=[IO.Compression.ZipFile]::OpenRead(" + literal(archive.wstring()) + L"); $root=" + literal(destination.wstring()) + L"; "
        L"try { $total=0L; $seen=@{}; foreach($entry in $archive.Entries) { $name=$entry.FullName; "
        L"if($name.EndsWith('/')){continue}; if($name.Contains('\\') -or $name -match '(^|/)\\.\\.?(/|$)' -or $name -match '[. ](/|$)'){throw 'Unsafe update entry'}; "
        L"if($name -cnotmatch '^(audiobridge\\.exe|audiobridge-updater\\.exe|onnxruntime\\.dll|msvcp140(?:_1)?\\.dll|vcruntime140(?:_1)?\\.dll|voice-models/(deepfilternet3_streaming|silero_vad)\\.onnx|README\\.md|VOICE_EFFECTS\\.md|DEVICE_PERMISSIONS\\.md|UPDATING\\.md|licenses/[A-Za-z0-9_. /-]+)$'){throw 'Unexpected update entry'}; "
        L"if($seen.ContainsKey($name)){throw 'Duplicate update entry'}; $seen[$name]=$true; $total+=$entry.Length; if($total -gt 600MB -or $seen.Count -gt 100){throw 'Update too large'}; "
        L"$path=Join-Path $root $name; [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null; "
        L"$input=$entry.Open(); try{$output=[IO.File]::Open($path,'CreateNew','Write','None'); try{$buffer=New-Object byte[] 65536; $copied=0L; while(($count=$input.Read($buffer,0,$buffer.Length)) -gt 0){$copied+=$count; if($copied -gt $entry.Length){throw 'Invalid update size'}; $output.Write($buffer,0,$count)}; if($copied -ne $entry.Length){throw 'Truncated update'}}finally{$output.Dispose()}}finally{$input.Dispose()} "
        L"} } finally {$archive.Dispose()}";
    static const wchar_t* digits = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(script.data());
    std::wstring encoded;
    for (size_t offset = 0; offset < script.size() * 2; offset += 3) {
        const size_t remaining = script.size() * 2 - offset;
        const unsigned value = (bytes[offset] << 16) | (remaining > 1 ? bytes[offset + 1] << 8 : 0) | (remaining > 2 ? bytes[offset + 2] : 0);
        encoded += digits[(value >> 18) & 63]; encoded += digits[(value >> 12) & 63];
        encoded += remaining > 1 ? digits[(value >> 6) & 63] : L'='; encoded += remaining > 2 ? digits[value & 63] : L'=';
    }
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("Cannot locate Windows update tools");
    const auto program = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = quoteArgument(program.wstring()) + L" -NoProfile -NonInteractive -EncodedCommand " + encoded;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, destination.c_str(), &startup, &process))
        throw std::runtime_error("Cannot unpack the update");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || code != 0) throw std::runtime_error("Could not safely unpack the update; installed files are unchanged");
    bundleFiles(destination);
}
}
