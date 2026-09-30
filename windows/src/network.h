#pragma once
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>
#include <map>
#include <memory>
#include <condition_variable>
#include "dtls_session.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

struct PacketHeader {
    uint8_t version;    // 0x01
    uint8_t type;       // 0x01=audio, 0x02=keepalive, 0x03=control
    uint32_t sequence;
    uint64_t timestamp; // microseconds since stream start
    uint16_t payloadLen;
};

enum PacketType : uint8_t {
    PACKET_AUDIO     = 0x01,
    PACKET_KEEPALIVE = 0x02,
    PACKET_CONTROL   = 0x03,
    PACKET_MIC_AUDIO = 0x04,
    PACKET_MEDIA_INFO = 0x05,
    PACKET_PING      = 0x06,
    PACKET_PONG      = 0x07,
    PACKET_SETTINGS  = 0x08,
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
    std::string clientId;   // MAC address or UUID
    std::string clientName;
    int64_t lastConnected;  // unix timestamp (seconds)
    std::string pskHex;     // 64-char hex string, empty for legacy peers
};

class Network {
public:
    using ConnectCallback = std::function<void(const std::string& clientName)>;
    using DisconnectCallback = std::function<void()>;
    using PauseCallback = std::function<void(bool paused)>;
    using MicAudioCallback = std::function<void(const uint8_t* opusData, int opusLen)>;
    // Called when an unknown peer wants to pair. Return value ignored;
    // call approvePeer()/rejectPeer() from any thread.
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

    // Call from any thread to approve/reject a pending peer
    void approvePeer(const std::string& clientId);
    void rejectPeer(const std::string& clientId);

    // Peer management
    std::map<std::string, ApprovedPeer> getApprovedPeers() const;
    void revokePeer(const std::string& clientId);
    std::string getPeerName(const std::string& clientId) const;

    bool isConnected() const { return connected_; }
    bool isPaused() const { return paused_; }
    bool isDtlsActive() const { return dtlsActive_; }

    // Send audio packet (16-byte header + opus payload)
    void sendAudio(const uint8_t* opusData, int opusLen);

    // Send keepalive
    void sendKeepalive();

    // Send ping for RTT measurement
    void sendPing();

    // Get smoothed client RTT in milliseconds
    double getClientRtt() const { return clientRttMs_.load(); }

    // Send media info to connected client
    void sendMediaInfo(const std::string& json);

    // Send jitter buffer settings to connected client (ms)
    void sendSettings(int jitterMs, int frameSizeMs = 10);

    std::string getClientAddress() const;
    std::string getServerMac() const { return macAddress_; }

private:
    void discoveryThread();
    void streamThread();
    void writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen);
    void sendPacket(const uint8_t* data, size_t len);
    static std::string getMacAddress();

    // DTLS
    bool startDtlsHandshake(const sockaddr_in& clientAddr, const std::string& clientId);
    void endDtlsSession();
    void endDtlsSessionLocked(); // Must be called with dtlsMutex_ held
    bool pskLookup(const std::string& identity, std::vector<uint8_t>& outPsk);
    std::string getPeerPsk(const std::string& clientId) const;
    void setPeerPsk(const std::string& clientId, const std::string& pskHex);

    // Peer persistence
    void loadApprovedPeers();
    void saveApprovedPeers();
    bool isPeerApproved(const std::string& clientId) const;
    void touchPeer(const std::string& clientId, const std::string& clientName);
    std::string getPeersFilePath() const;

    std::string serverName_;
    std::string macAddress_;
    SOCKET discoverySocket_ = INVALID_SOCKET;
    SOCKET streamSocket_ = INVALID_SOCKET;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> paused_{false};

    std::thread discoveryThread_;
    std::thread streamThread_;

    sockaddr_in clientAddr_{};
    mutable std::mutex clientMutex_;

    uint32_t sequence_ = 0;
    std::chrono::steady_clock::time_point streamStart_;
    std::chrono::steady_clock::time_point lastClientPacket_;

    // RTT measurement
    std::atomic<double> clientRttMs_{0.0};
    std::chrono::steady_clock::time_point lastPingSent_;
    bool pingPending_ = false;

    ConnectCallback onConnect_;
    DisconnectCallback onDisconnect_;
    PauseCallback onPause_;
    MicAudioCallback onMicAudio_;
    PairRequestCallback onPairRequest_;
    MediaCommandCallback onMediaCommand_;
    MediaInfoRequestCallback onMediaInfoRequest_;

    // Approved peers (clientId -> ApprovedPeer)
    std::map<std::string, ApprovedPeer> approvedPeers_;
    mutable std::mutex peersMutex_;

    // Pending pair request state
    struct PendingPeer {
        std::string clientId;
        std::string clientName;
        sockaddr_in addr;
        int addrLen;
        bool responded = false;
        bool approved = false;
    };
    PendingPeer pendingPeer_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;

    // DTLS session (protected by dtlsMutex_)
    std::mutex dtlsMutex_;
    std::unique_ptr<DtlsSession> dtlsSession_;
    bool dtlsActive_ = false;
    std::string connectedClientId_;

    // Per-connection diagnostic counters (reset on each new connection)
    std::atomic<int> dtlsRecvCount_{0};
    std::atomic<int> dtlsDecryptCount_{0};
    std::atomic<int> dtlsSendCount_{0};
    std::atomic<int> dtlsSendErrCount_{0};
};
