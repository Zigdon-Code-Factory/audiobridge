#include "network.h"
#include "secure_pairing.h"
#include <cassert>
#include <cstring>
#include <thread>

using Clock = std::chrono::steady_clock;

template <class Predicate> void eventually(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(predicate());
}

struct Client {
    SOCKET socket;
    sockaddr_in peer{};
    DtlsSession dtls;
    explicit Client(uint16_t port) {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(socket != INVALID_SOCKET);
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer.sin_port = htons(port);
        DWORD timeout = 20;
        assert(setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) == 0);
    }
    ~Client() { dtls.teardown(); closesocket(socket); }
    void raw(const uint8_t* bytes, size_t size) {
        assert(sendto(socket, (const char*)bytes, static_cast<int>(size), 0,
                      (sockaddr*)&peer, sizeof(peer)) == static_cast<int>(size));
    }
    void text(const std::string& message) { raw((const uint8_t*)message.data(), message.size()); }
    std::string wire() {
        char buffer[4096];
        int n = recvfrom(socket, buffer, sizeof(buffer), 0, nullptr, nullptr);
        return n > 0 ? std::string(buffer, n) : std::string();
    }
    std::string response() {
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) { auto msg = wire(); if (!msg.empty()) return msg; }
        assert(false && "Server response timed out");
        return {};
    }
    bool handshake(const std::string& id, const std::string& key) {
        assert(dtls.initClient(socket, peer, id, DtlsSession::hexToBytes(key.substr(3))));
        auto deadline = Clock::now() + std::chrono::seconds(6);
        while (Clock::now() < deadline) {
            int ret = dtls.continueHandshake();
            if (ret == 0) return true;
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
            auto packet = wire();
            if (!packet.empty() && uint8_t(packet[0]) >= 20 && uint8_t(packet[0]) <= 25)
                dtls.pushReceivedData((const uint8_t*)packet.data(), packet.size());
        }
        return false;
    }
    std::string decrypted() {
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            uint8_t plain[4096];
            int n = dtls.recv(plain, sizeof(plain));
            assert(n >= 0);
            if (n > 0) return std::string((const char*)plain, n);
            auto packet = wire();
            if (!packet.empty() && uint8_t(packet[0]) >= 20 && uint8_t(packet[0]) <= 25)
                dtls.pushReceivedData((const uint8_t*)packet.data(), packet.size());
        }
        assert(false && "Encrypted response timed out");
        return {};
    }
    void ready() {
        assert(dtls.send((const uint8_t*)"AB_READY2", 9) == 9);
        assert(decrypted() == "AB_READY2");
    }
    void control(uint8_t command) {
        uint8_t packet[17] = {1, PACKET_CONTROL};
        packet[14] = 1;
        packet[16] = command;
        assert(dtls.send(packet, sizeof(packet)) == sizeof(packet));
    }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    Network server;
    std::atomic<int> requests{0}, connections{0}, micPackets{0};
    std::atomic<bool> approve{false};
    std::string expectedCode;
    std::mutex codeMutex;
    server.setCallbacks([&](const std::string&) { ++connections; }, [] {}, [](bool) {},
        [&](const uint8_t* data, int len) { assert(len == 3 && data[0] == 42); ++micPackets; },
        [&](const std::string& token, const std::string&, const std::string& code) {
            if (token.empty()) return;
            ++requests;
            { std::lock_guard<std::mutex> lock(codeMutex); assert(code == expectedCode); }
            if (approve) server.approvePeer(token); else server.rejectPeer(token);
        });
    // Ephemeral ports avoid interacting with the user's running AudioBridge.
    assert(server.initialize("AudioBridge security test", 0, 0));
    const std::string id = "security-test-" + std::to_string(Clock::now().time_since_epoch().count());
    auto pair = [&](Client& client) {
        SecurePairing exchange;
        assert(exchange.generate());
        client.text("AB_CONNECT2|Phone|" + id + "|" + SecurePairing::commitment("Phone", id, exchange.publicKey()) + "|pair");
        const auto challenge = client.response();
        assert(challenge.rfind("AB_KEY2|", 0) == 0);
        std::string key, code;
        assert(exchange.derive(challenge.substr(8), "Phone", id, false, key, code));
        { std::lock_guard<std::mutex> lock(codeMutex); expectedCode = code; }
        client.text("AB_KEY2|" + exchange.publicKey());
        const auto response = client.response();
        assert(response.find(key.substr(3)) == std::string::npos);
        if (!approve) {
            assert(response.rfind("AB_REJECT|", 0) == 0);
            assert(!server.isConnected() && server.getApprovedPeers().count(id) == 0);
        } else {
            assert(response == "AB_ACCEPT2");
            assert(client.handshake(id, key));
            assert(!server.isConnected()); // No audio before encrypted READY.
            client.ready();
            eventually([&] { return server.isConnected(); });
        }
        return key;
    };
    { Client legacy(server.getStreamPort());
      legacy.text("AB_CONNECT|Legacy|id");
      assert(legacy.response().rfind("AB_REJECT|", 0) == 0);
      assert(!server.isConnected()); }
    { Client denied(server.getStreamPort()); pair(denied); }
    approve = true;
    std::string key;
    {
        Client client(server.getStreamPort());
        key = pair(client);
        assert(server.getApprovedPeers().at(id).pskHex == key);
        // A second phone gets an actionable busy response, not a silent timeout.
        // Its unauthenticated request must not replace or pause the active stream.
        {
            Client second(server.getStreamPort());
            second.text("AB_CONNECT2|Second phone|second-id|" + std::string(64, '0') + "|pair");
            assert(second.response() == "AB_REJECT|Server already connected. Disconnect the other device first.");
            assert(server.isConnected() && !server.isPaused() && connections == 1);
        }
        // Spoofed raw controls from the actual endpoint must still be rejected.
        uint8_t rawControl[17] = {1, PACKET_CONTROL};
        rawControl[14] = 1; rawControl[16] = CTRL_PAUSE;
        client.raw(rawControl, sizeof(rawControl));
        client.text("AB_DISCONNECT");
        client.text("AB_CONNECT|Attacker|id");
        client.text("AB_CONNECT2|Attacker|id|" + std::string(64, '0') + "|pair");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(server.isConnected() && !server.isPaused());
        assert(requests == 2);
        client.control(CTRL_PAUSE);
        eventually([&] { return server.isPaused(); });
        uint8_t mic[19] = {1, PACKET_MIC_AUDIO};
        mic[14] = 3; mic[16] = 42;
        client.raw(mic, sizeof(mic));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(micPackets == 0);
        assert(client.dtls.send(mic, sizeof(mic)) == sizeof(mic));
        eventually([&] { return micPackets == 1; });
        server.sendMediaInfo("private track title");
        auto metadata = client.decrypted();
        assert(metadata.size() > 16 && uint8_t(metadata[1]) == PACKET_MEDIA_INFO);
        assert(metadata.substr(16) == "private track title");
        client.control(CTRL_DISCONNECT);
        eventually([&] { return !server.isConnected(); });
    }
    {
        Client resumed(server.getStreamPort());
        resumed.text("AB_CONNECT2|Phone|" + id + "|" + std::string(64, '0') + "|resume");
        assert(resumed.response() == "AB_ACCEPT2"); // No key transmitted on reconnect.
        assert(resumed.handshake(id, key));
        resumed.ready();
        eventually([&] { return connections == 2; });
        assert(requests == 2);
        resumed.control(CTRL_DISCONNECT);
        eventually([&] { return !server.isConnected(); });
    }
    {
        Client impostor(server.getStreamPort());
        impostor.text("AB_CONNECT2|Phone|" + id + "|" + std::string(64, '0') + "|resume");
        assert(impostor.response() == "AB_ACCEPT2");
        assert(!impostor.handshake(id, "v2:" + std::string(64, '0')));
        assert(!server.isConnected() && connections == 2);
    }
    server.revokePeer(id);
    server.shutdown();
    WSACleanup();
    puts("Server pairing, reconnect, and plaintext rejection tests passed");
}
