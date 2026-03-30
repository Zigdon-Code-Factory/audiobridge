#include "network.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <sys/ioctl.h>
#include <linux/if.h>

Network::Network() {}

Network::~Network() {
    shutdown();
}

bool Network::initialize(const std::string& serverName) {
    serverName_ = serverName;
    macAddress_ = getMacAddress();
    loadApprovedPeers();

    // Discovery socket (port 4011)
    discoverySocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discoverySocket_ < 0) {
        printf("Failed to create discovery socket\n");
        return false;
    }

    int reuseAddr = 1;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
    int broadcast = 1;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(4011);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discoverySocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        printf("Failed to bind discovery socket on port 4011\n");
        return false;
    }

    // Stream socket (port 4012)
    streamSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (streamSocket_ < 0) {
        printf("Failed to create stream socket\n");
        return false;
    }

    setsockopt(streamSocket_, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));

    bindAddr.sin_port = htons(4012);
    if (bind(streamSocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        printf("Failed to bind stream socket on port 4012\n");
        return false;
    }

    // Set receive timeout
    timeval timeout{0, 100000}; // 100ms
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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
    endDtlsSession();
    if (discoverySocket_ >= 0) { close(discoverySocket_); discoverySocket_ = -1; }
    if (streamSocket_ >= 0) { close(streamSocket_); streamSocket_ = -1; }
}

