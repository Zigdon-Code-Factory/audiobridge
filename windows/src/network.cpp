#include "network.h"
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
        default: return "UNKNOWN";
    }
}

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
    bindAddr.sin_port = htons(4011);
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

    bindAddr.sin_port = htons(4012);
    if (bind(streamSocket_, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        printf("[NET] Failed to bind stream socket on port 4012: %d\n", WSAGetLastError());
        return false;
    }

    // Set non-blocking receive timeout on stream socket for keepalive checks
    DWORD timeout = 100; // 100ms
    setsockopt(streamSocket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

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
    printf("[NET] shutdown() called, running_=%d\n", running_.load());
    running_ = false;
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (streamThread_.joinable()) streamThread_.join();
    endDtlsSession();
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
            snprintf(response, sizeof(response), "AB_OFFER|%s|4012|%s",
                     serverName_.c_str(), macAddress_.c_str());

            sendto(discoverySocket_, response, (int)strlen(response), 0,
                   (sockaddr*)&senderAddr, addrLen);

            char addrStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, addrStr, sizeof(addrStr));
        }
    }
    printf("[NET] Discovery thread exiting\n");
}

void Network::streamThread() {
    printf("[NET] Stream thread started (tid=%lu)\n", GetCurrentThreadId());
    char buf[2048];
    while (running_) {
        sockaddr_in senderAddr{};
        int addrLen = sizeof(senderAddr);

        int received = recvfrom(streamSocket_, buf, sizeof(buf) - 1, 0,
                                 (sockaddr*)&senderAddr, &addrLen);

        if (received > 0) {
            uint8_t firstByte = (uint8_t)buf[0];

            // DTLS record (content types 20-25) — feed to DTLS engine
            bool isDtls;
            DtlsSession* session;
            {
                std::lock_guard<std::mutex> lock(dtlsMutex_);
                isDtls = dtlsActive_;
                session = dtlsSession_.get();
            }

            if (isDtls && session && firstByte >= 20 && firstByte <= 25) {
                dtlsRecvCount_++;
                if (dtlsRecvCount_ <= 5 || (dtlsRecvCount_ % 1000) == 0) {
                    printf("[NET] DTLS record #%d received (%d bytes, type=%d)\n",
                           dtlsRecvCount_.load(), received, firstByte);
                }

                // Push data and decrypt under lock
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    if (!dtlsSession_) {
                        printf("[NET] WARN: dtlsSession_ became null between check and use\n");
                        goto check_timeout;
                    }
                    dtlsSession_->pushReceivedData((const uint8_t*)buf, received);
                }

                uint8_t plainBuf[2048];
                int n;
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    if (!dtlsSession_) {
                        printf("[NET] WARN: dtlsSession_ null before recv\n");
                        goto check_timeout;
                    }
                    n = dtlsSession_->recv(plainBuf, sizeof(plainBuf));
                }

                if (n > 0) {
                    dtlsDecryptCount_++;
                    if (dtlsDecryptCount_ <= 5 || (dtlsDecryptCount_ % 1000) == 0) {
                        printf("[NET] Decrypted #%d: %d bytes, pkt type=0x%02X (%s)\n",
                               dtlsDecryptCount_.load(), n,
                               n >= 2 ? plainBuf[1] : 0,
                               n >= 2 ? packetTypeName(plainBuf[1]) : "?");
                    }
                    lastClientPacket_ = std::chrono::steady_clock::now();
                    // Process decrypted application data (same as binary packet handling)
                    if (n >= 16) {
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
                        } else if (type == PACKET_PONG) {
                            if (pingPending_) {
                                auto now = std::chrono::steady_clock::now();
                                double rtt = std::chrono::duration<double, std::milli>(
                                    now - lastPingSent_).count();
                                pingPending_ = false;
                                double prev = clientRttMs_.load();
                                if (prev == 0.0) {
                                    clientRttMs_.store(rtt);
                                } else {
                                    clientRttMs_.store(prev * 0.7 + rtt * 0.3);
                                }
                            }
                        }
                        // PACKET_KEEPALIVE: lastClientPacket_ already updated above
                    } else {
                        printf("[NET] WARN: Decrypted packet too short (%d bytes, need >=16)\n", n);
                    }
                } else if (n < 0) {
                    // DTLS session broken
                    printf("[NET] DTLS recv error (n=%d), disconnecting\n", n);
                    {
                        std::lock_guard<std::mutex> lock(dtlsMutex_);
                        endDtlsSessionLocked();
                    }
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
                goto check_timeout;
            }

            // During DTLS handshake (not yet established), feed DTLS records
            {
                std::lock_guard<std::mutex> lock(dtlsMutex_);
                if (dtlsSession_ && !dtlsActive_ && firstByte >= 20 && firstByte <= 25) {
                    dtlsSession_->pushReceivedData((const uint8_t*)buf, received);
                    // Handshake is driven by startDtlsHandshake(), not here
                    goto check_timeout;
                }
            }

            buf[received] = '\0';

            // Check for text control messages
            if (strncmp(buf, "AB_CONNECT|", 11) == 0) {
                printf("[NET] ---- AB_CONNECT received (%d bytes) ----\n", received);

                // Disconnect existing client if any
                if (connected_) {
                    printf("[NET] Disconnecting existing client before accepting new one\n");
                    {
                        std::lock_guard<std::mutex> lock(dtlsMutex_);
                        endDtlsSessionLocked();
                    }
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }

                // Parse: AB_CONNECT|<name>|<clientId>
                std::string payload(buf + 11);
                std::string clientName = payload;
                std::string clientId;

                size_t sep = payload.find('|');
                if (sep != std::string::npos) {
                    clientName = payload.substr(0, sep);
                    clientId = payload.substr(sep + 1);
                }

                // Sanitize: trim trailing whitespace/nulls from clientId
                while (!clientId.empty() && (clientId.back() == '\0' || clientId.back() == '\n' || clientId.back() == '\r')) {
                    clientId.pop_back();
                }

                char senderStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &senderAddr.sin_addr, senderStr, sizeof(senderStr));
                printf("[NET] AB_CONNECT from '%s' (id: '%s', len=%zu) at %s:%d\n",
                       clientName.c_str(), clientId.c_str(), clientId.size(),
                       senderStr, ntohs(senderAddr.sin_port));

                // Check if this peer is approved
                if (!clientId.empty() && isPeerApproved(clientId)) {
                    // Known peer — auto-accept
                    std::string storedName = getPeerName(clientId);
                    std::string pskHex = getPeerPsk(clientId);
                    printf("[NET] Known peer '%s', PSK %s (len=%zu)\n", storedName.c_str(),
                           pskHex.empty() ? "EMPTY (will generate)" : "present", pskHex.size());
                    touchPeer(clientId, clientName);

                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        clientAddr_ = senderAddr;
                    }

                    if (pskHex.empty()) {
                        pskHex = DtlsSession::generatePsk();
                        printf("[NET] Generated new PSK for peer %s\n", clientId.c_str());
                        setPeerPsk(clientId, pskHex);
                    }
                    // Always send PSK so client is guaranteed to have it
                    std::string acceptMsg = "AB_ACCEPT|" + DtlsSession::pskToBase64(pskHex);
                    sendto(streamSocket_, acceptMsg.c_str(), (int)acceptMsg.size(), 0,
                           (sockaddr*)&senderAddr, addrLen);
                    printf("[NET] Sent AB_ACCEPT with PSK to %s (%s), msg len=%zu\n",
                           storedName.c_str(), clientId.c_str(), acceptMsg.size());

                    // Reset per-connection diagnostic counters
                    dtlsRecvCount_ = 0;
                    dtlsDecryptCount_ = 0;
                    dtlsSendCount_ = 0;
                    dtlsSendErrCount_ = 0;

                    // Perform DTLS handshake
                    printf("[NET] Starting DTLS handshake...\n");
                    if (startDtlsHandshake(senderAddr, clientId)) {
                        connected_ = true;
                        paused_ = false;
                        sequence_ = 0;
                        streamStart_ = std::chrono::steady_clock::now();
                        lastClientPacket_ = streamStart_;
                        printf("[NET] DTLS connected to %s (encrypted)\n", storedName.c_str());
                        if (onConnect_) onConnect_(storedName);
                    } else {
                        // DTLS failed — fall back to unencrypted
                        printf("[NET] DTLS handshake FAILED, falling back to unencrypted for %s\n", storedName.c_str());
                        connected_ = true;
                        paused_ = false;
                        sequence_ = 0;
                        streamStart_ = std::chrono::steady_clock::now();
                        lastClientPacket_ = streamStart_;
                        if (onConnect_) onConnect_(storedName);
                    }
                } else if (clientId.empty()) {
                    // Legacy client without ID — no DTLS
                    printf("[NET] Legacy client (no ID), accepting without DTLS\n");
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

                    printf("[NET] Accepted legacy client: %s\n", clientName.c_str());
                    if (onConnect_) onConnect_(clientName);
                } else {
                    // Unknown peer — require approval
                    printf("[NET] Unknown peer, sending AB_PAIR_PENDING\n");
                    const char* pending = "AB_PAIR_PENDING";
                    sendto(streamSocket_, pending, (int)strlen(pending), 0,
                           (sockaddr*)&senderAddr, addrLen);

                    printf("[NET] New device wants to pair: '%s' (ID: %s)\n",
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
                            sendto(streamSocket_, acceptMsg.c_str(), (int)acceptMsg.size(), 0,
                                   (sockaddr*)&senderAddr, addrLen);

                            // Reset per-connection diagnostic counters
                            dtlsRecvCount_ = 0;
                            dtlsDecryptCount_ = 0;
                            dtlsSendCount_ = 0;
                            dtlsSendErrCount_ = 0;

                            // Perform DTLS handshake
                            printf("[NET] Starting DTLS handshake for new peer...\n");
                            if (startDtlsHandshake(senderAddr, clientId)) {
                                connected_ = true;
                                paused_ = false;
                                sequence_ = 0;
                                streamStart_ = std::chrono::steady_clock::now();
                                lastClientPacket_ = streamStart_;
                                printf("[NET] DTLS connected to new peer %s\n", clientName.c_str());
                                if (onConnect_) onConnect_(clientName);
                            } else {
                                printf("[NET] DTLS failed, falling back to unencrypted for new peer %s\n", clientName.c_str());
                                connected_ = true;
                                paused_ = false;
                                sequence_ = 0;
                                streamStart_ = std::chrono::steady_clock::now();
                                lastClientPacket_ = streamStart_;
                                if (onConnect_) onConnect_(clientName);
                            }
                        } else {
                            // Rejected or timed out
                            const char* reject = pendingPeer_.responded
                                ? "AB_REJECT|Denied by server"
                                : "AB_REJECT|Approval timed out";
                            sendto(streamSocket_, reject, (int)strlen(reject), 0,
                                   (sockaddr*)&senderAddr, addrLen);
                            printf("[NET] Pair request %s for %s\n",
                                   pendingPeer_.responded ? "denied" : "timed out",
                                   clientName.c_str());
                        }
                    }
                }
                continue;
            }

            if (strcmp(buf, "AB_DISCONNECT") == 0) {
                printf("[NET] AB_DISCONNECT received (plaintext)\n");
                if (connected_) {
                    {
                        std::lock_guard<std::mutex> lock(dtlsMutex_);
                        endDtlsSessionLocked();
                    }
                    connected_ = false;
                    paused_ = false;
                    if (onDisconnect_) onDisconnect_();
                }
                continue;
            }

            // Unencrypted binary packet from client (legacy mode, no DTLS)
            if (connected_ && !dtlsActive_ && received >= 16) {
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
                    printf("[NET] Control command (legacy): 0x%02X\n", cmd);
                    if (cmd == CTRL_PAUSE) {
                        printf("[NET] -> PAUSE stream\n");
                        paused_ = true;
                        if (onPause_) onPause_(true);
                    } else if (cmd == CTRL_RESUME) {
                        printf("[NET] -> RESUME stream\n");
                        paused_ = false;
                        if (onPause_) onPause_(false);
                    } else if (cmd == CTRL_DISCONNECT) {
                        printf("[NET] -> Client requested DISCONNECT (legacy)\n");
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
                    if (payloadLen > 2000) {
                        printf("[NET] WARN: MIC_AUDIO (legacy): payloadLen=%u is suspiciously large, dropping\n", payloadLen);
                    } else if (received >= 16 + payloadLen && onMicAudio_) {
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
                    sendPacket(pong, 16);
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

check_timeout:
        // Check for client timeout (5 seconds)
        if (connected_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastClientPacket_).count();
            if (elapsed > 5000) {
                printf("[NET] Client timed out (no packets for %lldms)\n", (long long)elapsed);
                {
                    std::lock_guard<std::mutex> lock(dtlsMutex_);
                    endDtlsSessionLocked();
                }
                connected_ = false;
                paused_ = false;
                printf("[NET] Client disconnected due to timeout\n");
                if (onDisconnect_) onDisconnect_();
            }
        }
    }
    printf("[NET] Stream thread exiting\n");
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

void Network::sendPacket(const uint8_t* data, size_t len) {
    // Acquires dtlsMutex_ to safely access DTLS state and send.
    // For unencrypted sends, also reads clientAddr_ under this lock.
    // Callers should NOT hold clientMutex_ — this method handles all locking.
    std::lock_guard<std::mutex> lock(dtlsMutex_);
    if (dtlsActive_ && dtlsSession_) {
        dtlsSendCount_++;
        int ret = dtlsSession_->send(data, len);
        if (ret < 0) {
            dtlsSendErrCount_++;
            if (dtlsSendErrCount_ <= 10 || (dtlsSendErrCount_ % 100) == 0) {
                printf("[NET] DTLS send error #%d: ret=%d (packet len=%zu, type=0x%02X)\n",
                       dtlsSendErrCount_.load(), ret, len, len >= 2 ? data[1] : 0);
            }
            // If we're getting a lot of send errors, the session is probably broken
            if (dtlsSendErrCount_ >= 50) {
                printf("[NET] CRITICAL: Too many DTLS send errors (%d), tearing down session\n",
                       dtlsSendErrCount_.load());
                endDtlsSessionLocked();
                connected_ = false;
                paused_ = false;
            }
        }
    } else if (!dtlsActive_) {
        sockaddr_in addr;
        {
            std::lock_guard<std::mutex> cLock(clientMutex_);
            addr = clientAddr_;
        }
        sendto(streamSocket_, (const char*)data, (int)len, 0,
               (sockaddr*)&addr, sizeof(addr));
    } else {
        // dtlsActive_ is true but dtlsSession_ is null — should not happen
        printf("[NET] BUG: dtlsActive_=true but dtlsSession_=null! Clearing dtlsActive_\n");
        dtlsActive_ = false;
    }
}

void Network::sendAudio(const uint8_t* opusData, int opusLen) {
    if (!connected_ || paused_) return;

    // Validate input
    if (!opusData || opusLen <= 0 || opusLen > 2000) {
        printf("[NET] sendAudio: invalid args (opusData=%p, opusLen=%d)\n",
               (void*)opusData, opusLen);
        return;
    }

    static int audioCount = 0;
    audioCount++;
    if (audioCount <= 5 || (audioCount % 5000) == 0) {
        printf("[NET] sendAudio #%d: opusLen=%d, dtls=%s, connected=%d, paused=%d\n",
               audioCount, opusLen, dtlsActive_ ? "YES" : "NO",
               connected_.load(), paused_.load());
    }

    uint8_t packet[2048];
    if (opusLen + 16 > (int)sizeof(packet)) {
        printf("[NET] sendAudio: packet too large (%d + 16 > 2048)\n", opusLen);
        return;
    }

    writeHeader(packet, PACKET_AUDIO, (uint16_t)opusLen);
    memcpy(packet + 16, opusData, opusLen);

    // Diagnostic: log header bytes and first opus bytes being sent
    static int sendAudioDiag = 0;
    sendAudioDiag++;
    if (sendAudioDiag <= 5 || (sendAudioDiag % 500) == 0) {
        printf("[NET-DIAG] sendAudio #%d: hdr[0..15]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, opusLen=%d, opus[0..3]=%02x %02x %02x %02x\n",
               sendAudioDiag,
               packet[0], packet[1], packet[2], packet[3],
               packet[4], packet[5], packet[6], packet[7],
               packet[8], packet[9], packet[10], packet[11],
               packet[12], packet[13], packet[14], packet[15],
               opusLen,
               opusLen > 0 ? opusData[0] : 0,
               opusLen > 1 ? opusData[1] : 0,
               opusLen > 2 ? opusData[2] : 0,
               opusLen > 3 ? opusData[3] : 0);
    }

    sendPacket(packet, 16 + opusLen);
}

void Network::sendKeepalive() {
    if (!connected_) return;

    uint8_t packet[16];
    writeHeader(packet, PACKET_KEEPALIVE, 0);

    sendPacket(packet, 16);
}

void Network::sendPing() {
    if (!connected_) return;

    uint8_t packet[16];
    writeHeader(packet, PACKET_PING, 0);

    lastPingSent_ = std::chrono::steady_clock::now();
    pingPending_ = true;

    sendPacket(packet, 16);
}

void Network::sendMediaInfo(const std::string& json) {
    if (!connected_) return;

    uint8_t packet[2048];
    int payloadLen = (int)json.size();
    if (payloadLen + 16 > (int)sizeof(packet)) {
        printf("[NET] sendMediaInfo: payload too large (%d bytes)\n", payloadLen);
        return;
    }

    writeHeader(packet, PACKET_MEDIA_INFO, (uint16_t)payloadLen);
    memcpy(packet + 16, json.c_str(), payloadLen);

    sendPacket(packet, 16 + payloadLen);
}

void Network::sendSettings(int jitterMs, int frameSizeMs) {
    if (!connected_) return;

    uint8_t packet[32];
    uint16_t payloadLen = 4;
    writeHeader(packet, PACKET_SETTINGS, payloadLen);
    uint16_t jitter = (uint16_t)jitterMs;
    uint16_t frameMs = (uint16_t)frameSizeMs;
    memcpy(packet + 16, &jitter, 2);
    memcpy(packet + 18, &frameMs, 2);
    sendPacket(packet, 16 + payloadLen);
    printf("[NET] Sent settings to client: jitter=%dms frameSize=%dms\n", jitterMs, frameSizeMs);
}

std::string Network::getClientAddress() const {
    if (!connected_) return "";
    std::lock_guard<std::mutex> lock(clientMutex_);
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

        // Format: clientId|clientName|lastConnected(unix)|psk_hex
        std::istringstream iss(line);
        std::string id, name, tsStr, pskHex;
        if (!std::getline(iss, id, '|')) continue;
        if (!std::getline(iss, name, '|')) continue;
        if (!std::getline(iss, tsStr, '|')) continue;
        std::getline(iss, pskHex, '|'); // Optional 4th field

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

bool Network::startDtlsHandshake(const sockaddr_in& clientAddr, const std::string& clientId) {
    // Disabled temporarily for instant connection testing
    return false;
    {
        std::lock_guard<std::mutex> lock(dtlsMutex_);
        endDtlsSessionLocked(); // Clean up any existing session
    }

    auto newSession = std::make_unique<DtlsSession>();
    connectedClientId_ = clientId;

    auto lookupFn = [this](const std::string& identity, std::vector<uint8_t>& outPsk) -> bool {
        return pskLookup(identity, outPsk);
    };

    if (!newSession->init((int)streamSocket_, clientAddr, lookupFn)) {
        printf("[NET] DTLS init failed\n");
        connectedClientId_.clear();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(dtlsMutex_);
        dtlsSession_ = std::move(newSession);
    }

    printf("[NET] Waiting for DTLS handshake (timeout=3s)...\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(dtlsMutex_);
            if (!dtlsSession_) {
                printf("[NET] DTLS session disappeared during handshake\n");
                connectedClientId_.clear();
                return false;
            }
            if (dtlsSession_->isEstablished()) {
                break;
            }
        }

        if (std::chrono::steady_clock::now() > deadline) {
            printf("[NET] DTLS handshake timed out after 3s\n");
            std::lock_guard<std::mutex> lock(dtlsMutex_);
            if (dtlsSession_) {
                dtlsSession_->teardown();
                dtlsSession_.reset();
            }
            connectedClientId_.clear();
            return false;
        }

        // Read a datagram from the socket
        char buf[2048];
        sockaddr_in sender{};
        int slen = sizeof(sender);
        int n = recvfrom(streamSocket_, buf, sizeof(buf), 0, (sockaddr*)&sender, &slen);
        if (n <= 0) continue;

        uint8_t firstByte = (uint8_t)buf[0];
        if (firstByte >= 20 && firstByte <= 25) {
            std::lock_guard<std::mutex> lock(dtlsMutex_);
            if (!dtlsSession_) {
                printf("[NET] DTLS session null during handshake recv\n");
                connectedClientId_.clear();
                return false;
            }
            // DTLS record — feed to session
            dtlsSession_->pushReceivedData((const uint8_t*)buf, n);
            int ret = dtlsSession_->continueHandshake();
            if (ret == 0) {
                // Handshake complete
                dtlsActive_ = true;
                printf("[NET] DTLS handshake complete (cipher: negotiated)\n");
                return true;
            }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                printf("[NET] DTLS handshake error: -0x%04X\n", -ret);
                dtlsSession_->teardown();
                dtlsSession_.reset();
                connectedClientId_.clear();
                return false;
            }
        } else {
            printf("[NET] Non-DTLS packet during handshake: firstByte=0x%02X, len=%d, ignoring\n",
                   firstByte, n);
        }
        // Ignore non-DTLS packets during handshake
    }

    // Check if handshake completed (loop exited due to isEstablished)
    {
        std::lock_guard<std::mutex> lock(dtlsMutex_);
        if (dtlsSession_ && dtlsSession_->isEstablished()) {
            dtlsActive_ = true;
            printf("[NET] DTLS handshake complete\n");
            return true;
        }
    }

    return false;
}

void Network::endDtlsSession() {
    std::lock_guard<std::mutex> lock(dtlsMutex_);
    endDtlsSessionLocked();
}

void Network::endDtlsSessionLocked() {
    // Must be called with dtlsMutex_ held
    if (dtlsSession_) {
        printf("[NET] Tearing down DTLS session (sends=%d, sendErrs=%d, recvd=%d, decrypted=%d)\n",
               dtlsSendCount_.load(), dtlsSendErrCount_.load(),
               dtlsRecvCount_.load(), dtlsDecryptCount_.load());
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
        printf("[NET] PSK lookup FAILED for identity '%s'\n", identity.c_str());
        return false;
    }
    outPsk = DtlsSession::hexToBytes(it->second.pskHex);
    printf("[NET] PSK lookup OK for identity '%s' (%zu bytes)\n", identity.c_str(), outPsk.size());
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
    printf("[NET] Revoked peer: %s\n", clientId.c_str());
}
