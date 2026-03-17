#include "gui.h"
#include "gui_html.h"
#include "network.h"

#include <sstream>
#include <ctime>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#endif

// ---- Helpers ----

std::string ServerGui::wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
    return s;
#else
    // Linux: wstring is UTF-32, convert via simple loop
    std::string s;
    s.reserve(ws.size() * 4);
    for (wchar_t wc : ws) {
        if (wc < 0x80) {
            s += (char)wc;
        } else if (wc < 0x800) {
            s += (char)(0xC0 | (wc >> 6));
            s += (char)(0x80 | (wc & 0x3F));
        } else if (wc < 0x10000) {
            s += (char)(0xE0 | (wc >> 12));
            s += (char)(0x80 | ((wc >> 6) & 0x3F));
            s += (char)(0x80 | (wc & 0x3F));
        } else {
            s += (char)(0xF0 | (wc >> 18));
            s += (char)(0x80 | ((wc >> 12) & 0x3F));
            s += (char)(0x80 | ((wc >> 6) & 0x3F));
            s += (char)(0x80 | (wc & 0x3F));
        }
    }
    return s;
#endif
}

std::string ServerGui::escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Parse first integer from JSON array like "[15]"
static int parseFirstInt(const std::string& args) {
    // Find first digit or minus
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == '-' || (args[i] >= '0' && args[i] <= '9')) {
            return std::atoi(args.c_str() + i);
        }
    }
    return 0;
}

// Parse first string from JSON array like '["value"]'
static std::string parseFirstString(const std::string& args) {
    auto start = args.find('"');
    if (start == std::string::npos) return "";
    start++;
    std::string result;
    for (size_t i = start; i < args.size(); i++) {
        if (args[i] == '"') break;
        if (args[i] == '\\' && i + 1 < args.size()) {
            i++;
            result += args[i];
        } else {
            result += args[i];
        }
    }
    return result;
}

// ---- Constructor / Destructor ----

ServerGui::ServerGui() {}
ServerGui::~ServerGui() {}

// ---- Initialize ----

bool ServerGui::initialize(const std::string& title) {
    try {
        webview_ = std::make_unique<webview::webview>(
#ifdef _DEBUG
            true,
#else
            false,
#endif
            nullptr
        );
    } catch (const webview::exception& e) {
        fprintf(stderr, "Failed to create webview: %s\n", e.what());
        return false;
    }

    webview_->set_title("AudioBridge -- " + title);
    webview_->set_size(720, 900, WEBVIEW_HINT_NONE);

    setupBindings();
    webview_->set_html(gui_html::HTML_CONTENT);

    return true;
}

// ---- Run ----

void ServerGui::run() {
    if (webview_) webview_->run();
}

// ---- Bindings ----

