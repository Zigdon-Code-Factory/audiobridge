// Shared transport implementation for the Windows, Linux and macOS wrappers.
void Network::streamThread() {
    uint8_t buffer[4096];
    while (running_) {
        sockaddr_in peer{};
#ifdef _WIN32
        int addrLen = sizeof(peer);
#else
        socklen_t addrLen = sizeof(peer);
#endif
        int n = static_cast<int>(recvfrom(streamSocket_, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                                          (sockaddr *)&peer, &addrLen));
        auto events = sessions_->poll(n > 0 ? &peer : nullptr, buffer, n > 0 ? size_t(n) : 0);
        for (const auto &event : events) {
            switch (event.kind) {
            case SecureServer::Kind::Connected:
                if (!sessions_->publish(event, [&] {
                        touchPeer(event.id, event.name);
                        setPeerPsk(event.id, event.key);
                    }))
                    break;
#ifdef _WIN32
                refreshVoiceQos();
#endif
                if (onConnect_)
                    onConnect_(event.name);
                break;
            case SecureServer::Kind::Disconnected:
#ifdef _WIN32
                refreshVoiceQos();
#endif
                if (onDisconnect_)
                    onDisconnect_();
                break;
            case SecureServer::Kind::Pair:
                if (onPairRequest_)
                    onPairRequest_(event.id, event.name, event.code);
                break;
            case SecureServer::Kind::Pause:
                if (onPause_)
                    onPause_(isPaused());
                break;
            case SecureServer::Kind::Mic:
                if (onMicAudio_)
                    onMicAudio_(event.data.data(), static_cast<int>(event.data.size()));
                break;
            case SecureServer::Kind::MicReset:
                if (onMicAudio_)
                    onMicAudio_(nullptr, 0);
                break;
            case SecureServer::Kind::Media:
                if (onMediaCommand_)
                    onMediaCommand_(event.data[0]);
                break;
            case SecureServer::Kind::MediaInfo:
                if (onMediaInfoRequest_)
                    onMediaInfoRequest_();
                break;
            }
        }
    }
}

void Network::sendAudio(const uint8_t *data, int len) {
    if (sessions_ && data && len > 0 && len <= 2032)
        sessions_->broadcast(1, data, size_t(len));
}
void Network::sendAudioPcm(const int16_t *data, int frames) {
    if (!sessions_ || !data || frames <= 0 || frames > 960)
        return;
    // Bound DTLS records below the LAN MTU, even for a 20 ms capture quantum.
    // Every phone gets independent contiguous audio sequence numbers.
    for (int offset = 0; offset < frames;) {
        const int count = (std::min)(240, frames - offset);
        sessions_->broadcast(9, reinterpret_cast<const uint8_t *>(data + offset * 2), size_t(count) * 4);
        offset += count;
    }
}
void Network::sendKeepalive() {
    if (sessions_)
        sessions_->broadcast(2, nullptr, 0);
}
void Network::sendPing() {
    if (sessions_)
        sessions_->ping();
}
void Network::sendMediaInfo(const std::string &json) {
    if (sessions_)
        sessions_->broadcast(5, reinterpret_cast<const uint8_t *>(json.data()), json.size());
}
void Network::sendSettings(int jitterMs, int frameSizeMs, uint16_t frameSamples, bool rawPcm) {
    if (!sessions_)
        return;
    uint8_t data[8]{};
    uint16_t jitter = uint16_t(jitterMs), frame = uint16_t(frameSizeMs);
    uint16_t samples = frameSamples ? frameSamples : uint16_t(frameSizeMs * 48);
    memcpy(data, &jitter, 2);
    memcpy(data + 2, &frame, 2);
    memcpy(data + 4, &samples, 2);
    data[6] = rawPcm ? 1 : 0;
    data[7] = isVoiceQosActive() ? 1 : 0;
    sessions_->broadcast(8, data, sizeof(data));
}
std::string Network::getClientAddress() const {
    std::string result;
    if (sessions_)
        for (const auto &p : sessions_->clients()) {
            if (!result.empty())
                result += ", ";
            result += p.address;
        }
    return result;
}
std::string Network::getClientNames() const {
    std::string result;
    if (sessions_)
        for (const auto &p : sessions_->clients()) {
            if (!result.empty())
                result += ", ";
            result += p.name;
        }
    return result;
}
double Network::getClientRtt() const {
    double result = 0;
    if (sessions_)
        for (const auto &p : sessions_->clients())
            result = (std::max)(result, p.rtt);
    return result;
}
void Network::approvePeer(const std::string &token) {
    if (sessions_)
        sessions_->approve(token, true);
}
void Network::rejectPeer(const std::string &token) {
    if (sessions_)
        sessions_->approve(token, false);
}
void Network::revokePeer(const std::string &id) {
    const auto persist = [&] {
        std::lock_guard<std::mutex> lock(peersMutex_);
        approvedPeers_.erase(id);
        saveApprovedPeers();
    };
    if (sessions_)
        sessions_->revoke(id, persist);
    else
        persist();
}
#ifdef _WIN32
void Network::refreshVoiceQos() {
    const auto clients = sessions_->clients();
    std::lock_guard<std::mutex> lock(qosMutex_);
    if (!qosHandle_)
        return;
    for (auto it = qosFlows_.begin(); it != qosFlows_.end();) {
        bool present = false;
        for (const auto &p : clients)
            if (p.address == it->first)
                present = true;
        if (!present) {
            QOSRemoveSocketFromFlow(qosHandle_, streamSocket_, it->second, 0);
            it = qosFlows_.erase(it);
        } else
            ++it;
    }
    for (const auto &p : clients)
        if (!qosFlows_.count(p.address)) {
            QOS_FLOWID flow = 0;
            sockaddr_in target = p.peer;
            if (QOSAddSocketToFlow(qosHandle_, streamSocket_, reinterpret_cast<PSOCKADDR>(&target),
                                   QOSTrafficTypeVoice, QOS_NON_ADAPTIVE_FLOW, &flow))
                qosFlows_[p.address] = flow;
        }
    qosFlowCount_ = qosFlows_.size();
}
void Network::detachVoiceQosFlow() {
    std::lock_guard<std::mutex> lock(qosMutex_);
    for (const auto &flow : qosFlows_)
        if (qosHandle_)
            QOSRemoveSocketFromFlow(qosHandle_, streamSocket_, flow.second, 0);
    qosFlows_.clear();
    qosFlowCount_ = 0;
}
#endif
