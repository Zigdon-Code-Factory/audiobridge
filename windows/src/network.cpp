#include "network.h"
#include <cstdio>
#include <cstring>

Network::Network() {}

Network::~Network() {
    shutdown();
}

bool Network::initialize(const std::string& serverName) {
    serverName_ = serverName;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return false;
    }

    // Discovery socket (port 4011)
    discoverySocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discoverySocket_ == INVALID_SOCKET) {
        printf("Failed to create discovery socket\n");
        return false;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));
    BOOL broadcast = TRUE;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(4011);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discoverySocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("Failed to bind discovery socket on port 4011: %d\n", WSAGetLastError());
        return false;
    }

    // Stream socket (port 4012)
    streamSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (streamSocket_ == INVALID_SOCKET) {
        printf("Failed to create stream socket\n");
        return false;
    }

    setsockopt(streamSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

    bindAddr.sin_port = htons(4012);
    if (bind(streamSocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("Failed to bind stream socket on port 4012: %d\n", WSAGetLastError());
        return false;
    }

    // Set non-blocking receive timeout on stream socket for keepalive checks
    DWORD timeout = 100; // 100ms
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    running_ = true;
    discoveryThread_ = std::thread(&Network::discoveryThread, this);
    streamThread_ = std::thread(&Network::streamThread, this);

    printf("Listening: discovery on :4011, stream on :4012\n");
    return true;
}

void Network::shutdown() {
    running_ = false;
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (streamThread_.joinable()) streamThread_.join();
    if (discoverySocket_ != INVALID_SOCKET) { closesocket(discoverySocket_); discoverySocket_ = INVALID_SOCKET; }
    if (streamSocket_ != INVALID_SOCKET) { closesocket(streamSocket_); streamSocket_ = INVALID_SOCKET; }
    WSACleanup();
}

void Network::discoveryThread() {
    char buf[256];
    while (running_) {
        sockaddr_in senderAddr{};
        int addrLen = sizeof(senderAddr);

        // Set timeout so we can check running_ periodically
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(discoverySocket_, &readSet);
        timeval tv{0, 200000}; // 200ms

        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        int received = recvfrom(discoverySocket_, buf, sizeof(buf) - 1, 0,
                                 (sockaddr*)&senderAddr, &addrLen);
        if (received <= 0) continue;

        buf[received] = '\0';
        if (strcmp(buf, "AB_DISCOVER") == 0) {
            char response[256];
            snprintf(response, sizeof(response), "AB_OFFER|%s|4012", serverName_.c_str());

            sendto(discoverySocket_, response, (int)strlen(response), 0,
                   (sockaddr*)&senderAddr, addrLen);

            char addrStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, addrStr, sizeof(addrStr));
        }
    }
}

void Network::streamThread() {
    char buf[2048];
    while (running_) {
        sockaddr_in senderAddr{};
        int addrLen = sizeof(senderAddr);

        int received = recvfrom(streamSocket_, buf, sizeof(buf) - 1, 0,
                                 (sockaddr*)&senderAddr, &addrLen);

        if (received > 0) {
            buf[received] = '\0';

            // Check for text control messages
            if (strncmp(buf, "AB_CONNECT|", 11) == 0) {
                std::string clientName(buf + 11);
                {
                    std::lock_guard<std::mutex> lock(clientMutex_);
                    clientAddr_ = senderAddr;
                }
                connected_ = true;
                paused_ = false;
                sequence_ = 0;
                streamStart_ = std::chrono::steady_clock::now();
                lastClientPacket_ = streamStart_;

                // Send AB_ACCEPT
                const char* accept = "AB_ACCEPT";
                sendto(streamSocket_, accept, (int)strlen(accept), 0,
                       (sockaddr*)&senderAddr, addrLen);

                if (onConnect_) onConnect_(clientName);
                continue;
            }

            if (strcmp(buf, "AB_DISCONNECT") == 0) {
                if (connected_) {
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
                continue;
            }

            // Binary packet from client
            if (connected_ && received >= 16) {
                lastClientPacket_ = std::chrono::steady_clock::now();

                uint8_t type = (uint8_t)buf[1];
                if (type == PACKET_CONTROL && received >= 17) {
                    uint8_t cmd = (uint8_t)buf[16];
                    if (cmd == CTRL_PAUSE) {
                        paused_ = true;
                        if (onPause_) onPause_(true);
                    } else if (cmd == CTRL_RESUME) {
                        paused_ = false;
                        if (onPause_) onPause_(false);
                    } else if (cmd == CTRL_DISCONNECT) {
                        connected_ = false;
                        paused_ = false;
                        if (onDisconnect_) onDisconnect_();
                    }
                } else if (type == PACKET_KEEPALIVE) {
                    // Just update lastClientPacket_ (already done above)
                }
            }
        }

        // Check for client timeout (5 seconds)
        if (connected_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastClientPacket_).count();
            if (elapsed > 5000) {
                connected_ = false;
                paused_ = false;
                printf("Client timed out\n");
                if (onDisconnect_) onDisconnect_();
            }
        }
    }
}

void Network::writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen) {
    auto now = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now - streamStart_).count();

    buf[0] = 0x01; // version
    buf[1] = type;
    memcpy(buf + 2, &sequence_, 4);
    memcpy(buf + 6, &timestamp, 8);
    memcpy(buf + 14, &payloadLen, 2);
    sequence_++;
}

void Network::sendAudio(const uint8_t* opusData, int opusLen) {
    if (!connected_ || paused_) return;

    uint8_t packet[2048];
    if (opusLen + 16 > (int)sizeof(packet)) return;

    writeHeader(packet, PACKET_AUDIO, (uint16_t)opusLen);
    memcpy(packet + 16, opusData, opusLen);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendto(streamSocket_, (char*)packet, 16 + opusLen, 0,
           (sockaddr*)&clientAddr_, sizeof(clientAddr_));
}

void Network::sendKeepalive() {
    if (!connected_) return;

    uint8_t packet[16];
    writeHeader(packet, PACKET_KEEPALIVE, 0);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendto(streamSocket_, (char*)packet, 16, 0,
           (sockaddr*)&clientAddr_, sizeof(clientAddr_));
}

std::string Network::getClientAddress() const {
    if (!connected_) return "";
    char addrStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr_.sin_addr, addrStr, sizeof(addrStr));
    return std::string(addrStr) + ":" + std::to_string(ntohs(clientAddr_.sin_port));
}
