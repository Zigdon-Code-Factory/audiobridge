#include "network.h"
#include "secure_pairing.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <mach-o/dyld.h>

Network::Network() {}

Network::~Network() {
    shutdown();
}

bool Network::initialize(const std::string& serverName, uint16_t discoveryPort, uint16_t streamPort) {
    serverName_ = serverName;
    macAddress_ = getMacAddress();
    loadApprovedPeers();

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

    timeval timeout{0, 100000};
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sessions_ = std::make_unique<SecureServer>(streamSocket_, [this](const std::string& id) {
        return isPeerApproved(id) ? getPeerPsk(id) : "";
    });
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
    if (sessions_) sessions_->close();
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

std::string Network::getMacAddress() {
    // macOS: use sysctl to get MAC address
    int mib[6] = {CTL_NET, AF_ROUTE, 0, AF_LINK, NET_RT_IFLIST, 0};

    // Find en0 (primary ethernet/wifi)
    mib[5] = (int)if_nametoindex("en0");
    if (mib[5] == 0) return "00:00:00:00:00:00";

    size_t len = 0;
    if (sysctl(mib, 6, nullptr, &len, nullptr, 0) < 0) return "00:00:00:00:00:00";

    std::vector<char> macBuf(len);
    if (sysctl(mib, 6, macBuf.data(), &len, nullptr, 0) < 0) return "00:00:00:00:00:00";

    struct if_msghdr* ifm = (struct if_msghdr*)macBuf.data();
    struct sockaddr_dl* sdl = (struct sockaddr_dl*)(ifm + 1);
    unsigned char* mac = (unsigned char*)LLADDR(sdl);

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return macStr;
}

// --- Peer Persistence ---

std::string Network::getPeersFilePath() const {
    // macOS: use _NSGetExecutablePath
    char exePath[4096];
    uint32_t size = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &size) != 0) return "approved_peers.txt";

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

    socklen_t boundLen = sizeof(bindAddr);
    if (getsockname(streamSocket_, (sockaddr*)&bindAddr, &boundLen) != 0) return false;
    streamPort_ = ntohs(bindAddr.sin_port);

#include "network_sessions.inc"