void ServerGui::setupBindings() {
    // _tick: called every 50ms by JS setInterval
    webview_->bind("_tick", [this](const std::string& /*args*/) -> std::string {
        if (onTick_) onTick_();
        return buildStateJson();
    });

    // _setJitter: user moved jitter slider
    webview_->bind("_setJitter", [this](const std::string& args) -> std::string {
        int ms = parseFirstInt(args);
        if (ms < 0) ms = 0;
        if (ms > 50) ms = 50;
        jitterBufferMs_ = ms;
        if (onJitterChange_) onJitterChange_(ms);
        return "{}";
    });

    // _approvePair: user clicked Approve
    webview_->bind("_approvePair", [this](const std::string& args) -> std::string {
        std::string clientId = parseFirstString(args);
        if (!clientId.empty() && onPairApprove_) {
            onPairApprove_(clientId);
            clearPairRequest();
        }
        return "{}";
    });

    // _denyPair: user clicked Deny
    webview_->bind("_denyPair", [this](const std::string& args) -> std::string {
        std::string clientId = parseFirstString(args);
        if (!clientId.empty() && onPairDeny_) {
            onPairDeny_(clientId);
            clearPairRequest();
        }
        return "{}";
    });

    // _revokePeer: user clicked revoke on a paired device
    webview_->bind("_revokePeer", [this](const std::string& args) -> std::string {
        std::string clientId = parseFirstString(args);
        if (!clientId.empty() && onRevoke_) {
            onRevoke_(clientId);
        }
        return "{}";
    });

    // _selectDevice: user changed input device dropdown
    webview_->bind("_selectDevice", [this](const std::string& args) -> std::string {
        int index = parseFirstInt(args);
        std::wstring deviceId;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            if (index == 0) {
                deviceId = L"";  // default device
            } else if (index - 1 < (int)audioDevices_.size()) {
                deviceId = audioDevices_[index - 1].id;
            }
        }
        if (onDeviceChange_) onDeviceChange_(deviceId);
        return "{}";
    });

    // _selectOutDevice: user changed output device dropdown
    webview_->bind("_selectOutDevice", [this](const std::string& args) -> std::string {
        int index = parseFirstInt(args);
        std::wstring deviceId;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            if (index == 0) {
                deviceId = L"";
            } else if (index - 1 < (int)outAudioDevices_.size()) {
                deviceId = outAudioDevices_[index - 1].id;
            }
        }
        if (onOutDeviceChange_) onOutDeviceChange_(deviceId);
        return "{}";
    });
}

// ---- State JSON Builder ----

int ServerGui::getCurrentDeviceIndex() const {
    // Must be called with dataMutex_ held
    if (currentDeviceId_.empty()) return 0;
    for (size_t i = 0; i < audioDevices_.size(); i++) {
        if (audioDevices_[i].id == currentDeviceId_) return (int)(i + 1);
    }
    return 0;
}

int ServerGui::getCurrentOutDeviceIndex() const {
    // Must be called with dataMutex_ held
    if (currentOutDeviceId_.empty()) return 0;
    for (size_t i = 0; i < outAudioDevices_.size(); i++) {
        if (outAudioDevices_[i].id == currentOutDeviceId_) return (int)(i + 1);
    }
    return 0;
}