void Network::discoveryThread() {
    char buf[256];
    while (running_) {
        sockaddr_in senderAddr{};
        socklen_t addrLen = sizeof(senderAddr);

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(discoverySocket_, &readSet);
        timeval tv{0, 200000};

        int sel = select(discoverySocket_ + 1, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        ssize_t received = recvfrom(discoverySocket_, buf, sizeof(buf) - 1, 0,
                                     (sockaddr*)&senderAddr, &addrLen);
        if (received <= 0) continue;

        buf[received] = '\0';
        if (strcmp(buf, "AB_DISCOVER") == 0) {
            char response[512];
            snprintf(response, sizeof(response), "AB_OFFER|%s|4012|%s",
                     serverName_.c_str(), macAddress_.c_str());

            sendto(discoverySocket_, response, strlen(response), 0,
                   (sockaddr*)&senderAddr, addrLen);
        }
    }
}

void Network::streamThread() {
    char buf[2048];
    while (running_) {
        sockaddr_in senderAddr{};
        socklen_t addrLen = sizeof(senderAddr);

        ssize_t received = recvfrom(streamSocket_, buf, sizeof(buf) - 1, 0,
                                     (sockaddr*)&senderAddr, &addrLen);

        if (received > 0) {
            uint8_t firstByte = (uint8_t)buf[0];

            // DTLS record (content types 20-25) — feed to DTLS engine
            if (dtlsActive_ && dtlsSession_ && firstByte >= 20 && firstByte <= 25) {
                dtlsSession_->pushReceivedData((const uint8_t*)buf, received);
                uint8_t plainBuf[2048];
                int n = dtlsSession_->recv(plainBuf, sizeof(plainBuf));
                if (n > 0) {
                    lastClientPacket_ = std::chrono::steady_clock::now();
                    // Process decrypted application data (same as binary packet handling)
                    if (n >= 16) {
                        uint8_t type = plainBuf[1];
                        if (type == PACKET_CONTROL && n >= 17) {
                            uint8_t cmd = plainBuf[16];
                            printf("[NET] Control command: 0x%02X\n", cmd);
                            if (cmd == CTRL_PAUSE) {
                                paused_ = true;
                                if (onPause_) onPause_(true);
                            } else if (cmd == CTRL_RESUME) {
                                paused_ = false;
                                if (onPause_) onPause_(false);
                            } else if (cmd == CTRL_DISCONNECT) {
                                endDtlsSession();
                                connected_ = false;
                                paused_ = false;
                                if (onDisconnect_) onDisconnect_();
                            } else if (cmd >= CTRL_MEDIA_PLAY_PAUSE && cmd <= CTRL_MEDIA_PREV) {
                                if (onMediaCommand_) onMediaCommand_(cmd);
                            } else if (cmd == CTRL_MEDIA_INFO_REQ) {
                                if (onMediaInfoRequest_) onMediaInfoRequest_();
                            }
                        } else if (type == PACKET_MIC_AUDIO) {
                            uint16_t payloadLen;
                            memcpy(&payloadLen, plainBuf + 14, 2);
                            if (n >= 16 + payloadLen && onMicAudio_) {
                                onMicAudio_(plainBuf + 16, payloadLen);
                            }
                        }
                        // PACKET_KEEPALIVE: lastClientPacket_ already updated above
                    }
                } else if (n < 0) {
                    // DTLS session broken
                    printf("[NET] DTLS recv error, disconnecting\n");
                    endDtlsSession();
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
                goto check_timeout;
            }

            // During DTLS handshake (not yet established), feed DTLS records
            if (dtlsSession_ && !dtlsActive_ && firstByte >= 20 && firstByte <= 25) {
                dtlsSession_->pushReceivedData((const uint8_t*)buf, received);
                // Handshake is driven by startDtlsHandshake(), not here
                goto check_timeout;
            }

            buf[received] = '\0';

            if (strncmp(buf, "AB_CONNECT|", 11) == 0) {
                // Disconnect existing client if any
                if (connected_) {
                    endDtlsSession();
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }

                std::string payload(buf + 11);
                std::string clientName = payload;
                std::string clientId;

                size_t sep = payload.find('|');
                if (sep != std::string::npos) {
                    clientName = payload.substr(0, sep);
                    clientId = payload.substr(sep + 1);
                }

                if (!clientId.empty() && isPeerApproved(clientId)) {
                    // Known peer — auto-accept
                    std::string storedName = getPeerName(clientId);
                    std::string pskHex = getPeerPsk(clientId);
                    touchPeer(clientId, clientName);

                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        clientAddr_ = senderAddr;
                    }

                    if (pskHex.empty()) {
                        pskHex = DtlsSession::generatePsk();
                        setPeerPsk(clientId, pskHex);
                    }
                    // Always send PSK so client is guaranteed to have it
                    std::string acceptMsg = "AB_ACCEPT|" + DtlsSession::pskToBase64(pskHex);
                    sendto(streamSocket_, acceptMsg.c_str(), acceptMsg.size(), 0,
                           (sockaddr*)&senderAddr, addrLen);
                    printf("Accepted peer with PSK: %s (%s)\n", storedName.c_str(), clientId.c_str());

                    // Perform DTLS handshake
                    if (startDtlsHandshake(senderAddr, clientId)) {
                        connected_ = true;
                        paused_ = false;
                        sequence_ = 0;
                        streamStart_ = std::chrono::steady_clock::now();
                        lastClientPacket_ = streamStart_;
                        if (onConnect_) onConnect_(storedName);
                    } else {
                        printf("DTLS handshake failed with %s\n", storedName.c_str());
                    }
                } else if (clientId.empty()) {
                    // Legacy client without ID — no DTLS
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
                    sendto(streamSocket_, accept, strlen(accept), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("Accepted legacy client (no ID): %s\n", clientName.c_str());
                    if (onConnect_) onConnect_(clientName);
                } else {
                    // Unknown peer — require approval
                    const char* pending = "AB_PAIR_PENDING";
                    sendto(streamSocket_, pending, strlen(pending), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("\nNew device wants to pair: '%s' (ID: %s)\n",
                           clientName.c_str(), clientId.c_str());

                    {
                        std::lock_guard<std::mutex> lock(pendingMutex_);
                        pendingPeer_.clientId = clientId;
                        pendingPeer_.clientName = clientName;
                        pendingPeer_.addr = senderAddr;
                        pendingPeer_.addrLen = addrLen;
                        pendingPeer_.responded = false;
                        pendingPeer_.approved = false;
                    }

                    if (onPairRequest_) {
                        onPairRequest_(clientId, clientName);
                    }

                    {
                        std::unique_lock<std::mutex> lock(pendingMutex_);
                        pendingCv_.wait_for(lock, std::chrono::seconds(30), [this] {
                            return pendingPeer_.responded || !running_;
                        });

                        if (pendingPeer_.responded && pendingPeer_.approved) {
                            // Generate PSK for new peer
                            std::string pskHex = DtlsSession::generatePsk();
                            touchPeer(clientId, clientName);
                            setPeerPsk(clientId, pskHex);

                            {
                                std::lock_guard<std::mutex> lock2(clientMutex_);
                                clientAddr_ = senderAddr;
                            }

                            // Send accept with PSK
                            std::string acceptMsg = "AB_ACCEPT|" + DtlsSession::pskToBase64(pskHex);
                            sendto(streamSocket_, acceptMsg.c_str(), acceptMsg.size(), 0,
                                   (sockaddr*)&senderAddr, addrLen);

                            // Perform DTLS handshake
                            if (startDtlsHandshake(senderAddr, clientId)) {
                                connected_ = true;
                                paused_ = false;
                                sequence_ = 0;
                                streamStart_ = std::chrono::steady_clock::now();
                                lastClientPacket_ = streamStart_;
                                if (onConnect_) onConnect_(clientName);
                            } else {
                                printf("DTLS handshake failed with new peer %s\n", clientName.c_str());
                            }
                        } else {
                            const char* reject = pendingPeer_.responded
                                ? "AB_REJECT|Denied by server"
                                : "AB_REJECT|Approval timed out";
                            sendto(streamSocket_, reject, strlen(reject), 0,
                                   (sockaddr*)&senderAddr, addrLen);
                        }
                    }
                }
                continue;
            }

            if (strcmp(buf, "AB_DISCONNECT") == 0) {
                if (connected_) {
                    endDtlsSession();
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
                continue;
            }

            // Unencrypted binary packet from client (legacy mode, no DTLS)
            if (connected_ && !dtlsActive_ && received >= 16) {
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
                        paused_ = true;
                        if (onPause_) onPause_(true);
                    } else if (cmd == CTRL_RESUME) {
                        paused_ = false;
                        if (onPause_) onPause_(false);
                    } else if (cmd == CTRL_DISCONNECT) {
                        connected_ = false;
                        paused_ = false;
                        if (onDisconnect_) onDisconnect_();
                    } else if (cmd >= CTRL_MEDIA_PLAY_PAUSE && cmd <= CTRL_MEDIA_PREV) {
                        if (onMediaCommand_) onMediaCommand_(cmd);
                    } else if (cmd == CTRL_MEDIA_INFO_REQ) {
                        if (onMediaInfoRequest_) onMediaInfoRequest_();
                    }
                } else if (type == PACKET_MIC_AUDIO && received >= 16) {
                    uint16_t payloadLen;
                    memcpy(&payloadLen, buf + 14, 2);
                    if (received >= 16 + payloadLen && onMicAudio_) {
                        onMicAudio_((const uint8_t*)(buf + 16), payloadLen);
                    }
                }
            }
        }

check_timeout:
        // Client timeout
        if (connected_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastClientPacket_).count();
            if (elapsed > 5000) {
                endDtlsSession();
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

    buf[0] = 0x01;
    buf[1] = type;
    memcpy(buf + 2, &sequence_, 4);
    memcpy(buf + 6, &timestamp, 8);
    memcpy(buf + 14, &payloadLen, 2);
    sequence_++;
}

void Network::sendPacket(const uint8_t* data, size_t len) {
    // Must be called with clientMutex_ held
    if (dtlsActive_ && dtlsSession_) {
        dtlsSession_->send(data, len);
    } else {
        sendto(streamSocket_, (const char*)data, len, 0,
               (sockaddr*)&clientAddr_, sizeof(clientAddr_));
    }
}

void Network::sendAudio(const uint8_t* opusData, int opusLen) {
    if (!connected_ || paused_) return;

    uint8_t packet[2048];
    if (opusLen + 16 > (int)sizeof(packet)) return;

    writeHeader(packet, PACKET_AUDIO, (uint16_t)opusLen);
    memcpy(packet + 16, opusData, opusLen);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendPacket(packet, 16 + opusLen);
}

void Network::sendKeepalive() {
    if (!connected_) return;

    uint8_t packet[16];
    writeHeader(packet, PACKET_KEEPALIVE, 0);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendPacket(packet, 16);
}

void Network::sendMediaInfo(const std::string& json) {
    if (!connected_) return;

    uint8_t packet[2048];
    int payloadLen = (int)json.size();
    if (payloadLen + 16 > (int)sizeof(packet)) return;

    writeHeader(packet, PACKET_MEDIA_INFO, (uint16_t)payloadLen);
    memcpy(packet + 16, json.c_str(), payloadLen);

    std::lock_guard<std::mutex> lock(clientMutex_);
    sendPacket(packet, 16 + payloadLen);
}

std::string Network::getClientAddress() const {
    if (!connected_) return "";
    char addrStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr_.sin_addr, addrStr, sizeof(addrStr));
    return std::string(addrStr) + ":" + std::to_string(ntohs(clientAddr_.sin_port));
}

std::string Network::getMacAddress() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return "00:00:00:00:00:00";

    // Find first non-loopback interface
    std::string ifname;
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        ifname = ifa->ifa_name;
        break;
    }
    freeifaddrs(ifaddr);

    if (ifname.empty()) return "00:00:00:00:00:00";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "00:00:00:00:00:00";

    struct ifreq ifr{};
    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return "00:00:00:00:00:00";
    }
    close(sock);

    unsigned char* mac = (unsigned char*)ifr.ifr_hwaddr.sa_data;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return macStr;
}

