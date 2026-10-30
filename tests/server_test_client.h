#pragma once
#include "network.h"
#include "secure_pairing.h"
#include <cassert>
#include <cstring>
#include <thread>
using Clock = std::chrono::steady_clock;

template <class Predicate> void eventually(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(predicate());
}

struct Client {
    SOCKET socket;
    sockaddr_in peer{};
    DtlsSession dtls;
    explicit Client(uint16_t port, const char *host = "127.0.0.1") {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(socket != INVALID_SOCKET);
        peer.sin_family = AF_INET;
        assert(inet_pton(AF_INET, host, &peer.sin_addr) == 1);
        peer.sin_port = htons(port);
        DWORD timeout = 20;
        assert(setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) == 0);
    }
    ~Client() {
        dtls.teardown();
        closesocket(socket);
    }
    void raw(const uint8_t *bytes, size_t size) {
        assert(sendto(socket, (const char *)bytes, static_cast<int>(size), 0, (sockaddr *)&peer,
                      sizeof(peer)) == static_cast<int>(size));
    }
    void text(const std::string &message) { raw((const uint8_t *)message.data(), message.size()); }
    std::string wire() {
        char buffer[4096];
        int n = recvfrom(socket, buffer, sizeof(buffer), 0, nullptr, nullptr);
        return n > 0 ? std::string(buffer, n) : std::string();
    }
    std::string response() {
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            auto msg = wire();
            if (!msg.empty())
                return msg;
        }
        assert(false && "Server response timed out");
        return {};
    }
    bool handshake(const std::string &id, const std::string &key) {
        assert(dtls.initClient(socket, peer, id, DtlsSession::hexToBytes(key.substr(3))));
        auto deadline = Clock::now() + std::chrono::seconds(6);
        while (Clock::now() < deadline) {
            int ret = dtls.continueHandshake();
            if (ret == 0)
                return true;
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                return false;
            auto packet = wire();
            if (!packet.empty() && uint8_t(packet[0]) >= 20 && uint8_t(packet[0]) <= 25)
                dtls.pushReceivedData((const uint8_t *)packet.data(), packet.size());
        }
        return false;
    }
    std::string decrypted() {
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            uint8_t plain[4096];
            int n = dtls.recv(plain, sizeof(plain));
            assert(n >= 0);
            if (n > 0)
                return std::string((const char *)plain, n);
            auto packet = wire();
            if (!packet.empty() && uint8_t(packet[0]) >= 20 && uint8_t(packet[0]) <= 25)
                dtls.pushReceivedData((const uint8_t *)packet.data(), packet.size());
        }
        assert(false && "Encrypted response timed out");
        return {};
    }
    std::string packet(uint8_t type) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            auto value = decrypted();
            if (value.size() >= 16 && uint8_t(value[1]) == type)
                return value;
        }
        assert(false && "Expected application packet missing");
        return {};
    }
    void keepalive() {
        uint8_t packet[16] = {1, 2};
        assert(dtls.send(packet, sizeof(packet)) == sizeof(packet));
    }
    void ready() {
        assert(dtls.send((const uint8_t *)"AB_READY2", 9) == 9);
        assert(decrypted() == "AB_READY2");
    }
    void control(uint8_t command) {
        uint8_t packet[17] = {1, PACKET_CONTROL};
        packet[14] = 1;
        packet[16] = command;
        assert(dtls.send(packet, sizeof(packet)) == sizeof(packet));
    }
};
