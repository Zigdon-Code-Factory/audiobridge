#include "server_test_client.h"
#include <array>
#include <atomic>
#include <iostream>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    Network server;
    const std::string prefix = "multistream-test-" + std::to_string(Clock::now().time_since_epoch().count());
    std::atomic<bool> autoApprove{true};
    std::atomic<int> micPackets{0}, resets{0};
    std::mutex pairMutex;
    std::string expectedCode, pendingToken;
    server.setCallbacks([](const std::string &) {}, [] {}, [](bool) {},
                        [&](const uint8_t *data, int size) {
                            if (!data && !size) {
                                ++resets;
                                return;
                            }
                            assert(size == 1 && data[0] == 42);
                            ++micPackets;
                        },
                        [&](const std::string &token, const std::string &, const std::string &code) {
                            if (token.empty())
                                return;
                            {
                                std::lock_guard<std::mutex> lock(pairMutex);
                                assert(code == expectedCode);
                                pendingToken = token;
                            }
                            if (autoApprove)
                                server.approvePeer(token);
                        });
    assert(server.initialize("Multi-stream test", 0, 0));
    std::vector<std::unique_ptr<Client>> phones;
    std::vector<std::string> keys;
    std::array<int16_t, 1920> audio{};
    for (size_t i = 0; i < audio.size(); ++i)
        audio[i] = int16_t((i * 137) % 32000 - 16000);
    std::map<Client *, uint32_t> nextSequence;
    auto verifyPcm = [&](Client &client, int frames) {
        int copied = 0;
        while (copied < frames) {
            auto packet = client.packet(9);
            assert(packet.size() <= 976);
            uint32_t sequence;
            memcpy(&sequence, packet.data() + 2, 4);
            assert(sequence == nextSequence[&client]++);
            const int count = int(packet.size() - 16) / 4;
            assert(count > 0 && copied + count <= frames);
            assert(memcmp(packet.data() + 16, audio.data() + copied * 2, size_t(count) * 4) == 0);
            copied += count;
        }
    };
    auto startPair = [&](Client &client, const std::string &id) {
        SecurePairing exchange;
        assert(exchange.generate());
        client.text("AB_CONNECT2|" + id + "|" + id + "|" +
                    SecurePairing::commitment(id, id, exchange.publicKey()) + "|pair");
        auto reply = client.response();
        assert(reply.rfind("AB_KEY2|", 0) == 0);
        std::string key, code;
        assert(exchange.derive(reply.substr(8), id, id, false, key, code));
        {
            std::lock_guard<std::mutex> lock(pairMutex);
            expectedCode = code;
            pendingToken.clear();
        }
        client.text("AB_KEY2|" + exchange.publicKey());
        return key;
    };
    for (int i = 0; i < 8; ++i) {
        auto phone = std::make_unique<Client>(server.getStreamPort());
        const auto id = prefix + "-" + std::to_string(i);
        if (i == 1)
            autoApprove = false;
        auto key = startPair(*phone, id);
        if (i == 1) {
            eventually([&] {
                std::lock_guard<std::mutex> lock(pairMutex);
                return !pendingToken.empty();
            });
            // Approval can remain open while the first phone receives exact PCM.
            for (int j = 0; j < 20; ++j) {
                server.sendAudioPcm(audio.data(), 120);
                verifyPcm(*phones[0], 120);
                phones[0]->keepalive();
            }
            std::string token;
            {
                std::lock_guard<std::mutex> lock(pairMutex);
                token = pendingToken;
            }
            server.approvePeer(token);
            autoApprove = true;
        }
        assert(phone->response() == "AB_ACCEPT2");
        assert(phone->handshake(id, key));
        phone->ready();
        eventually([&] {
            return server.getClientCount() == size_t(i + 1) && server.getApprovedPeers().count(id) > 0;
        });
        for (const auto &old : keys)
            assert(key != old);
        keys.push_back(key);
        phones.push_back(std::move(phone));
        for (auto &p : phones)
            p->keepalive();
    }
    assert(server.getClientCount() == 8);
    {
        Client ninth(server.getStreamPort());
        ninth.text("AB_CONNECT2|Ninth|ninth|" + std::string(64, '0') + "|pair");
        assert(ninth.response() == "AB_REJECT|All 8 stream slots are in use");
    }
    for (size_t i = 0; i < phones.size(); ++i) {
        std::string status;
        do {
            status = phones[i]->packet(0x0a);
        } while (uint8_t(status[17]) != 8);
        assert(status.size() == 18 && bool(status[16]) == (i == 0));
    }
    // PCM 2.5/5/10/20 ms, including the previously oversized 20 ms payload.
    for (int frames : {120, 240, 480, 960}) {
        server.sendAudioPcm(audio.data(), frames);
        for (auto &phone : phones) {
            verifyPcm(*phone, frames);
            phone->keepalive();
        }
    }
    const uint8_t opus[] = {0x12, 0x34, 0x56};
    server.sendAudio(opus, sizeof(opus));
    for (auto &phone : phones) {
        auto p = phone->packet(1);
        assert(p.substr(16) == std::string((const char *)opus, 3));
        uint32_t sequence;
        memcpy(&sequence, p.data() + 2, 4);
        assert(sequence == nextSequence[phone.get()]++);
    }
    server.sendMediaInfo("same private track");
    for (auto &phone : phones)
        assert(phone->packet(5).substr(16) == "same private track");
    phones[0]->control(CTRL_PAUSE);
    // Each pause is independent; the capture stays active for the other phones.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!server.isPaused());
    server.sendAudioPcm(audio.data(), 120);
    for (size_t i = 1; i < phones.size(); ++i)
        verifyPcm(*phones[i], 120);
    const auto pausedUntil = Clock::now() + std::chrono::milliseconds(150);
    while (Clock::now() < pausedUntil) {
        auto wire = phones[0]->wire();
        if (!wire.empty() && uint8_t(wire[0]) >= 20 && uint8_t(wire[0]) <= 25)
            phones[0]->dtls.pushReceivedData((const uint8_t *)wire.data(), wire.size());
        uint8_t plain[4096];
        int count = phones[0]->dtls.recv(plain, sizeof(plain));
        assert(count >= 0);
        assert(count < 16 || (plain[1] != 1 && plain[1] != 9));
    }
    // Mic return stays with the first phone; never interleave independent Opus streams.
    uint8_t mic[17] = {1, 4};
    mic[14] = 1;
    mic[16] = 42;
    assert(phones[1]->dtls.send(mic, sizeof(mic)) == sizeof(mic));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(micPackets == 0);
    assert(phones[0]->dtls.send(mic, sizeof(mic)) == sizeof(mic));
    eventually([&] { return micPackets == 1; });
    phones[0]->control(CTRL_RESUME);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.sendAudioPcm(audio.data(), 120);
    for (auto &p : phones)
        verifyPcm(*p, 120);
    phones[7]->control(CTRL_DISCONNECT);
    eventually([&] { return server.getClientCount() == 7; });
    phones.pop_back();
    {
        Client bad(server.getStreamPort());
        bad.text("AB_CONNECT2|Wrong key|" + prefix + "-7|" + std::string(64, '0') + "|resume");
        assert(bad.response() == "AB_ACCEPT2");
        assert(!bad.handshake(prefix + "-7", "v2:" + std::string(64, '0')));
        assert(server.getClientCount() == 7);
        server.sendAudioPcm(audio.data(), 120);
        for (auto &phone : phones)
            verifyPcm(*phone, 120);
    }
    server.revokePeer(prefix + "-6");
    eventually([&] { return server.getClientCount() == 6; });
    phones.pop_back();
    // One stalled client expires while all remaining clients keep receiving audio.
    const auto until = Clock::now() + std::chrono::milliseconds(5300);
    while (Clock::now() < until) {
        server.sendAudioPcm(audio.data(), 120);
        for (size_t i = 0; i + 1 < phones.size(); ++i) {
            verifyPcm(*phones[i], 120);
            phones[i]->keepalive();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    eventually([&] { return server.getClientCount() == 5; });
    phones.pop_back();
    phones[0]->control(CTRL_DISCONNECT);
    eventually([&] { return server.getClientCount() == 4; });
    eventually([&] { return resets >= 2; });
    assert(server.getMicOwnerName() == prefix + "-1");
    assert(phones[1]->dtls.send(mic, sizeof(mic)) == sizeof(mic));
    eventually([&] { return micPackets == 2; });
    for (int i = 0; i < 8; ++i)
        server.revokePeer(prefix + "-" + std::to_string(i));
    server.shutdown();
    phones.clear();
    WSACleanup();
    puts("PASS eight independent encrypted streams, live pairing, PCM sizes, pause, mic ownership, "
         "disconnect, revoke and timeout");
}
