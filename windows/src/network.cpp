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
    pendingCv_.notify_all();
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (streamThread_.joinable()) streamThread_.join();
    endDtlsSession();
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

void Network::streamThread() {
    char buf[4096];
    while (running_) {
        sockaddr_in senderAddr{};
        int addrLen = sizeof(senderAddr);
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
                    if (!connected_) acceptSecureConnection(senderAddr, request);
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
            int fromLen = sizeof(from);
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
    attachVoiceQosFlow(peerAddr);
    dtlsRecvCount_ = 0;
    dtlsDecryptCount_ = 0;
    dtlsSendCount_ = 0;
    dtlsSendErrCount_ = 0;
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

    const bool isAudio = (type == PACKET_AUDIO || type == PACKET_AUDIO_PCM);
    const uint32_t seq = isAudio ? sequence_.fetch_add(1, std::memory_order_relaxed) : 0;

    buf[0] = 0x01; // version
    buf[1] = type;
    memcpy(buf + 2, &seq, 4);
    memcpy(buf + 6, &timestamp, 8);
    memcpy(buf + 14, &payloadLen, 2);
}

void Network::sendPacket(const uint8_t* data, size_t len) {
    // Acquires dtlsMutex_ to safely access DTLS state and send.
    // There is deliberately no plaintext fallback.
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

void Network::sendAudioPcm(const int16_t* pcmStereo, int frameCount) {
    if (!connected_ || paused_) return;
    if (!pcmStereo || frameCount <= 0) return;

    const int payloadLen = frameCount * 2 * (int)sizeof(int16_t);
    if (payloadLen <= 0 || payloadLen + 16 > 2048) {
        printf("[NET] sendAudioPcm: invalid frameCount=%d (payloadLen=%d)\n", frameCount, payloadLen);
        return;
    }

    static int pcmCount = 0;
    pcmCount++;
    if (pcmCount <= 5 || (pcmCount % 5000) == 0) {
        printf("[NET] sendAudioPcm #%d: frames=%d (%d bytes), qos=%s, dtls=%s\n",
               pcmCount, frameCount, payloadLen,
               isVoiceQosActive() ? "AC_VO" : "BE",
               dtlsActive_ ? "YES" : "NO");
    }

    uint8_t packet[2048];
    writeHeader(packet, PACKET_AUDIO_PCM, (uint16_t)payloadLen);
    memcpy(packet + 16, pcmStereo, payloadLen);
    sendPacket(packet, 16 + payloadLen);
}

void Network::attachVoiceQosFlow(const sockaddr_in& destAddr) {
    std::lock_guard<std::mutex> lock(qosMutex_);
    if (!qosHandle_ || streamSocket_ == INVALID_SOCKET) return;

    QOS_FLOWID prevFlow = qosFlowId_.exchange(0);
    if (prevFlow != 0) {
        QOSRemoveSocketFromFlow(qosHandle_, streamSocket_, prevFlow, 0);
    }

    QOS_FLOWID newFlow = 0;
    sockaddr_in target = destAddr;
    if (QOSAddSocketToFlow(qosHandle_, streamSocket_, reinterpret_cast<PSOCKADDR>(&target),
                           QOSTrafficTypeVoice, QOS_NON_ADAPTIVE_FLOW, &newFlow)) {
        qosFlowId_.store(newFlow, std::memory_order_relaxed);
        char ipStr[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &target.sin_addr, ipStr, sizeof(ipStr));
        printf("[NET] Attached Wi-Fi WMM Voice QoS (AC_VO / DSCP EF, flowId=%lu) -> %s:%u\n",
               (unsigned long)newFlow, ipStr, ntohs(target.sin_port));
    } else {
        printf("[NET] WARN: QOSAddSocketToFlow(Voice) failed (err=%lu)\n", GetLastError());
    }
}

void Network::detachVoiceQosFlow() {
    std::lock_guard<std::mutex> lock(qosMutex_);
    QOS_FLOWID prevFlow = qosFlowId_.exchange(0);
    if (prevFlow != 0 && qosHandle_ && streamSocket_ != INVALID_SOCKET) {
        QOSRemoveSocketFromFlow(qosHandle_, streamSocket_, prevFlow, 0);
        printf("[NET] Detached Wi-Fi WMM Voice QoS flowId=%lu\n", (unsigned long)prevFlow);
    }
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

void Network::sendSettings(int jitterMs, int frameSizeMs, uint16_t frameSamples, bool rawPcm) {
    if (!connected_) return;

    uint8_t packet[32];
    uint16_t payloadLen = 8;
    writeHeader(packet, PACKET_SETTINGS, payloadLen);
    uint16_t jitter = (uint16_t)jitterMs;
    uint16_t frameMs = (uint16_t)frameSizeMs;
    uint16_t samples = frameSamples > 0 ? frameSamples : (uint16_t)(frameSizeMs * 48);
    uint8_t codec = rawPcm ? 1 : 0; // 0 = Opus, 1 = Raw 16-bit PCM
    uint8_t flags = isVoiceQosActive() ? 1 : 0;
    memcpy(packet + 16, &jitter, 2);
    memcpy(packet + 18, &frameMs, 2);
    memcpy(packet + 20, &samples, 2);
    packet[22] = codec;
    packet[23] = flags;
    sendPacket(packet, 16 + payloadLen);
    printf("[NET] Sent settings to client: jitter=%dms frame=%dms (%u samples) codec=%s qos=%s\n",
           jitterMs, frameSizeMs, samples, rawPcm ? "PCM" : "Opus", flags ? "AC_VO" : "BE");
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
        int fromLen = sizeof(from);
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
