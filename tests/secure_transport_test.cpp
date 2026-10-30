#include "secure_pairing.h"
#include <cassert>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

struct Endpoint {
    DtlsSocket socket;
    sockaddr_in addr{};
    Endpoint() {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(socket != static_cast<DtlsSocket>(-1));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
#ifdef _WIN32
        int size = sizeof(addr);
        u_long nonblock = 1;
        assert(ioctlsocket(socket, FIONBIO, &nonblock) == 0);
#else
        socklen_t size = sizeof(addr);
        assert(fcntl(socket, F_SETFL, O_NONBLOCK) == 0);
#endif
        assert(getsockname(socket, reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    }
    ~Endpoint() {
#ifdef _WIN32
        closesocket(socket);
#else
        close(socket);
#endif
    }
    std::vector<uint8_t> take() {
        uint8_t data[8192];
        int n = recvfrom(socket, reinterpret_cast<char*>(data), sizeof(data), 0, nullptr, nullptr);
        return n > 0 ? std::vector<uint8_t>(data, data + n) : std::vector<uint8_t>();
    }
    void feed(DtlsSession& session, bool& drop) {
        for (;;) {
            auto packet = take();
            if (packet.empty()) break;
            if (drop) { drop = false; continue; }
            session.pushReceivedData(packet.data(), packet.size());
        }
    }
};

struct Connection {
    Endpoint clientSocket, serverSocket;
    DtlsSession client, server;
    bool handshake(const std::vector<uint8_t>& clientKey, const std::vector<uint8_t>& serverKey,
                   const std::string& identity = "phone", bool losePacket = false) {
        assert(server.init(serverSocket.socket, clientSocket.addr,
            [serverKey](const std::string& id, std::vector<uint8_t>& key) {
                if (id != "phone") return false;
                key = serverKey;
                return true;
            }));
        assert(client.initClient(clientSocket.socket, serverSocket.addr, identity, clientKey));
        bool dropClientFlight = losePacket, noDrop = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto* session : {&client, &server}) {
                if (session->isEstablished()) continue;
                int ret = session->continueHandshake();
                if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
            }
            clientSocket.feed(client, noDrop);
            serverSocket.feed(server, dropClientFlight);
            if (client.isEstablished() && server.isEstablished()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

static void pairingTests() {
    SecurePairing client, server;
    assert(client.generate() && server.generate());
    std::string ck, sk, cc, sc;
    const auto commit = SecurePairing::commitment("Phone", "phone", client.publicKey());
    assert(commit.size() == 64);
    assert(commit != SecurePairing::commitment("Other name", "phone", client.publicKey()));
    assert(commit != SecurePairing::commitment("Phone", "other id", client.publicKey()));
    assert(commit != SecurePairing::commitment("Phone", "phone", server.publicKey()));
    assert(client.derive(server.publicKey(), "Phone", "phone", false, ck, cc));
    assert(server.derive(client.publicKey(), "Phone", "phone", true, sk, sc));
    assert(ck == sk && cc == sc && cc.size() == 6);
    assert(SecurePairing::validStoredKey(ck));
    assert(!SecurePairing::validStoredKey(ck.substr(3))); // Exposed v1 keys cannot resume.
    assert(!SecurePairing::validKey(std::string(64, 'z')));
    assert(DtlsSession::hexToBytes("0").empty());
    assert(DtlsSession::hexToBytes("0z").empty());
    assert(!SecurePairing::validField("Phone|injected", 128));
    assert(!SecurePairing::validField(std::string("a\0b", 3), 128));
    SecurePairing invalid;
    assert(invalid.generate());
    assert(!invalid.derive(std::string(64, '0'), "Phone", "phone", false, ck, cc));
    assert(ck.empty() && cc.empty());
}

static std::vector<uint8_t> receiveWire(Endpoint& socket) {
    for (int i = 0; i < 100; ++i) {
        auto packet = socket.take();
        if (!packet.empty()) return packet;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Expected UDP datagram");
    return {};
}

static void transportTests() {
    const std::vector<uint8_t> key(32, 0x42);
    std::vector<uint8_t> oldRecord;
    {
        Connection pair;
        assert(pair.handshake(key, key, "phone", true)); // Retransmit a lost ClientHello.
        // All application types use the same authenticated, replay-protected path.
        for (uint8_t type = 1; type <= 9; ++type) {
            std::vector<uint8_t> plain(496, 0x5a);
            plain[0] = 1; plain[1] = type;
            auto& sender = type == 4 ? pair.client : pair.server;
            auto& receiver = type == 4 ? pair.server : pair.client;
            auto& socket = type == 4 ? pair.serverSocket : pair.clientSocket;
            assert(sender.send(plain.data(), plain.size()) == static_cast<int>(plain.size()));
            auto wire = receiveWire(socket);
            assert(wire[0] == 23);
            assert(std::search(wire.begin(), wire.end(), plain.begin(), plain.end()) == wire.end());
            uint8_t output[4096];
            auto corrupt = wire;
            corrupt.back() ^= 1;
            receiver.pushReceivedData(corrupt.data(), corrupt.size());
            assert(receiver.recv(output, sizeof(output)) <= 0);
            receiver.pushReceivedData(wire.data(), wire.size());
            assert(receiver.recv(output, sizeof(output)) == static_cast<int>(plain.size()));
            assert(std::memcmp(output, plain.data(), plain.size()) == 0);
            receiver.pushReceivedData(wire.data(), wire.size());
            assert(receiver.recv(output, sizeof(output)) == 0); // Duplicate rejected.
            if (type == 4) oldRecord = wire;
        }
        // Out-of-order records within the DTLS replay window are valid once.
        const uint8_t first[] = {1, 2, 3}, second[] = {4, 5, 6};
        assert(pair.client.send(first, 3) == 3);
        auto a = receiveWire(pair.serverSocket);
        assert(pair.client.send(second, 3) == 3);
        auto b = receiveWire(pair.serverSocket);
        pair.server.pushReceivedData(b.data(), b.size());
        pair.server.pushReceivedData(a.data(), a.size());
        uint8_t output[4096];
        assert(pair.server.recv(output, sizeof(output)) == 3 && output[0] == 4);
        assert(pair.server.recv(output, sizeof(output)) == 3 && output[0] == 1);
        const uint8_t raw[] = {1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 3};
        pair.server.pushReceivedData(raw, sizeof(raw));
        assert(pair.server.recv(output, sizeof(output)) <= 0);
    }
    {
        Connection reconnect;
        assert(reconnect.handshake(key, key));
        uint8_t output[4096];
        reconnect.server.pushReceivedData(oldRecord.data(), oldRecord.size());
        assert(reconnect.server.recv(output, sizeof(output)) <= 0); // Prior session rejected.
        const uint8_t live[] = {7, 8, 9};
        assert(reconnect.client.send(live, 3) == 3);
        auto wire = receiveWire(reconnect.serverSocket);
        reconnect.server.pushReceivedData(wire.data(), wire.size());
        assert(reconnect.server.recv(output, sizeof(output)) == 3);
    }
    { Connection wrongKey; assert(!wrongKey.handshake(std::vector<uint8_t>(32, 0x43), key)); }
    { Connection wrongId; assert(!wrongId.handshake(key, key, "impostor")); }
    DtlsSession uninitialized;
    assert(uninitialized.send(key.data(), key.size()) < 0);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    WSADATA data;
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    pairingTests();
    transportTests();
#ifdef _WIN32
    WSACleanup();
#endif
    puts("Secure pairing and DTLS transport tests passed");
}
