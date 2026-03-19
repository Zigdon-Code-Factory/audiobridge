#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>
#include <map>
#include <condition_variable>

struct PacketHeader {
    uint8_t version;
    uint8_t type;
    uint32_t sequence;
    uint64_t timestamp;
    uint16_t payloadLen;
};

enum PacketType : uint8_t {
    PACKET_AUDIO     = 0x01,
    PACKET_KEEPALIVE = 0x02,
    PACKET_CONTROL   = 0x03,
    PACKET_MIC_AUDIO = 0x04,
    PACKET_MEDIA_INFO = 0x05,
};

enum ControlCmd : uint8_t {
    CTRL_PAUSE      = 0x01,
    CTRL_RESUME     = 0x02,
    CTRL_DISCONNECT = 0x03,
    CTRL_MEDIA_PLAY_PAUSE = 0x10,
    CTRL_MEDIA_NEXT       = 0x11,
    CTRL_MEDIA_PREV       = 0x12,
    CTRL_MEDIA_INFO_REQ   = 0x13,
};

struct ApprovedPeer {
    std::string clientId;
    std::string clientName;
    int64_t lastConnected;
};

class Network {
public:
    using ConnectCallback = std::function<void(const std::string& clientName)>;
    using DisconnectCallback = std::function<void()>;
    using PauseCallback = std::function<void(bool paused)>;
    using MicAudioCallback = std::function<void(const uint8_t* opusData, int opusLen)>;
    using PairRequestCallback = std::function<void(const std::string& clientId, const std::string& clientName)>;
    using MediaCommandCallback = std::function<void(uint8_t cmd)>;
    using MediaInfoRequestCallback = std::function<void()>;

    static constexpr int PEER_EXPIRY_DAYS = 30;

    Network();
    ~Network();

    bool initialize(const std::string& serverName);
    void shutdown();

    void setCallbacks(ConnectCallback onConnect, DisconnectCallback onDisconnect,
                      PauseCallback onPause, MicAudioCallback onMicAudio,
                      PairRequestCallback onPairRequest = nullptr,
                      MediaCommandCallback onMediaCommand = nullptr,
                      MediaInfoRequestCallback onMediaInfoRequest = nullptr) {
        onConnect_ = onConnect;
        onDisconnect_ = onDisconnect;
        onPause_ = onPause;
        onMicAudio_ = onMicAudio;
        onPairRequest_ = onPairRequest;
        onMediaCommand_ = onMediaCommand;
        onMediaInfoRequest_ = onMediaInfoRequest;
    }

    void approvePeer(const std::string& clientId);
    void rejectPeer(const std::string& clientId);

    std::map<std::string, ApprovedPeer> getApprovedPeers() const;
    void revokePeer(const std::string& clientId);
    std::string getPeerName(const std::string& clientId) const;

    bool isConnected() const { return connected_; }
    bool isPaused() const { return paused_; }

    void sendAudio(const uint8_t* opusData, int opusLen);
    void sendKeepalive();
    void sendMediaInfo(const std::string& json);

    std::string getClientAddress() const;
    std::string getServerMac() const { return macAddress_; }

private:
    void discoveryThread();
    void streamThread();
    void writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen);
    static std::string getMacAddress();

    void loadApprovedPeers();
    void saveApprovedPeers();
    bool isPeerApproved(const std::string& clientId) const;
    void touchPeer(const std::string& clientId, const std::string& clientName);
    std::string getPeersFilePath() const;

    std::string serverName_;
    std::string macAddress_;

    int discoverySocket_ = -1;
    int streamSocket_ = -1;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> paused_{false};
    uint32_t sequence_ = 0;
    std::chrono::steady_clock::time_point streamStart_;
    std::chrono::steady_clock::time_point lastClientPacket_;

    std::thread discoveryThread_;
    std::thread streamThread_;

    mutable std::mutex clientMutex_;
    sockaddr_in clientAddr_{};

    struct PendingPeer {
        std::string clientId;
        std::string clientName;
        sockaddr_in addr{};
        socklen_t addrLen = 0;
        bool responded = false;
        bool approved = false;
    };
    PendingPeer pendingPeer_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;

    ConnectCallback onConnect_;
    DisconnectCallback onDisconnect_;
    PauseCallback onPause_;
    MicAudioCallback onMicAudio_;
    PairRequestCallback onPairRequest_;
    MediaCommandCallback onMediaCommand_;
    MediaInfoRequestCallback onMediaInfoRequest_;

    std::map<std::string, ApprovedPeer> approvedPeers_;
    mutable std::mutex peersMutex_;
};