// --- Peer Persistence ---

std::string Network::getPeersFilePath() const {
    // Store next to the executable
    char exePath[4096];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return "approved_peers.txt";
    exePath[len] = '\0';
    std::string path(exePath);
    size_t lastSlash = path.find_last_of('/');
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

        std::istringstream iss(line);
        std::string id, name, tsStr, pskHex;
        if (!std::getline(iss, id, '|')) continue;
        if (!std::getline(iss, name, '|')) continue;
        if (!std::getline(iss, tsStr, '|')) continue;
        std::getline(iss, pskHex, '|'); // Optional 4th field

        int64_t ts = 0;
        try { ts = std::stoll(tsStr); } catch (...) { continue; }

        if (now - ts > expirySeconds) { expired++; continue; }

        ApprovedPeer peer;
        peer.clientId = id;
        peer.clientName = name;
        peer.lastConnected = ts;
        peer.pskHex = pskHex;
        approvedPeers_[id] = peer;
        loaded++;
    }

    if (loaded > 0 || expired > 0) {
        printf("Loaded %d approved peer(s), %d expired\n", loaded, expired);
    }

    if (expired > 0) {
        file.close();
        saveApprovedPeers();
    }
}

void Network::saveApprovedPeers() {
    std::string filePath = getPeersFilePath();
    std::ofstream file(filePath, std::ios::trunc);
    if (!file.is_open()) return;

    file << "# AudioBridge approved peers\n";
    file << "# Format: clientId|clientName|lastConnected(unix)|psk_hex\n";
    for (const auto& [id, peer] : approvedPeers_) {
        file << peer.clientId << "|" << peer.clientName << "|" << peer.lastConnected
             << "|" << peer.pskHex << "\n";
    }
}

