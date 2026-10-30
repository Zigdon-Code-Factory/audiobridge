#pragma once
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <qos2.h>
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
#include "secure_server.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "qwave.lib")

struct PacketHeader {
    uint8_t version;    // 0x01
    uint8_t type;       // 0x01=audio(opus), 0x09=audio(pcm), 0x02=keepalive, 0x03=control
    uint32_t sequence;
    uint64_t timestamp; // microseconds since stream start
    uint16_t payloadLen;
};

enum PacketType : uint8_t {
    PACKET_AUDIO      = 0x01,
    PACKET_KEEPALIVE  = 0x02,
    PACKET_CONTROL    = 0x03,
    PACKET_MIC_AUDIO  = 0x04,
    PACKET_MEDIA_INFO = 0x05,
    PACKET_PING       = 0x06,
    PACKET_PONG       = 0x07,
    PACKET_SETTINGS   = 0x08,
    PACKET_AUDIO_PCM  = 0x09,
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
    std::string pskHex;     // v2: plus 64 hex digits; old exposed keys are rejected
};

class Network {
public:
    using ConnectCallback = std::function<void(const std::string& clientName)>;
    using DisconnectCallback = std::function<void()>;
    using PauseCallback = std::function<void(bool paused)>;
    // A null/zero packet resets decoder state when the return-mic owner changes.
    using MicAudioCallback = std::function<void(const uint8_t* opusData, int opusLen)>;
    // Called when an unknown peer wants to pair. Return value ignored;
    // call approvePeer()/rejectPeer() from any thread.
    // First argument is an opaque approval token bound to this key exchange;
    // pass it unchanged to approvePeer/rejectPeer. Empty token dismisses the prompt.
    using PairRequestCallback = std::function<void(const std::string& clientId, const std::string& clientName, const std::string& code)>;
    using MediaCommandCallback = std::function<void(uint8_t cmd)>;
    using MediaInfoRequestCallback = std::function<void()>;

    static constexpr int PEER_EXPIRY_DAYS = 30;

    Network();
    ~Network();

    bool initialize(const std::string& serverName, uint16_t discoveryPort = 4011, uint16_t streamPort = 4012);
    uint16_t getStreamPort() const { return streamPort_; }
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

    bool isConnected() const { return getClientCount() > 0; }
    bool isPaused() const { return sessions_ && sessions_->allPaused(); }
    bool isDtlsActive() const { return isConnected(); }
    bool isVoiceQosActive() const { return qosFlowCount_.load(std::memory_order_relaxed) != 0; }

    // Send Opus audio packet (16-byte header + opus payload)
    void sendAudio(const uint8_t* opusData, int opusLen);

    // Send zero-lookahead Raw 16-bit Stereo PCM audio packet (16-byte header + interleaved int16 stereo)
    void sendAudioPcm(const int16_t* pcmStereo, int frameCount);

    // Send keepalive
    void sendKeepalive();

    // Send ping for RTT measurement
    void sendPing();

    // Get smoothed client RTT in milliseconds
    double getClientRtt() const;

    // Send media info to connected client
    void sendMediaInfo(const std::string& json);

    // Send jitter buffer, frame size, exact sample quantum, and codec mode to connected client
    void sendSettings(int jitterMs, int frameSizeMs = 5, uint16_t frameSamples = 240, bool rawPcm = true);

    size_t getClientCount() const { return sessions_ ? sessions_->count() : 0; }
    std::string getClientNames() const;
    std::string getMicOwnerName() const { return sessions_ ? sessions_->micOwner() : ""; }
    std::string getClientAddress() const;
    std::string getServerMac() const { return macAddress_; }

private:
    void discoveryThread();
    void streamThread();
    static std::string getMacAddress();
    std::string getPeerPsk(const std::string& clientId) const;
    void setPeerPsk(const std::string& clientId, const std::string& pskHex);
    void loadApprovedPeers();
    void saveApprovedPeers(); // Caller holds peersMutex_.
    bool isPeerApproved(const std::string& clientId) const;
    void touchPeer(const std::string& clientId, const std::string& clientName);
    std::string getPeersFilePath() const;
    uint16_t streamPort_ = 4012;
    std::string serverName_, macAddress_;
#ifdef _WIN32
    SOCKET discoverySocket_ = INVALID_SOCKET, streamSocket_ = INVALID_SOCKET;
    HANDLE qosHandle_ = nullptr;
    std::atomic<size_t> qosFlowCount_{0};
    std::map<std::string, QOS_FLOWID> qosFlows_;
    std::mutex qosMutex_;
    void refreshVoiceQos();
    void detachVoiceQosFlow();
#else
    int discoverySocket_ = -1, streamSocket_ = -1;
#endif
    std::atomic<bool> running_{false};
    std::thread discoveryThread_, streamThread_;
    std::unique_ptr<SecureServer> sessions_;
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
