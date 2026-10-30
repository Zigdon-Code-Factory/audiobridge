#include "network.h"
#include "secure_pairing.h"
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

bool Network::initialize(const std::string& serverName, uint16_t discoveryPort, uint16_t streamPort) {
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
    bindAddr.sin_port = htons(discoveryPort);
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

    bindAddr.sin_port = htons(streamPort);
    if (bind(streamSocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        printf("Failed to bind stream socket on port 4012\n");
        return false;
    }

    socklen_t boundLen = sizeof(bindAddr);
    if (getsockname(streamSocket_, (sockaddr*)&bindAddr, &boundLen) != 0) return false;
    streamPort_ = ntohs(bindAddr.sin_port);

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
    pendingCv_.notify_all();
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
            snprintf(response, sizeof(response), "AB_OFFER|%s|%u|%s",
                     serverName_.c_str(), unsigned(streamPort_), macAddress_.c_str());

            sendto(discoverySocket_, response, strlen(response), 0,
                   (sockaddr*)&senderAddr, addrLen);
        }
    }
}

void Network::streamThread() {
    char buf[4096];
    while (running_) {
        sockaddr_in senderAddr{};
        socklen_t addrLen = sizeof(senderAddr);
        int received = recvfrom(streamSocket_, buf, sizeof(buf) - 1, 0,
                                (sockaddr*)&senderAddr, &addrLen);
        if (received > 0) {
            if (static_cast<uint8_t>(buf[0]) >= 20 && static_cast<uint8_t>(buf[0]) <= 25) {
                uint8_t plainBuf[4096];
                int n = 0;
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    if (connected_ && dtlsSession_ && dtlsActive_ &&
                        senderAddr.sin_addr.s_addr == clientAddr_.sin_addr.s_addr &&
                        senderAddr.sin_port == clientAddr_.sin_port) {
                        dtlsSession_->pushReceivedData(reinterpret_cast<const uint8_t*>(buf), received);
                        n = dtlsSession_->recv(plainBuf, sizeof(plainBuf));
                    }
                }
                if (n > 0) {
                    lastClientPacket_ = std::chrono::steady_clock::now();
                    // The client retries READY if our acknowledgement was lost.
                    if (n == 9 && memcmp(plainBuf, "AB_READY2", 9) == 0) {
                        sendPacket(reinterpret_cast<const uint8_t*>("AB_READY2"), 9);
                    }
                    uint16_t declaredLen = 0;
                    if (n >= 16) memcpy(&declaredLen, plainBuf + 14, 2);
                    // Process decrypted application data (same as binary packet handling)
                    if (n >= 16 && plainBuf[0] == 1 && n == 16 + declaredLen) {
                        uint8_t type = plainBuf[1];
                        if (type == PACKET_CONTROL && n >= 17) {
                            uint8_t cmd = plainBuf[16];
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
                                printf("[NET] -> Client requested DISCONNECT\n");
                                {
                                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                                    endDtlsSessionLocked();
                                }
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
                        } else if (type == PACKET_MIC_AUDIO) {
                            uint16_t payloadLen;
                            memcpy(&payloadLen, plainBuf + 14, 2);
                            if (payloadLen > 2000) {
                                printf("[NET] WARN: MIC_AUDIO: payloadLen=%u is suspiciously large, dropping\n", payloadLen);
                            } else if (n >= 16 + payloadLen && onMicAudio_) {
                                onMicAudio_(plainBuf + 16, payloadLen);
                            } else if (n < 16 + payloadLen) {
                                printf("[NET] WARN: MIC_AUDIO: packet too short, need %d, got %d\n",
                                       16 + payloadLen, n);
                            }
                        } else if (type == PACKET_PING) {
                            // Respond with PONG immediately (via DTLS)
                            uint8_t pong[16];
                            writeHeader(pong, PACKET_PONG, 0);
                            memcpy(pong + 6, plainBuf + 6, 8);
                            sendPacket(pong, 16);
                        }
                        // PACKET_KEEPALIVE: lastClientPacket_ already updated above
                    }

                } else if (n < 0) {
                    endDtlsSession();
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
            } else {
                const std::string request(buf, received);
                if (request.rfind("AB_CONNECT2|", 0) == 0) {
                    // An unauthenticated connect must never evict a live session.
                    if (!connected_) {
                        acceptSecureConnection(senderAddr, request);
                    } else {
                        const char* busy = "AB_REJECT|Server already connected. Disconnect the other device first.";
                        sendto(streamSocket_, busy, static_cast<int>(strlen(busy)), 0,
                               (sockaddr*)&senderAddr, sizeof(senderAddr));
                    }
                } else if (request.rfind("AB_CONNECT|", 0) == 0) {
                    const char* reject = "AB_REJECT|Encryption required: update both apps";
                    sendto(streamSocket_, reject, static_cast<int>(strlen(reject)), 0,
                           (sockaddr*)&senderAddr, sizeof(senderAddr));
                }
                // Plaintext audio, controls, keepalives and disconnects are discarded.
            }
        }
        if (connected_ && std::chrono::steady_clock::now() - lastClientPacket_ > std::chrono::seconds(5)) {
            endDtlsSession();
            connected_ = false;
            paused_ = false;
            if (onDisconnect_) onDisconnect_();
        }
    }
}

