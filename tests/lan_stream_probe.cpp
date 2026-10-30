#include "server_test_client.h"
#include <cstdio>

// Manual LAN receiver: no audio recording, no saved key. Compare the printed
// code with the desktop approval prompt before approving this temporary client.
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 4) {
        puts("Usage: lan_stream_probe <server IPv4> <port> <seconds>");
        return 2;
    }
    const int seconds = atoi(argv[3]);
    if (seconds < 10 || seconds > 600)
        return 2;
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    int result = 1;
    {
        Client client(uint16_t(atoi(argv[2])), argv[1]);
        const std::string id = "lan-probe-" + std::to_string(Clock::now().time_since_epoch().count());
        const std::string name = "LAN test receiver";
        SecurePairing exchange;
        assert(exchange.generate());
        client.text("AB_CONNECT2|" + name + "|" + id + "|" +
                    SecurePairing::commitment(name, id, exchange.publicKey()) + "|pair");
        auto reply = client.response();
        assert(reply.rfind("AB_KEY2|", 0) == 0);
        std::string key, code;
        assert(exchange.derive(reply.substr(8), name, id, false, key, code));
        printf("PAIR_CODE=%s ID=%s\n", code.c_str(), id.c_str());
        client.text("AB_KEY2|" + exchange.publicKey());
        auto deadline = Clock::now() + std::chrono::seconds(32);
        bool accepted = false;
        while (Clock::now() < deadline) {
            auto wire = client.wire();
            if (wire == "AB_ACCEPT2") {
                accepted = true;
                break;
            }
            if (wire.rfind("AB_REJECT|", 0) == 0) {
                puts(wire.c_str());
                break;
            }
        }
        assert(accepted && client.handshake(id, key));
        client.ready();
        auto start = Clock::now(), keepalive = start, nextReport = start + std::chrono::seconds(30);
        uint64_t packets = 0, missing = 0, nonzero = 0;
        uint32_t previous = 0;
        bool seen = false;
        int clients = 0;
        while (Clock::now() - start < std::chrono::seconds(seconds)) {
            auto now = Clock::now();
            if (now >= keepalive) {
                client.keepalive();
                keepalive = now + std::chrono::milliseconds(500);
            }
            auto wire = client.wire();
            if (!wire.empty() && uint8_t(wire[0]) >= 20 && uint8_t(wire[0]) <= 25)
                client.dtls.pushReceivedData((const uint8_t *)wire.data(), wire.size());
            for (int i = 0; i < 32; ++i) {
                uint8_t plain[4096];
                int n = client.dtls.recv(plain, sizeof(plain));
                assert(n >= 0);
                if (!n)
                    break;
                if (n < 16)
                    continue;
                if (plain[1] == 3 && n == 17 && plain[16] == 3) {
                    puts("Unexpected server disconnect");
                    return 1;
                }
                if (plain[1] == 6) {
                    plain[1] = 7;
                    assert(client.dtls.send(plain, n) == n);
                }
                if (plain[1] == 0x0a && n == 18)
                    clients = plain[17];
                if (plain[1] == 1 || plain[1] == 9) {
                    uint32_t seq;
                    memcpy(&seq, plain + 2, 4);
                    if (seen && seq > previous + 1)
                        missing += seq - previous - 1;
                    previous = seq;
                    seen = true;
                    ++packets;
                    bool signal = false;
                    for (int j = 16; j < n; ++j)
                        signal |= plain[j] != 0;
                    if (signal)
                        ++nonzero;
                }
            }
            if (now >= nextReport) {
                printf("SOAK packets=%llu nonzero=%llu missed=%llu clients=%d\n", packets, nonzero, missing,
                       clients);
                nextReport += std::chrono::seconds(30);
            }
        }
        client.control(CTRL_DISCONNECT);
        result = packets > uint64_t(seconds) * 30 && nonzero > 0 ? 0 : 1;
        printf("RESULT=%s seconds=%d packets=%llu nonzero=%llu missed=%llu clients=%d ID=%s\n",
               result ? "FAIL" : "PASS", seconds, packets, nonzero, missing, clients, id.c_str());
    }
    WSACleanup();
    return result;
}
