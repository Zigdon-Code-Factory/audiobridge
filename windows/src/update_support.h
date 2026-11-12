#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace app_update {
struct Release {
    std::string version;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};
std::array<unsigned, 3> versionParts(const std::string& version);
std::optional<Release> parseRelease(const std::string& json, const std::string& currentVersion);
bool trustedUrl(const std::wstring& address);
std::string sha256File(const std::filesystem::path& file);
bool allowedBundlePath(const std::string& name);
std::vector<std::filesystem::path> bundleFiles(const std::filesystem::path& source);
void applyBundle(const std::filesystem::path& source, const std::filesystem::path& target,
                 const std::filesystem::path& backup);
void extractBundle(const std::filesystem::path& archive, const std::filesystem::path& destination);
bool startProcess(const std::filesystem::path& program, const std::wstring& arguments,
                  const std::filesystem::path& directory, bool hidden);
std::wstring quoteArgument(const std::wstring& value);
}
