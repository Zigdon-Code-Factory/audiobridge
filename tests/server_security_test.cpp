#include "network.h"
#include "secure_pairing.h"
#include <cassert>
#include <cstring>
#include <thread>

#include "server_test_client.h"

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
        [&](const uint8_t* data, int len) { if (!data && !len) return; assert(len == 3 && data[0] == 42); ++micPackets; },
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
        // A pending second pairing must not block the authenticated stream.
        // Its unauthenticated request must not replace or pause the active stream.
        {
            Client second(server.getStreamPort());
            second.text("AB_CONNECT2|Second phone|second-id|" + std::string(64, '0') + "|pair");
            assert(second.response().rfind("AB_KEY2|", 0) == 0);
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
        auto metadata = client.packet(PACKET_MEDIA_INFO);
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
