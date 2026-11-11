#include "server_test_client.h"
#include "remote_control.h"
#include <filesystem>
#include <fstream>
#include <ctime>

static void command(Client& client, uint32_t id, uint32_t operation, int32_t value, size_t size = 16) {
    uint8_t bytes[48] = {1, RemoteControl::requestType};
    bytes[14] = static_cast<uint8_t>(size);
    const uint32_t fields[] = {id, 1, operation, static_cast<uint32_t>(value)};
    for (size_t field = 0; field < 4; ++field)
        for (size_t byte = 0; byte < 4; ++byte)
            bytes[16 + field * 4 + byte] = uint8_t(fields[field] >> (byte * 8));
    assert(client.dtls.send(bytes, 16 + size) == int(16 + size));
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA sockets;
    assert(WSAStartup(MAKEWORD(2, 2), &sockets) == 0);
    uint8_t bytes[16]{};
    RemoteControl::Request decoded;
    assert(!RemoteControl::decode(bytes, 15, decoded));
    assert(RemoteControl::decode(bytes, 16, decoded) && RemoteControl::valid(decoded));
    for (uint32_t operation = 0; operation < 32; ++operation) {
        const bool allowed = operation <= 12 || operation == 16;
        auto request = RemoteControl::Request{1, 1, operation, operation == 2 ? 5 : operation == 10 ? 50 : operation == 11 ? 300 : operation == 16 ? 0x10 : 0};
        assert(RemoteControl::valid(request) == allowed);
        request.value = -1;
        assert(!RemoteControl::valid(request));
        request.value = INT32_MAX;
        assert(!RemoteControl::valid(request));
    }
    assert(!RemoteControl::storedAdmin("") && !RemoteControl::storedAdmin("Admin"));
    assert(!RemoteControl::storedAdmin("admin|anything") && RemoteControl::storedAdmin("admin"));
    char executable[MAX_PATH]{};
    GetModuleFileNameA(nullptr, executable, MAX_PATH);
    const auto file = std::filesystem::path(executable).parent_path() / "approved_peers.txt";
    const std::string legacyKey = "v2:" + std::string(64, 'a');
    {
        std::ofstream seed(file);
        seed << "legacy|Legacy phone|" << std::time(nullptr) << "|" << legacyKey << "\n";
        seed << "invalid-role|Other phone|" << std::time(nullptr) << "|" << legacyKey << "|owner\n";
        seed << "persisted-admin|Admin phone|" << std::time(nullptr) << "|" << legacyKey << "|admin\n";
    }
    const std::string id = "role-test-phone";
    std::string key;
    Network server;
    std::atomic<int> connected{0};
    server.setCallbacks([&](const std::string&) { ++connected; }, [] {}, [](bool) {}, {},
        [&](const std::string& token, const std::string&, const std::string&) { if (!token.empty()) server.approvePeer(token); });
    assert(server.initialize("Permission test", 0, 0));
    assert(!server.getApprovedPeers().at("legacy").admin);
    assert(!server.getApprovedPeers().at("invalid-role").admin);
    assert(server.getApprovedPeers().at("persisted-admin").admin);
    int applied = 0;
    auto apply = [&](const RemoteControl::Request& request) -> std::string {
        assert(request.command == 1 || request.command == 16);
        ++applied;
        return "";
    };
    {
        Client phone(server.getStreamPort());
        SecurePairing pairing;
        assert(pairing.generate());
        phone.text("AB_CONNECT2|Test phone|" + id + "|" + SecurePairing::commitment("Test phone", id, pairing.publicKey()) + "|pair");
        const auto challenge = phone.response();
        std::string code;
        assert(challenge.rfind("AB_KEY2|", 0) == 0);
        assert(pairing.derive(challenge.substr(8), "Test phone", id, false, key, code));
        phone.text("AB_KEY2|" + pairing.publicKey());
        assert(phone.response() == "AB_ACCEPT2");
        assert(phone.handshake(id, key));
        phone.ready();
        eventually([&] { return connected == 1; });
        assert(!server.getApprovedPeers().at(id).admin);
        server.sendControlState("{\"revision\":1,\"jitter\":10}");
        auto state = phone.packet(RemoteControl::stateType).substr(16);
        assert(state.find("\"admin\":false") != std::string::npos);
        assert(state.find(key) == std::string::npos && state.find("peer") == std::string::npos);
        command(phone, 1, 1, 20);
        assert(phone.packet(RemoteControl::resultType).find("Read only") != std::string::npos);
        phone.control(CTRL_MEDIA_NEXT);
        assert(phone.packet(RemoteControl::resultType).find("Read only") != std::string::npos);
        server.processControls(apply);
        assert(applied == 0);
        const auto blockedFile = std::filesystem::path(file.string() + ".tmp");
        assert(std::filesystem::create_directory(blockedFile));
        assert(!server.setPeerAdmin(id, true));
        assert(!server.getApprovedPeers().at(id).admin);
        assert(std::filesystem::remove(blockedFile));
        assert(server.setPeerAdmin(id, true));
        assert(!server.setPeerAdmin("missing", true));
        {
            Client reader(server.getStreamPort());
            reader.text("AB_CONNECT2|Test phone|legacy|" + std::string(64, '0') + "|resume");
            assert(reader.response() == "AB_ACCEPT2" && reader.handshake("legacy", legacyKey));
            reader.ready();
            eventually([&] { return server.getClientCount() == 2; });
            command(reader, 100, 1, 30);
            assert(reader.packet(RemoteControl::resultType).find("Read only") != std::string::npos);
            command(reader, 101, 99, 1);
            assert(reader.packet(RemoteControl::resultType).find("Invalid command") != std::string::npos);
            assert(!server.getApprovedPeers().at("legacy").admin && server.getApprovedPeers().at(id).admin);
            server.sendControlState("{\"revision\":1,\"jitter\":10}");
            assert(reader.packet(RemoteControl::stateType).find("\"admin\":false") != std::string::npos);
            reader.control(CTRL_DISCONNECT);
            eventually([&] { return server.getClientCount() == 1; });
        }
        command(phone, 2, 1, 20);
        eventually([&] { server.processControls(apply); return applied == 1; });
        assert(phone.packet(RemoteControl::resultType).find("\"ok\":true") != std::string::npos);
        command(phone, 3, 99, 1);
        assert(phone.packet(RemoteControl::resultType).find("Invalid command") != std::string::npos);
        command(phone, 4, 1, 151);
        assert(phone.packet(RemoteControl::resultType).find("Invalid command") != std::string::npos);
        command(phone, 5, 1, 20, 20);
        assert(phone.packet(RemoteControl::resultType).find("Invalid request size") != std::string::npos);
        assert(!server.getApprovedPeers().at("legacy").admin);
        command(phone, 6, 1, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(server.setPeerAdmin(id, false));
        assert(server.setPeerAdmin(id, true));
        server.processControls(apply);
        assert(applied == 1);
        assert(phone.packet(RemoteControl::resultType).find("Permission changed") != std::string::npos);
        assert(server.setPeerAdmin(id, false));
        server.sendControlState("{\"revision\":1,\"jitter\":10}");
        assert(phone.packet(RemoteControl::stateType).find("\"admin\":false") != std::string::npos);
        phone.control(CTRL_PAUSE);
        eventually([&] { return server.isPaused(); });
        phone.control(CTRL_RESUME);
        eventually([&] { return !server.isPaused(); });
        const int16_t pcm[4] = {11, 22, 33, 44};
        server.sendAudioPcm(pcm, 2);
        assert(phone.packet(PACKET_AUDIO_PCM).size() == 24);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(server.setPeerAdmin(id, true));
        phone.control(CTRL_MEDIA_NEXT);
        eventually([&] { server.processControls(apply); return applied == 2; });
        assert(phone.packet(RemoteControl::resultType).find("\"ok\":true") != std::string::npos);
        command(phone, 7, 1, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.processControls([](const RemoteControl::Request&) { return "Device failed"; });
        assert(phone.packet(RemoteControl::resultType).find("\"ok\":false") != std::string::npos);
        phone.keepalive();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        for (uint32_t request = 10; request < 22; ++request) command(phone, request, 1, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int beforeLimit = applied;
        server.processControls(apply);
        assert(applied - beforeLimit == 10);
        phone.control(CTRL_DISCONNECT);
        eventually([&] { return !server.isConnected(); });
    }
    server.shutdown();
    {
        Network restored;
        assert(restored.initialize("Restore test", 0, 0));
        assert(restored.getApprovedPeers().at(id).admin);
        assert(restored.getApprovedPeers().at(id).pskHex == key);
        assert(!restored.getApprovedPeers().at("legacy").admin);
        Client phone(restored.getStreamPort());
        phone.text("AB_CONNECT2|Renamed phone|" + id + "|" + std::string(64, '0') + "|resume");
        assert(phone.response() == "AB_ACCEPT2" && phone.handshake(id, key));
        phone.ready();
        eventually([&] { return restored.isConnected(); });
        command(phone, 8, 1, 40);
        eventually([&] { restored.processControls(apply); return applied == 13; });
        assert(phone.packet(RemoteControl::resultType).find("\"ok\":true") != std::string::npos);
        const auto blockedFile = std::filesystem::path(file.string() + ".tmp");
        assert(std::filesystem::create_directory(blockedFile));
        assert(!restored.revokePeer(id));
        assert(restored.getApprovedPeers().at(id).admin);
        assert(restored.getApprovedPeers().at(id).pskHex == key);
        assert(std::filesystem::remove(blockedFile));
        assert(restored.setPeerAdmin(id, false));
        assert(restored.revokePeer(id));
        restored.shutdown();
    }
    {
        Network restored;
        assert(restored.initialize("Revocation test", 0, 0));
        assert(!restored.getApprovedPeers().count(id));
        assert(!restored.getApprovedPeers().at("legacy").admin);
        restored.shutdown();
    }
    std::filesystem::remove(file);
    WSACleanup();
    puts("Peer permission protocol, read-only migration, authorization, connected downgrade and restore passed");
}
