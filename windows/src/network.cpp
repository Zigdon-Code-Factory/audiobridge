#include "network.h"
#include "secure_pairing.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>

// Diagnostic log macro with timestamps for crash investigation
static const char* packetTypeName(uint8_t type) {
    switch (type) {
        case 0x01: return "AUDIO";
        case 0x02: return "KEEPALIVE";
        case 0x03: return "CONTROL";
        case 0x04: return "MIC_AUDIO";
        case 0x05: return "MEDIA_INFO";
        case 0x06: return "PING";
        case 0x07: return "PONG";
        case 0x08: return "SETTINGS";
        case 0x09: return "AUDIO_PCM";
        default: return "UNKNOWN";
    }
}

Network::Network() {}

Network::~Network() {
    shutdown();
}

bool Network::initialize(const std::string& serverName, uint16_t discoveryPort, uint16_t streamPort) {
    serverName_ = serverName;
    macAddress_ = getMacAddress();

    // Load approved peers from disk
    loadApprovedPeers();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[NET] WSAStartup failed\n");
        return false;
    }

    // Discovery socket (port 4011)
    discoverySocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discoverySocket_ == INVALID_SOCKET) {
        printf("[NET] Failed to create discovery socket\n");
        return false;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));
    BOOL broadcast = TRUE;
    setsockopt(discoverySocket_, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(discoveryPort);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discoverySocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("[NET] Failed to bind discovery socket on port 4011: %d\n", WSAGetLastError());
        return false;
    }

    // Stream socket (port 4012)
    streamSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (streamSocket_ == INVALID_SOCKET) {
        printf("[NET] Failed to create stream socket\n");
        return false;
    }

    setsockopt(streamSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

    bindAddr.sin_port = htons(streamPort);
    if (bind(streamSocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("[NET] Failed to bind stream socket on port 4012: %d\n", WSAGetLastError());
        return false;
    }

    int boundLen = sizeof(bindAddr);
    if (getsockname(streamSocket_, (sockaddr*)&bindAddr, &boundLen) != 0) return false;
    streamPort_ = ntohs(bindAddr.sin_port);

    // Set non-blocking receive timeout on stream socket for keepalive checks
    DWORD timeout = 100; // 100ms
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    // Enlarge UDP send/receive socket buffers to 256 KB so low-latency Raw PCM bursts never drop
    int sockBufBytes = 262144;
    setsockopt(streamSocket_, SOL_SOCKET, SO_SNDBUF, (char*)&sockBufBytes, sizeof(sockBufBytes));
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVBUF, (char*)&sockBufBytes, sizeof(sockBufBytes));

    // Initialize Windows qWAVE handle for 802.11e WMM Voice (AC_VO / DSCP 46 EF) Wi-Fi prioritization
    QOS_VERSION qosVer{1, 0};
    if (QOSCreateHandle(&qosVer, &qosHandle_)) {
        printf("[NET] qWAVE QoS handle initialized (Wi-Fi WMM AC_VO Voice ready)\n");
    } else {
        qosHandle_ = nullptr;
        printf("[NET] WARN: QOSCreateHandle failed (err=%lu), continuing without qWAVE\n", GetLastError());
    }

    sessions_ = std::make_unique<SecureServer>(streamSocket_, [this](const std::string& id) {
        return isPeerApproved(id) ? getPeerPsk(id) : "";
    });
    running_ = true;
    discoveryThread_ = std::thread(&Network::discoveryThread, this);
    streamThread_ = std::thread(&Network::streamThread, this);

    printf("[NET] Listening: discovery on :4011, stream on :4012\n");
    printf("[NET] Server MAC: %s\n", macAddress_.c_str());
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        printf("[NET] Approved peers: %zu\n", approvedPeers_.size());
    }
    return true;
}

void Network::shutdown() {
    if (!running_ && discoverySocket_ == INVALID_SOCKET && streamSocket_ == INVALID_SOCKET) return;
    printf("[NET] shutdown() called, running_=%d\n", running_.load());
    running_ = false;
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (streamThread_.joinable()) streamThread_.join();
    if (sessions_) sessions_->close();
    detachVoiceQosFlow();
    if (qosHandle_) {
        QOSCloseHandle(qosHandle_);
        qosHandle_ = nullptr;
    }
    if (discoverySocket_ != INVALID_SOCKET) { closesocket(discoverySocket_); discoverySocket_ = INVALID_SOCKET; }
    if (streamSocket_ != INVALID_SOCKET) { closesocket(streamSocket_); streamSocket_ = INVALID_SOCKET; }
    WSACleanup();
    printf("[NET] shutdown() complete\n");
}

void Network::discoveryThread() {
    printf("[NET] Discovery thread started (tid=%lu)\n", GetCurrentThreadId());
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
            snprintf(response, sizeof(response), "AB_OFFER|%s|%u|%s",
                     serverName_.c_str(), unsigned(streamPort_), macAddress_.c_str());

            sendto(discoverySocket_, response, (int)strlen(response), 0,
                   (sockaddr*)&senderAddr, addrLen);

            char addrStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, addrStr, sizeof(addrStr));
        }
    }
    printf("[NET] Discovery thread exiting\n");
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

        // Format: clientId|clientName|lastConnected(unix)|v2:psk_hex
        std::istringstream iss(line);
        std::string id, name, tsStr, pskHex;
        if (!std::getline(iss, id, '|')) continue;
        if (!std::getline(iss, name, '|')) continue;
        if (!std::getline(iss, tsStr, '|')) continue;
        std::getline(iss, pskHex, '|'); // Optional 4th field

        if (!SecurePairing::validStoredKey(pskHex)) continue;
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
        peer.pskHex = pskHex;
        approvedPeers_[id] = peer;
        loaded++;
    }

    if (loaded > 0 || expired > 0) {
        printf("[NET] Loaded %d approved peer(s), %d expired\n", loaded, expired);
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
        printf("[NET] Warning: Could not write peers file: %s\n", filePath.c_str());
        return;
    }

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

// --- DTLS ---

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

std::map<std::string, ApprovedPeer> Network::getApprovedPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    return approvedPeers_;
}

#include "network_sessions.inc"
