#include "network.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>

Network::Network() {}

Network::~Network() {
    shutdown();
}

bool Network::initialize(const std::string& serverName) {
    serverName_ = serverName;
    macAddress_ = getMacAddress();

    // Load approved peers from disk
    loadApprovedPeers();

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
    printf("Server MAC: %s\n", macAddress_.c_str());
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        printf("Approved peers: %zu\n", approvedPeers_.size());
    }
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
            char response[512];
            snprintf(response, sizeof(response), "AB_OFFER|%s|4012|%s",
                     serverName_.c_str(), macAddress_.c_str());

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
                // Parse: AB_CONNECT|<name>|<clientId>
                std::string payload(buf + 11);
                std::string clientName = payload;
                std::string clientId;

                size_t sep = payload.find('|');
                if (sep != std::string::npos) {
                    clientName = payload.substr(0, sep);
                    clientId = payload.substr(sep + 1);
                }

                // Check if this peer is approved
                if (!clientId.empty() && isPeerApproved(clientId)) {
                    // Known peer — auto-accept, use stored name (not client-provided)
                    std::string storedName = getPeerName(clientId);
                    touchPeer(clientId, clientName);

                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        clientAddr_ = senderAddr;
                    }
                    connected_ = true;
                    paused_ = false;
                    sequence_ = 0;
                    streamStart_ = std::chrono::steady_clock::now();
                    lastClientPacket_ = streamStart_;

                    const char* accept = "AB_ACCEPT";
                    sendto(streamSocket_, accept, (int)strlen(accept), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("Auto-accepted known peer: %s (%s)\n", storedName.c_str(), clientId.c_str());
                    if (onConnect_) onConnect_(storedName);
                } else if (clientId.empty()) {
                    // Legacy client without ID — accept without pairing
                    // (backwards compatible)
                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        clientAddr_ = senderAddr;
                    }
                    connected_ = true;
                    paused_ = false;
                    sequence_ = 0;
                    streamStart_ = std::chrono::steady_clock::now();
                    lastClientPacket_ = streamStart_;

                    const char* accept = "AB_ACCEPT";
                    sendto(streamSocket_, accept, (int)strlen(accept), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("Accepted legacy client (no ID): %s\n", clientName.c_str());
                    if (onConnect_) onConnect_(clientName);
                } else {
                    // Unknown peer — send PAIR_PENDING and wait for approval
                    const char* pending = "AB_PAIR_PENDING";
                    sendto(streamSocket_, pending, (int)strlen(pending), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("\nNew device wants to pair: '%s' (ID: %s)\n",
                           clientName.c_str(), clientId.c_str());

                    // Set up pending state
                    {
                        std::lock_guard<std::mutex> lock(pendingMutex_);
                        pendingPeer_.clientId = clientId;
                        pendingPeer_.clientName = clientName;
                        pendingPeer_.addr = senderAddr;
                        pendingPeer_.addrLen = addrLen;
                        pendingPeer_.responded = false;
                        pendingPeer_.approved = false;
                    }

                    // Notify via callback (e.g. console prompt)
                    if (onPairRequest_) {
                        onPairRequest_(clientId, clientName);
                    }

                    // Wait for approval (up to 30 seconds)
                    {
                        std::unique_lock<std::mutex> lock(pendingMutex_);
                        pendingCv_.wait_for(lock, std::chrono::seconds(30), [this] {
                            return pendingPeer_.responded || !running_;
                        });

                        if (pendingPeer_.responded && pendingPeer_.approved) {
                            // Approved — add to peers and accept
                            touchPeer(clientId, clientName);

                            {
                                std::lock_guard<std::mutex> lock2(clientMutex_);
                                clientAddr_ = senderAddr;
                            }
                            connected_ = true;
                            paused_ = false;
                            sequence_ = 0;
                            streamStart_ = std::chrono::steady_clock::now();
                            lastClientPacket_ = streamStart_;

                            const char* accept = "AB_ACCEPT";
                            sendto(streamSocket_, accept, (int)strlen(accept), 0,
                                   (sockaddr*)&senderAddr, addrLen);

                            if (onConnect_) onConnect_(clientName);
                        } else {
                            // Rejected or timed out
                            const char* reject = pendingPeer_.responded
                                ? "AB_REJECT|Denied by server"
                                : "AB_REJECT|Approval timed out";
                            sendto(streamSocket_, reject, (int)strlen(reject), 0,
                                   (sockaddr*)&senderAddr, addrLen);
                            printf("Pair request %s for %s\n",
                                   pendingPeer_.responded ? "denied" : "timed out",
                                   clientName.c_str());
                        }
                    }
                }
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
                // Verify sender IP matches connected client (port may differ for mic socket)
                bool sameClient;
                {
                    std::lock_guard<std::mutex> lock(clientMutex_);
                    sameClient = (senderAddr.sin_addr.s_addr == clientAddr_.sin_addr.s_addr);
                }
                if (!sameClient) continue;

                lastClientPacket_ = std::chrono::steady_clock::now();

                uint8_t type = (uint8_t)buf[1];
                if (type == PACKET_CONTROL && received >= 17) {
                    uint8_t cmd = (uint8_t)buf[16];
                    printf("[NET] Control command: 0x%02X\n", cmd);
                    if (cmd == CTRL_PAUSE) {
                        printf("[NET] -> PAUSE stream\n");
                        paused_ = true;
                        if (onPause_) onPause_(true);
                    } else if (cmd == CTRL_RESUME) {
                        printf("[NET] -> RESUME stream\n");
                        paused_ = false;
                        if (onPause_) onPause_(false);
                    } else if (cmd == CTRL_DISCONNECT) {
                        connected_ = false;
                        paused_ = false;
                        if (onDisconnect_) onDisconnect_();
                    } else if (cmd >= CTRL_MEDIA_PLAY_PAUSE && cmd <= CTRL_MEDIA_PREV) {
                        printf("[NET] -> Media command 0x%02X, callback=%s\n", cmd, onMediaCommand_ ? "YES" : "NO");
                        if (onMediaCommand_) onMediaCommand_(cmd);
                    } else if (cmd == CTRL_MEDIA_INFO_REQ) {
                        printf("[NET] -> Media info request\n");
                        if (onMediaInfoRequest_) onMediaInfoRequest_();
                    } else {
                        printf("[NET] -> Unknown control command 0x%02X\n", cmd);
                    }
                } else if (type == PACKET_CONTROL && received < 17) {
                    printf("[NET] Control packet too short: %d bytes (need 17)\n", received);
                } else if (type == PACKET_MIC_AUDIO && received >= 16) {
                    uint16_t payloadLen;
                    memcpy(&payloadLen, buf + 14, 2);
                    if (received >= 16 + payloadLen && onMicAudio_) {
                        onMicAudio_((const uint8_t*)(buf + 16), payloadLen);
                    }
                } else if (type == PACKET_KEEPALIVE) {
                    // Just update lastClientPacket_ (already done above)
                } else if (type == PACKET_PING) {
                    // Respond with PONG immediately
                    uint8_t pong[16];
                    writeHeader(pong, PACKET_PONG, 0);
                    // Copy the client's timestamp into pong so they can measure RTT
                    memcpy(pong + 6, buf + 6, 8);
                    std::lock_guard<std::mutex> lock(clientMutex_);
                    sendto(streamSocket_, (char*)pong, 16, 0,
                           (sockaddr*)&clientAddr_, sizeof(clientAddr_));
                } else if (type == PACKET_PONG) {
                    // Client responded to our ping — compute RTT
                    if (pingPending_) {
                        auto now = std::chrono::steady_clock::now();
                        double rtt = std::chrono::duration<double, std::milli>(
                            now - lastPingSent_).count();
                        pingPending_ = false;
                        // EWMA smoothing (alpha = 0.3)
                        double prev = clientRttMs_.load();
                        if (prev == 0.0) {
                            clientRttMs_.store(rtt);
                        } else {
                            clientRttMs_.store(prev * 0.7 + rtt * 0.3);
                        }
                    }
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

void Network::sendPing() {
    if (!connected_) return;

    uint8_t packet[16];
    writeHeader(packet, PACKET_PING, 0);

    lastPingSent_ = std::chrono::steady_clock::now();
    pingPending_ = true;

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendto(streamSocket_, (char*)packet, 16, 0,
           (sockaddr*)&clientAddr_, sizeof(clientAddr_));
}

void Network::sendMediaInfo(const std::string& json) {
    if (!connected_) return;

    uint8_t packet[2048];
    int payloadLen = (int)json.size();
    if (payloadLen + 16 > (int)sizeof(packet)) return;

    writeHeader(packet, PACKET_MEDIA_INFO, (uint16_t)payloadLen);
    memcpy(packet + 16, json.c_str(), payloadLen);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendto(streamSocket_, (char*)packet, 16 + payloadLen, 0,
           (sockaddr*)&clientAddr_, sizeof(clientAddr_));
}

std::string Network::getClientAddress() const {
    if (!connected_) return "";
    char addrStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr_.sin_addr, addrStr, sizeof(addrStr));
    return std::string(addrStr) + ":" + std::to_string(ntohs(clientAddr_.sin_port));
}

std::string Network::getMacAddress() {
    ULONG bufLen = 0;
    GetAdaptersInfo(nullptr, &bufLen);
    if (bufLen == 0) return "00:00:00:00:00:00";

    std::vector<uint8_t> buffer(bufLen);
    PIP_ADAPTER_INFO adapters = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());

    if (GetAdaptersInfo(adapters, &bufLen) != ERROR_SUCCESS) {
        return "00:00:00:00:00:00";
    }

    // Find the first adapter with a valid MAC (skip loopback/virtual)
    for (PIP_ADAPTER_INFO adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->AddressLength == 6 && adapter->Type != MIB_IF_TYPE_LOOPBACK) {
            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     adapter->Address[0], adapter->Address[1],
                     adapter->Address[2], adapter->Address[3],
                     adapter->Address[4], adapter->Address[5]);
            // Skip zero MACs
            if (strcmp(mac, "00:00:00:00:00:00") != 0) {
                return mac;
            }
        }
    }
    return "00:00:00:00:00:00";
}

