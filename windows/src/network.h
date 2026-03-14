#pragma once
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

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
};

enum ControlCmd : uint8_t {
    CTRL_PAUSE      = 0x01,
    CTRL_RESUME     = 0x02,
    CTRL_DISCONNECT = 0x03,
};

class Network {
public:
    using ConnectCallback = std::function<void(const std::string& clientName)>;
    using DisconnectCallback = std::function<void()>;
    using PauseCallback = std::function<void(bool paused)>;

    Network();
    ~Network();

    bool initialize(const std::string& serverName);
    void shutdown();

    void setCallbacks(ConnectCallback onConnect, DisconnectCallback onDisconnect, PauseCallback onPause) {
        onConnect_ = onConnect;
        onDisconnect_ = onDisconnect;
        onPause_ = onPause;
    }

    bool isConnected() const { return connected_; }
    bool isPaused() const { return paused_; }

    // Send audio packet (16-byte header + opus payload)
    void sendAudio(const uint8_t* opusData, int opusLen);

    // Send keepalive
    void sendKeepalive();

    std::string getClientAddress() const;

private:
    void discoveryThread();
    void streamThread();
    void writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen);

    std::string serverName_;
    SOCKET discoverySocket_ = INVALID_SOCKET;
    SOCKET streamSocket_ = INVALID_SOCKET;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> paused_{false};

    std::thread discoveryThread_;
    std::thread streamThread_;

    sockaddr_in clientAddr_{};
    std::mutex clientMutex_;

    uint32_t sequence_ = 0;
    std::chrono::steady_clock::time_point streamStart_;
    std::chrono::steady_clock::time_point lastClientPacket_;

    ConnectCallback onConnect_;
    DisconnectCallback onDisconnect_;
    PauseCallback onPause_;
};
