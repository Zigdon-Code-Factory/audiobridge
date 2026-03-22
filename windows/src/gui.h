#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <map>
#include <memory>
#include <cstdint>

// Forward declarations
struct ApprovedPeer;

#include "webview/webview.h"

struct ServerStats {
    bool connected = false;
    bool paused = false;
    std::string clientName;
    std::string clientAddress;
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    double kbps = 0.0;
    float peakLevel = 0.0f;
    int jitterBufferMs = 10;
    uint32_t sequenceNum = 0;
    double uptimeSeconds = 0.0;
    std::string serverName;
    std::string serverMac;
    double clientRttMs = 0.0;
};

struct PairRequest {
    std::string clientId;
    std::string clientName;
    bool pending = false;
};

class ServerGui {
public:
    using JitterChangeCallback = std::function<void(int bufferMs)>;
    using PairApproveCallback = std::function<void(const std::string& clientId)>;
    using PairDenyCallback = std::function<void(const std::string& clientId)>;
    using RevokeCallback = std::function<void(const std::string& clientId)>;
    using DeviceChangeCallback = std::function<void(const std::wstring& deviceId)>;
    using TickCallback = std::function<void()>;

    ServerGui();
    ~ServerGui();

    bool initialize(const std::string& title);

    // Run the event loop (blocks until window closed)
    void run();

    // Update data (thread-safe)
    void updateStats(const ServerStats& stats);
    void updateApprovedPeers(const std::map<std::string, ApprovedPeer>& peers);
    void showPairRequest(const std::string& clientId, const std::string& name);
    void clearPairRequest();
    void addLogMessage(const std::string& msg);
    void updateDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                       const std::wstring& currentId);
    void updateOutDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                       const std::wstring& currentId);

    // Set callbacks
    void setJitterChangeCallback(JitterChangeCallback cb) { onJitterChange_ = cb; }
    void setPairApproveCallback(PairApproveCallback cb) { onPairApprove_ = cb; }
    void setPairDenyCallback(PairDenyCallback cb) { onPairDeny_ = cb; }
    void setRevokeCallback(RevokeCallback cb) { onRevoke_ = cb; }
    void setDeviceChangeCallback(DeviceChangeCallback cb) { onDeviceChange_ = cb; }
    void setOutDeviceChangeCallback(DeviceChangeCallback cb) { onOutDeviceChange_ = cb; }
    void setTickCallback(TickCallback cb) { onTick_ = cb; }
    int getJitterBufferMs() const { return jitterBufferMs_; }

private:
    void setupBindings();
    std::string buildStateJson();
    int getCurrentDeviceIndex() const;
    int getCurrentOutDeviceIndex() const;

    static std::string escapeJson(const std::string& s);
    static std::string wideToUtf8(const std::wstring& ws);

    std::unique_ptr<webview::webview> webview_;
    int jitterBufferMs_ = 10;

    // Thread-safe data
    mutable std::mutex dataMutex_;
    ServerStats stats_;
    std::vector<std::pair<std::string, std::string>> approvedPeers_;  // id, name
    std::vector<int64_t> peerTimestamps_;
    PairRequest pairRequest_;
    std::vector<std::string> logMessages_;
    static constexpr int MAX_LOG_MESSAGES = 50;

    // Device lists
    struct DeviceEntry { std::wstring id; std::string name; };
    std::vector<DeviceEntry> audioDevices_;
    std::wstring currentDeviceId_;
    std::vector<DeviceEntry> outAudioDevices_;
    std::wstring currentOutDeviceId_;
    bool devicesChanged_ = true;
    bool outDevicesChanged_ = true;

    // Callbacks
    JitterChangeCallback onJitterChange_;
    PairApproveCallback onPairApprove_;
    PairDenyCallback onPairDeny_;
    RevokeCallback onRevoke_;
    DeviceChangeCallback onDeviceChange_;
    DeviceChangeCallback onOutDeviceChange_;
    TickCallback onTick_;
};
