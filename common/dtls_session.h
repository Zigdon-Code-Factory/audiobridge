#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/timing.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// Callback to look up a PSK by client identity (UUID string).
// Returns true if found, filling outPsk with the raw key bytes.
using PskLookupFn = std::function<bool(const std::string& identity, std::vector<uint8_t>& outPsk)>;

class DtlsSession {
public:
    DtlsSession();
    ~DtlsSession();

    // Initialize as DTLS 1.2 PSK server.
    // socketFd: the existing bound UDP socket (not owned, not closed by this class)
    // peerAddr: the connected client's address
    // pskLookup: callback to resolve PSK identity -> key
    bool init(int socketFd, const sockaddr_in& peerAddr, PskLookupFn pskLookup);

    // Initialize as DTLS 1.2 PSK client.
    // socketFd: the UDP socket connected to the server
    // serverAddr: the server's address
    // identity: PSK identity (client UUID)
    // psk: raw PSK bytes
    bool initClient(int socketFd, const sockaddr_in& serverAddr,
                    const std::string& identity, const std::vector<uint8_t>& psk);

    // Feed a raw UDP datagram received from the socket into the DTLS engine.
    // Call this from the main recv loop when the first byte indicates a DTLS record (20-25).
    void pushReceivedData(const uint8_t* data, size_t len);

    // Drive one step of the DTLS handshake.
    // Returns 0 on success (handshake complete), MBEDTLS_ERR_SSL_WANT_READ if more
    // data needed, or a negative mbedTLS error code on failure.
    int continueHandshake();

    // Send encrypted application data. Returns bytes written or negative error.
    int send(const uint8_t* data, size_t len);

    // Receive decrypted application data. Returns bytes read, 0 on timeout, or negative error.
    // Call pushReceivedData() first to feed the DTLS record, then call this to get plaintext.
    int recv(uint8_t* buf, size_t maxLen);

    // Gracefully close the DTLS session.
    void teardown();

    bool isEstablished() const { return established_; }

    // Generate a 32-byte random PSK, returned as 64-char hex string.
    static std::string generatePsk();

    // Convert between hex PSK and base64 for wire transmission.
    static std::string pskToBase64(const std::string& pskHex);
    static std::string base64ToPskHex(const std::string& b64);

    // Convert hex string to raw bytes and vice versa.
    static std::vector<uint8_t> hexToBytes(const std::string& hex);
    static std::string bytesToHex(const std::vector<uint8_t>& bytes);

private:
    // mbedTLS BIO callbacks
    static int bioSend(void* ctx, const unsigned char* buf, size_t len);
    static int bioRecv(void* ctx, unsigned char* buf, size_t len, uint32_t timeout);
    // Non-blocking recv for client handshake (never sleeps; outer loop feeds data)
    static int bioRecvNonBlocking(void* ctx, unsigned char* buf, size_t len);

    // mbedTLS PSK callback
    static int pskCallback(void* ctx, mbedtls_ssl_context* ssl,
                           const unsigned char* identity, size_t identityLen);

    // mbedTLS contexts
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context ctrDrbg_;
    mbedtls_timing_delay_context timer_;
    mbedtls_ssl_cookie_ctx cookie_;

    // Socket and peer (not owned)
    int socketFd_ = -1;
    sockaddr_in peerAddr_{};

    // PSK lookup
    PskLookupFn pskLookup_;

    // Internal receive buffer (push model: main loop pushes datagrams here)
    std::mutex recvMutex_;
    std::condition_variable recvCv_;
    std::vector<uint8_t> recvBuf_;
    bool recvReady_ = false;

    std::atomic<bool> established_{false};
    bool initialized_ = false;
    bool isServer_ = false;
    bool cookieSetup_ = false;
};