bool Network::isPeerApproved(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it == approvedPeers_.end()) return false;
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

// --- DTLS ---

bool Network::startDtlsHandshake(const sockaddr_in& clientAddr, const std::string& clientId) {
    endDtlsSession(); // Clean up any existing session

    dtlsSession_ = std::make_unique<DtlsSession>();
    connectedClientId_ = clientId;

    auto lookupFn = [this](const std::string& identity, std::vector<uint8_t>& outPsk) -> bool {
        return pskLookup(identity, outPsk);
    };

    if (!dtlsSession_->init(streamSocket_, clientAddr, lookupFn)) {
        printf("[NET] DTLS init failed\n");
        dtlsSession_.reset();
        connectedClientId_.clear();
        return false;
    }

    printf("[NET] Waiting for DTLS handshake...\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!dtlsSession_->isEstablished() && running_) {
        if (std::chrono::steady_clock::now() > deadline) {
            printf("[NET] DTLS handshake timed out\n");
            dtlsSession_->teardown();
            dtlsSession_.reset();
            connectedClientId_.clear();
            return false;
        }

        // Read a datagram from the socket
        char buf[2048];
        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t n = recvfrom(streamSocket_, buf, sizeof(buf), 0, (sockaddr*)&sender, &slen);
        if (n <= 0) continue;

        uint8_t firstByte = (uint8_t)buf[0];
        if (firstByte >= 20 && firstByte <= 25) {
            // DTLS record — feed to session
            dtlsSession_->pushReceivedData((const uint8_t*)buf, n);
            int ret = dtlsSession_->continueHandshake();
            if (ret == 0) {
                // Handshake complete
                dtlsActive_ = true;
                printf("[NET] DTLS handshake complete\n");
                return true;
            }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                printf("[NET] DTLS handshake error: -0x%04X\n", -ret);
                dtlsSession_->teardown();
                dtlsSession_.reset();
                connectedClientId_.clear();
                return false;
            }
        }
        // Ignore non-DTLS packets during handshake
    }

    return false;
}

void Network::endDtlsSession() {
    if (dtlsSession_) {
        dtlsSession_->teardown();
        dtlsSession_.reset();
    }
    dtlsActive_ = false;
    connectedClientId_.clear();
}

bool Network::pskLookup(const std::string& identity, std::vector<uint8_t>& outPsk) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(identity);
    if (it == approvedPeers_.end() || it->second.pskHex.empty()) {
        return false;
    }
    outPsk = DtlsSession::hexToBytes(it->second.pskHex);
    return !outPsk.empty();
}

std::string Network::getPeerPsk(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it != approvedPeers_.end()) return it->second.pskHex;
    return "";
}

void Network::setPeerPsk(const std::string& clientId, const std::string& pskHex) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = approvedPeers_.find(clientId);
    if (it != approvedPeers_.end()) {
        it->second.pskHex = pskHex;
        saveApprovedPeers();
    }
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