// --- Peer Persistence ---

std::string Network::getPeersFilePath() const {
    // Store next to the executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        path = path.substr(0, lastSlash + 1);
    }
    return path + "approved_peers.txt";
}

void Network::loadApprovedPeers() {
    std::lock_guard<std::mutex> lock(peersMutex_);
    approvedPeers_.clear();

    std::string filePath = getPeersFilePath();
    std::ifstream file(filePath);
    if (!file.is_open()) return;

    std::string line;
    int64_t now = std::time(nullptr);
    int64_t expirySeconds = PEER_EXPIRY_DAYS * 24LL * 3600LL;
    int loaded = 0, expired = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Format: clientId|clientName|lastConnected(unix timestamp)
        std::istringstream iss(line);
        std::string id, name, tsStr;
        if (!std::getline(iss, id, '|')) continue;
        if (!std::getline(iss, name, '|')) continue;
        if (!std::getline(iss, tsStr, '|')) continue;

        int64_t ts = 0;
        try { ts = std::stoll(tsStr); } catch (...) { continue; }

        // Skip expired peers (30-day sliding window)
        if (now - ts > expirySeconds) {
            expired++;
            continue;
        }

        ApprovedPeer peer;
        peer.clientId = id;
        peer.clientName = name;
        peer.lastConnected = ts;
        approvedPeers_[id] = peer;
        loaded++;
    }

    if (loaded > 0 || expired > 0) {
        printf("Loaded %d approved peer(s), %d expired\n", loaded, expired);
    }

    // If we pruned expired peers, rewrite the file
    if (expired > 0) {
        file.close();
        saveApprovedPeers();
    }
}