// Safe float-to-string that always uses '.' and handles nan/inf
static std::string jsonFloat(double v) {
    if (v != v) return "0";           // nan
    if (v > 1e308 || v < -1e308) return "0"; // inf
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string ServerGui::buildStateJson() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::string s;
    s.reserve(2048);
    s += "{";
    s += "\"connected\":" + std::string(stats_.connected ? "true" : "false") + ",";
    s += "\"paused\":" + std::string(stats_.paused ? "true" : "false") + ",";
    s += "\"clientName\":\"" + escapeJson(stats_.clientName) + "\",";
    s += "\"clientAddress\":\"" + escapeJson(stats_.clientAddress) + "\",";
    s += "\"packetsSent\":" + std::to_string(stats_.packetsSent) + ",";
    s += "\"bytesSent\":" + std::to_string(stats_.bytesSent) + ",";
    s += "\"kbps\":" + jsonFloat(stats_.kbps) + ",";
    s += "\"peakLevel\":" + jsonFloat(stats_.peakLevel) + ",";
    s += "\"jitterBufferMs\":" + std::to_string(stats_.jitterBufferMs) + ",";
    s += "\"sequenceNum\":" + std::to_string(stats_.sequenceNum) + ",";
    s += "\"uptimeSeconds\":" + jsonFloat(stats_.uptimeSeconds) + ",";
    s += "\"serverName\":\"" + escapeJson(stats_.serverName) + "\",";
    s += "\"serverMac\":\"" + escapeJson(stats_.serverMac) + "\",";

    // Pair request
    s += "\"pairRequest\":{";
    s += "\"pending\":" + std::string(pairRequest_.pending ? "true" : "false") + ",";
    s += "\"clientId\":\"" + escapeJson(pairRequest_.clientId) + "\",";
    s += "\"clientName\":\"" + escapeJson(pairRequest_.clientName) + "\"";
    s += "},";

    // Peers
    s += "\"peers\":[";
    for (size_t i = 0; i < approvedPeers_.size(); i++) {
        if (i > 0) s += ",";
        int64_t ts = (i < peerTimestamps_.size()) ? peerTimestamps_[i] : 0;
        s += "{\"id\":\"" + escapeJson(approvedPeers_[i].first) + "\",";
        s += "\"name\":\"" + escapeJson(approvedPeers_[i].second) + "\",";
        s += "\"lastConnected\":" + std::to_string(ts) + "}";
    }
    s += "],";

    // Logs
    s += "\"logs\":[";
    for (size_t i = 0; i < logMessages_.size(); i++) {
        if (i > 0) s += ",";
        s += "\"" + escapeJson(logMessages_[i]) + "\"";
    }
    s += "],";

    // Device lists - always include
    bool devChanged = devicesChanged_;
    if (devicesChanged_) devicesChanged_ = false;
    s += "\"devicesChanged\":" + std::string(devChanged ? "true" : "false") + ",";
    s += "\"devices\":[";
    for (size_t i = 0; i < audioDevices_.size(); i++) {
        if (i > 0) s += ",";
        s += "\"" + escapeJson(audioDevices_[i].name) + "\"";
    }
    s += "],\"currentDeviceIndex\":" + std::to_string(getCurrentDeviceIndex()) + ",";

    bool outDevChanged = outDevicesChanged_;
    if (outDevicesChanged_) outDevicesChanged_ = false;
    s += "\"outDevicesChanged\":" + std::string(outDevChanged ? "true" : "false") + ",";
    s += "\"outDevices\":[";
    for (size_t i = 0; i < outAudioDevices_.size(); i++) {
        if (i > 0) s += ",";
        s += "\"" + escapeJson(outAudioDevices_[i].name) + "\"";
    }
    s += "],\"currentOutDeviceIndex\":" + std::to_string(getCurrentOutDeviceIndex());

    s += "}";
    return s;
}

// ---- Thread-safe update methods ----

void ServerGui::updateStats(const ServerStats& stats) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    stats_ = stats;
}

void ServerGui::updateApprovedPeers(const std::map<std::string, ApprovedPeer>& peers) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    approvedPeers_.clear();
    peerTimestamps_.clear();
    for (const auto& [id, peer] : peers) {
        approvedPeers_.push_back({peer.clientId, peer.clientName});
        peerTimestamps_.push_back(peer.lastConnected);
    }
}

void ServerGui::showPairRequest(const std::string& clientId, const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        pairRequest_ = {clientId, name, true};
    }
    // Push immediately via dispatch for responsiveness
    if (webview_) {
        // Use JSON-encoded strings with double quotes for safe JS embedding
        webview_->dispatch([this, clientId, name] {
            webview_->eval("showPairRequest(\"" + escapeJson(clientId) +
                          "\",\"" + escapeJson(name) + "\")");
        });
    }
}

void ServerGui::clearPairRequest() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    pairRequest_.pending = false;
}

void ServerGui::addLogMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    time_t now = std::time(nullptr);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    logMessages_.push_back(std::string(timeBuf) + "  " + msg);
    if (logMessages_.size() > MAX_LOG_MESSAGES) {
        logMessages_.erase(logMessages_.begin());
    }
}

void ServerGui::updateDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                              const std::wstring& currentId) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    audioDevices_.clear();
    for (const auto& [id, name] : devices) {
        audioDevices_.push_back({id, wideToUtf8(name)});
    }
    currentDeviceId_ = currentId;
    devicesChanged_ = true;
}

void ServerGui::updateOutDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                               const std::wstring& currentId) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    outAudioDevices_.clear();
    for (const auto& [id, name] : devices) {
        outAudioDevices_.push_back({id, wideToUtf8(name)});
    }
    currentOutDeviceId_ = currentId;
    outDevicesChanged_ = true;
}
