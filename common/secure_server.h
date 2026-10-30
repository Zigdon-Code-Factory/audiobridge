#pragma once
#include "dtls_session.h"
#include "secure_pairing.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

// One socket reader drives bounded, independent DTLS sessions. No pairing wait
// owns the receive loop; an unapproved/slow phone cannot suspend existing audio.
class SecureServer {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t maxClients = 8;
    struct ClientInfo {
        std::string id, name, address;
        sockaddr_in peer{};
        bool paused;
        double rtt;
    };
    enum class Kind { Connected, Disconnected, Pair, Pause, Mic, Media, MediaInfo, MicReset };
    struct Event {
        Kind kind;
        std::string id, name, key, code;
        std::vector<uint8_t> data;
    };
    using Lookup = std::function<std::string(const std::string &)>;
    SecureServer(DtlsSocket socket, Lookup lookup) : socket_(socket), lookup_(std::move(lookup)) {}

    std::vector<Event> poll(const sockaddr_in *from = nullptr, const uint8_t *bytes = nullptr,
                            size_t size = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (from && bytes && size)
            receive(*from, bytes, size);
        const auto now = Clock::now();
        bool heartbeat = false;
        for (auto &item : peers_) {
            auto &p = *item.second;
            if (p.dead)
                continue;
            if (p.phase == Phase::Active) {
                if (now - p.lastPacket > std::chrono::seconds(5)) {
                    p.dead = true;
                    continue;
                }
                if (now >= p.retry) {
                    send(p, 2, nullptr, 0);
                    heartbeat = true;
                    p.retry = now + std::chrono::milliseconds(500);
                }
            } else if (now >= p.deadline) {
                reject(p, "Pairing or authentication timed out");
            } else if (p.phase == Phase::Reveal && now >= p.retry) {
                text(p.peer, "AB_KEY2|" + p.exchange->publicKey());
                p.retry = now + std::chrono::milliseconds(500);
            } else if (p.phase == Phase::Handshake) {
                if (now >= p.retry && !p.dtls->isEstablished()) {
                    text(p.peer, "AB_ACCEPT2");
                    p.retry = now + std::chrono::milliseconds(500);
                }
                drive(p);
            }
        }
        reap();
        if (heartbeat)
            sessionInfo();
        auto result = std::move(events_);
        events_.clear();
        return result;
    }
    void approve(const std::string &token, bool allowed) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &item : peers_) {
            auto &p = *item.second;
            if (p.phase != Phase::Approval || p.token != token || p.dead)
                continue;
            if (Clock::now() >= p.deadline || !allowed)
                reject(p, "Pairing denied or timed out");
            else {
                dismiss(p);
                beginHandshake(p);
            }
            break;
        }
    }
    void revoke(const std::string &id, const std::function<void()> &persist = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &item : peers_)
            if (item.second->id == id) {
                auto &p = *item.second;
                if (p.phase == Phase::Active) {
                    uint8_t cmd = 3;
                    send(p, 3, &cmd, 1);
                }
                p.dead = true;
            }
        if (persist)
            persist();
    }
    bool publish(const Event &event, const std::function<void()> &persist) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &item : peers_) {
            const auto &p = *item.second;
            if (!p.dead && p.phase == Phase::Active && p.id == event.id && p.key == event.key) {
                persist();
                return true;
            }
        }
        return false;
    }
    void broadcast(uint8_t type, const uint8_t *data, size_t len) {
        if (len > 2032)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &item : peers_) {
            auto &p = *item.second;
            if (p.phase != Phase::Active || p.dead || ((type == 1 || type == 9) && p.paused))
                continue;
            send(p, type, data, len);
        }
    }
    void ping() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &item : peers_) {
            auto &p = *item.second;
            if (p.phase != Phase::Active || p.dead)
                continue;
            p.pingSent = Clock::now();
            p.pingPending = true;
            p.pingStamp = timestamp(p);
            send(p, 6, nullptr, 0, p.pingStamp);
        }
    }
    std::vector<ClientInfo> clients() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ClientInfo> result;
        for (const auto &item : peers_) {
            const auto &p = *item.second;
            if (p.phase == Phase::Active && !p.dead)
                result.push_back({p.id, p.name, item.first, p.peer, p.paused, p.rtt});
        }
        return result;
    }
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto &item : peers_)
            if (!item.second->dead && item.second->phase == Phase::Active)
                ++count;
        return count;
    }
    bool allPaused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        bool any = false;
        for (const auto &item : peers_)
            if (!item.second->dead && item.second->phase == Phase::Active) {
                any = true;
                if (!item.second->paused)
                    return false;
            }
        return any;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        peers_.clear();
        events_.clear();
        micOwner_.clear();
        prompt_.clear();
    }
    std::string micOwner() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(micOwner_);
        return it == peers_.end() ? "" : it->second->name;
    }

  private:
    enum class Phase { Reveal, Approval, Handshake, Active };
    struct Peer {
        sockaddr_in peer{};
        std::string endpoint, id, name, commitment, key, token;
        Phase phase = Phase::Reveal;
        std::unique_ptr<SecurePairing> exchange;
        std::unique_ptr<DtlsSession> dtls;
        Clock::time_point deadline, retry, lastPacket, start, pingSent;
        bool dead = false, paused = false, pingPending = false;
        uint32_t sequence = 0;
        uint64_t order = 0, pingStamp = 0;
        double rtt = 0;
        int sendErrors = 0;
    };
    static std::string endpoint(const sockaddr_in &peer) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
    }
    void text(const sockaddr_in &peer, const std::string &msg) {
        sendto(socket_, msg.data(), static_cast<int>(msg.size()), 0, (const sockaddr *)&peer, sizeof(peer));
    }
    uint64_t timestamp(const Peer &p) const {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - p.start).count();
    }
    void send(Peer &p, uint8_t type, const uint8_t *data, size_t len, uint64_t stamp = UINT64_MAX) {
        std::array<uint8_t, 2048> packet{};
        if (len > packet.size() - 16 || !p.dtls)
            return;
        packet[0] = 1;
        packet[1] = type;
        uint32_t seq = (type == 1 || type == 9) ? p.sequence++ : 0;
        uint64_t ts = stamp == UINT64_MAX ? timestamp(p) : stamp;
        uint16_t payload = static_cast<uint16_t>(len);
        memcpy(packet.data() + 2, &seq, 4);
        memcpy(packet.data() + 6, &ts, 8);
        memcpy(packet.data() + 14, &payload, 2);
        if (len)
            memcpy(packet.data() + 16, data, len);
        if (p.dtls->send(packet.data(), len + 16) != int(len + 16)) {
            if (++p.sendErrors >= 5)
                p.dead = true;
        } else
            p.sendErrors = 0;
    }
    void dismiss(Peer &p) {
        if (prompt_ == p.endpoint) {
            events_.push_back({Kind::Pair});
            prompt_.clear();
        }
    }
    void reject(Peer &p, const char *reason) {
        text(p.peer, std::string("AB_REJECT|") + reason);
        dismiss(p);
        p.dead = true;
    }
    void beginHandshake(Peer &p) {
        p.exchange.reset();
        p.dtls = std::make_unique<DtlsSession>();
        const auto id = p.id;
        const auto key = DtlsSession::hexToBytes(p.key.substr(3));
        if (!p.dtls->init(socket_, p.peer, [id, key](const std::string &identity, std::vector<uint8_t> &out) {
                if (identity != id)
                    return false;
                out = key;
                return true;
            })) {
            p.dead = true;
            return;
        }
        p.phase = Phase::Handshake;
        p.deadline = Clock::now() + std::chrono::seconds(10);
        p.retry = Clock::now() + std::chrono::milliseconds(500);
        text(p.peer, "AB_ACCEPT2");
    }
    void receive(const sockaddr_in &from, const uint8_t *bytes, size_t size) {
        const auto address = endpoint(from);
        auto it = peers_.find(address);
        if (bytes[0] >= 20 && bytes[0] <= 25) {
            if (it == peers_.end() || !it->second->dtls || it->second->dead)
                return;
            auto &p = *it->second;
            p.dtls->pushReceivedData(bytes, size);
            drive(p);
            return;
        }
        const std::string msg(reinterpret_cast<const char *>(bytes), size);
        if (msg.rfind("AB_KEY2|", 0) == 0) {
            if (it == peers_.end())
                return;
            auto &p = *it->second;
            if (p.phase != Phase::Reveal || p.dead)
                return;
            const auto pub = msg.substr(8);
            std::string code;
            if (SecurePairing::commitment(p.name, p.id, pub) != p.commitment ||
                !p.exchange->derive(pub, p.name, p.id, true, p.key, code)) {
                reject(p, "Invalid pairing key");
                return;
            }
            p.phase = Phase::Approval;
            p.token = p.id + ":" + p.commitment;
            prompt_ = p.endpoint;
            events_.push_back({Kind::Pair, p.token, p.name, "", code});
            return;
        }
        if (msg.rfind("AB_CONNECT|", 0) == 0) {
            text(from, "AB_REJECT|Encryption required: update both apps");
            return;
        }
        if (msg.rfind("AB_CONNECT2|", 0) != 0)
            return;
        std::istringstream input(msg);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(input, field, '|'))
            fields.push_back(field);
        if (fields.size() != 5 || !SecurePairing::validField(fields[1], 128) ||
            !SecurePairing::validField(fields[2], 128) || !SecurePairing::validKey(fields[3]) ||
            (fields[4] != "pair" && fields[4] != "resume"))
            return;
        // Retries never replace state, including an authenticated session.
        if (it != peers_.end()) {
            auto &p = *it->second;
            if (p.dead || p.id != fields[2] || p.commitment != fields[3])
                return;
            if (p.phase == Phase::Reveal)
                text(from, "AB_KEY2|" + p.exchange->publicKey());
            else if (p.phase == Phase::Handshake)
                text(from, "AB_ACCEPT2");
            return;
        }
        for (const auto &item : peers_)
            if (!item.second->dead && item.second->id == fields[2]) {
                text(from, "AB_REJECT|This device is already connected. Retry in a few seconds.");
                return;
            }
        if (peers_.size() >= maxClients) {
            text(from, "AB_REJECT|All 8 stream slots are in use");
            return;
        }
        std::string key = fields[4] == "resume" ? lookup_(fields[2]) : "";
        const bool pair = !SecurePairing::validStoredKey(key);
        if (pair)
            for (const auto &item : peers_) {
                if (!item.second->dead &&
                    (item.second->phase == Phase::Reveal || item.second->phase == Phase::Approval)) {
                    text(from, "AB_REJECT|Another device is pairing. Try again after approval.");
                    return;
                }
            }
        auto p = std::make_unique<Peer>();
        p->peer = from;
        p->endpoint = address;
        p->id = fields[2];
        p->name = fields[1];
        p->commitment = fields[3];
        p->key = key;
        p->deadline = Clock::now() + std::chrono::seconds(35);
        p->retry = Clock::now();
        if (pair) {
            p->exchange = std::make_unique<SecurePairing>();
            if (!p->exchange->generate())
                return;
        } else
            beginHandshake(*p);
        peers_.emplace(address, std::move(p));
    }
    void drive(Peer &p) {
        if (!p.dtls || p.dead)
            return;
        if (!p.dtls->isEstablished()) {
            int ret = p.dtls->continueHandshake();
            if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                p.dead = true;
            if (p.dead || !p.dtls->isEstablished())
                return;
        }
        for (int i = 0; i < 16 && !p.dead; ++i) {
            uint8_t plain[4096];
            int n = p.dtls->recv(plain, sizeof(plain));
            if (n < 0) {
                p.dead = true;
                break;
            }
            if (!n)
                break;
            if (n == 9 && memcmp(plain, "AB_READY2", 9) == 0) {
                if (p.dtls->send(plain, 9) != 9) {
                    p.dead = true;
                    break;
                }
                if (p.phase == Phase::Handshake) {
                    p.phase = Phase::Active;
                    p.order = ++order_;
                    p.start = p.lastPacket = Clock::now();
                    p.retry = p.start + std::chrono::milliseconds(500);
                    events_.push_back({Kind::Connected, p.id, p.name, p.key});
                    updateMicOwner();
                    sessionInfo();
                }
                continue;
            }
            if (p.phase != Phase::Active || n < 16 || plain[0] != 1)
                continue;
            uint16_t len;
            memcpy(&len, plain + 14, 2);
            if (n != 16 + len)
                continue;
            p.lastPacket = Clock::now();
            const uint8_t type = plain[1];
            if (type == 3 && len == 1) {
                uint8_t cmd = plain[16];
                if (cmd == 1 || cmd == 2) {
                    p.paused = cmd == 1;
                    events_.push_back({Kind::Pause});
                } else if (cmd == 3)
                    p.dead = true;
                else if (cmd >= 0x10 && cmd <= 0x12)
                    events_.push_back({Kind::Media, "", "", "", "", {cmd}});
                else if (cmd == 0x13)
                    events_.push_back({Kind::MediaInfo});
            } else if (type == 4 && len && len <= 2000 && micOwner_ == p.endpoint) {
                events_.push_back({Kind::Mic, "", "", "", "", std::vector<uint8_t>(plain + 16, plain + n)});
            } else if (type == 6 && len == 0) {
                uint64_t stamp;
                memcpy(&stamp, plain + 6, 8);
                send(p, 7, nullptr, 0, stamp);
            } else if (type == 7 && len == 0 && p.pingPending) {
                uint64_t stamp;
                memcpy(&stamp, plain + 6, 8);
                if (stamp == p.pingStamp) {
                    double ms = std::chrono::duration<double, std::milli>(Clock::now() - p.pingSent).count();
                    p.rtt = p.rtt == 0 ? ms : p.rtt * 0.7 + ms * 0.3;
                    p.pingPending = false;
                }
            }
        }
    }
    void updateMicOwner() {
        std::string owner;
        uint64_t oldest = UINT64_MAX;
        for (const auto &item : peers_) {
            const auto &p = *item.second;
            if (!p.dead && p.phase == Phase::Active && p.order < oldest) {
                oldest = p.order;
                owner = item.first;
            }
        }
        if (owner != micOwner_) {
            micOwner_ = owner;
            events_.push_back({Kind::MicReset});
        }
    }
    void sessionInfo() {
        uint8_t count = 0;
        for (const auto &item : peers_)
            if (!item.second->dead && item.second->phase == Phase::Active)
                ++count;
        for (auto &item : peers_) {
            auto &p = *item.second;
            if (!p.dead && p.phase == Phase::Active) {
                uint8_t payload[2] = {uint8_t(micOwner_ == p.endpoint), count};
                send(p, 0x0a, payload, 2);
            }
        }
    }
    void reap() {
        bool changed = false;
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto &p = *it->second;
            if (!p.dead) {
                ++it;
                continue;
            }
            dismiss(p);
            if (p.phase == Phase::Active) {
                events_.push_back({Kind::Disconnected, p.id, p.name});
                changed = true;
            }
            it = peers_.erase(it);
        }
        if (changed) {
            updateMicOwner();
            sessionInfo();
        }
    }
    DtlsSocket socket_;
    Lookup lookup_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Peer>> peers_;
    std::vector<Event> events_;
    std::string prompt_, micOwner_;
    uint64_t order_ = 0;
};