void Network::saveApprovedPeers() {
    // Must be called with peersMutex_ held, OR hold it here
    // We'll try-lock to avoid deadlocks; callers that already hold it
    // will call the internal version
    std::string filePath = getPeersFilePath();
    std::ofstream file(filePath, std::ios::trunc);
    if (!file.is_open()) {
        printf("Warning: Could not write peers file: %s\n", filePath.c_str());
        return;
    }

    file << "# AudioBridge approved peers\n";
    file << "# Format: clientId|clientName|lastConnected(unix)\n";
    for (const auto& [id, peer] : approvedPeers_) {
        file << peer.clientId << "|" << peer.clientName << "|" << peer.lastConnected << "\n";
    }
}

bool Network::isPeerApproved(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it == approvedPeers_.end()) return false;

    // Double-check expiry
    int64_t now = std::time(nullptr);
    int64_t expirySeconds = PEER_EXPIRY_DAYS * 24LL * 3600LL;
    return (now - it->second.lastConnected) <= expirySeconds;
}

std::string Network::getPeerName(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it != approvedPeers_.end()) return it->second.clientName;
    return "";
}

void Network::touchPeer(const std::string& clientId, const std::string& clientName) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it != approvedPeers_.end()) {
        // Only update timestamp — name is locked at approval time
        it->second.lastConnected = std::time(nullptr);
    } else {
        ApprovedPeer peer;
        peer.clientId = clientId;
        peer.clientName = clientName;
        peer.lastConnected = std::time(nullptr);
        approvedPeers_[clientId] = peer;
    }
    saveApprovedPeers();
}

void Network::approvePeer(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pendingPeer_.clientId == clientId) {
        pendingPeer_.responded = true;
        pendingPeer_.approved = true;
        pendingCv_.notify_one();
    }
}

void Network::rejectPeer(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pendingPeer_.clientId == clientId) {
        pendingPeer_.responded = true;
        pendingPeer_.approved = false;
        pendingCv_.notify_one();
    }
}

std::map<std::string, ApprovedPeer> Network::getApprovedPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return approvedPeers_;
}

void Network::revokePeer(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    approvedPeers_.erase(clientId);
    saveApprovedPeers();
    printf("Revoked peer: %s\n", clientId.c_str());
}