void Network::acceptSecureConnection(const sockaddr_in& peerAddr, const std::string& request) {
    std::vector<std::string> fields;
    std::istringstream input(request);
    std::string field;
    while (std::getline(input, field, '|')) fields.push_back(field);
    if (fields.size() != 5 || !SecurePairing::validField(fields[1], 128) ||
        !SecurePairing::validField(fields[2], 128) || !SecurePairing::validKey(fields[3]) ||
        (fields[4] != "pair" && fields[4] != "resume")) return;
    const auto& name = fields[1];
    const auto& id = fields[2];
    auto reply = [&](const std::string& text) {
        sendto(streamSocket_, text.data(), static_cast<int>(text.size()), 0,
               (const sockaddr*)&peerAddr, sizeof(peerAddr));
    };
    std::string key = getPeerPsk(id);
    const bool pairing = fields[4] == "pair" || !isPeerApproved(id) || !SecurePairing::validStoredKey(key);
    if (pairing) {
        SecurePairing exchange;
        if (!exchange.generate()) { reply("AB_REJECT|Key generation failed"); return; }
        const std::string challenge = "AB_KEY2|" + exchange.publicKey();
        reply(challenge);
        // Retry the same challenge; never change a committed transcript on retry.
        std::string reveal;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            char packet[512];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(streamSocket_, packet, sizeof(packet), 0, (sockaddr*)&from, &fromLen);
            if (n > 0 && from.sin_addr.s_addr == peerAddr.sin_addr.s_addr && from.sin_port == peerAddr.sin_port) {
                std::string msg(packet, n);
                if (msg.rfind("AB_KEY2|", 0) == 0) { reveal = msg.substr(8); break; }
            }
            if (std::chrono::steady_clock::now() >= retry) {
                reply(challenge);
                retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
        }
        std::string code;
        if (reveal.empty() || SecurePairing::commitment(name, id, reveal) != fields[3] ||
            !exchange.derive(reveal, name, id, true, key, code)) {
            reply("AB_REJECT|Pairing verification failed"); return;
        }
        const std::string approvalToken = id + ":" + exchange.publicKey();
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingPeer_.clientId = approvalToken;
            pendingPeer_.clientName = name;
            pendingPeer_.responded = false;
            pendingPeer_.approved = false;
        }
        if (onPairRequest_) onPairRequest_(approvalToken, name, code);
        bool approved = false;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            pendingCv_.wait_for(lock, std::chrono::seconds(30), [this] {
                return pendingPeer_.responded || !running_;
            });
            approved = running_ && pendingPeer_.responded && pendingPeer_.approved;
            pendingPeer_.clientId.clear(); // Ignore stale approval buttons.
        }
        if (onPairRequest_) onPairRequest_("", "", ""); // Dismiss expired/answered prompt.
        if (!approved) { reply("AB_REJECT|Pairing denied or timed out"); return; }
    }
    reply("AB_ACCEPT2");
    if (!startDtlsHandshake(peerAddr, id, key)) return;
    // Do not persist a key or mark a peer connected until mutual authentication
    // and the client's encrypted READY prove possession of the derived key.
    touchPeer(id, name);
    setPeerPsk(id, key);
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clientAddr_ = peerAddr;
    }
    paused_ = false;
    sequence_ = 0;
    streamStart_ = std::chrono::steady_clock::now();
    lastClientPacket_ = streamStart_;
    connected_ = true;
    if (onConnect_) onConnect_(getPeerName(id));
}

void Network::writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen) {
    auto now = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now - streamStart_).count();

    buf[0] = 0x01;
    buf[1] = type;
    const uint32_t seq = sequence_++;
    memcpy(buf + 2, &seq, 4);
    memcpy(buf + 6, &timestamp, 8);
    memcpy(buf + 14, &payloadLen, 2);
}

void Network::sendPacket(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(dtlsMutex_);
    if (dtlsActive_ && dtlsSession_) dtlsSession_->send(data, len);
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

        if (!SecurePairing::validStoredKey(pskHex)) continue;
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

bool Network::startDtlsHandshake(const sockaddr_in& peer, const std::string& id,
                                 const std::string& storedKey) {
    endDtlsSession();
    if (!SecurePairing::validStoredKey(storedKey)) return false;
    auto session = std::make_unique<DtlsSession>();
    const auto key = DtlsSession::hexToBytes(storedKey.substr(3));
    if (!session->init(streamSocket_, peer, [id, key](const std::string& identity, std::vector<uint8_t>& out) {
        if (identity != id) return false;
        out = key;
        return true;
    })) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (running_ && std::chrono::steady_clock::now() < deadline) {
        if (!session->isEstablished() && std::chrono::steady_clock::now() >= retry) {
            sendto(streamSocket_, "AB_ACCEPT2", 10, 0, (const sockaddr*)&peer, sizeof(peer));
            retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        if (!session->isEstablished()) {
            int ret = session->continueHandshake();
            if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        } else {
            uint8_t plain[64];
            int n = session->recv(plain, sizeof(plain));
            if (n == 9 && memcmp(plain, "AB_READY2", 9) == 0) {
                if (session->send(plain, n) != n) return false;
                std::lock_guard<std::mutex> lock(dtlsMutex_);
                dtlsSession_ = std::move(session);
                dtlsActive_ = true;
                connectedClientId_ = id;
                return true;
            }
            if (n < 0) return false;
        }
        uint8_t packet[4096];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(streamSocket_, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                         (sockaddr*)&from, &fromLen);
        if (n > 0 && from.sin_addr.s_addr == peer.sin_addr.s_addr && from.sin_port == peer.sin_port &&
            packet[0] >= 20 && packet[0] <= 25) session->pushReceivedData(packet, n);
    }
    return false;
}

void Network::endDtlsSession() {
    std::lock_guard<std::mutex> lock(dtlsMutex_);
    endDtlsSessionLocked();
}

void Network::endDtlsSessionLocked() {
    dtlsActive_ = false;
    dtlsSession_.reset();
    connectedClientId_.clear();
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
