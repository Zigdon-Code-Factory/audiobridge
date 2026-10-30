#include "dtls_session.h"

#include <mbedtls/error.h>

// MBEDTLS_ERR_NET_SEND_FAILED / RECV_FAILED are in net_sockets.h which
// may not be available on all platforms (e.g. Android NDK). Define fallbacks.
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

#include <cstdio>
#include <cstring>
#include <chrono>

#ifdef _WIN32
#include <WinSock2.h>
typedef int ssize_t; // WinSock sendto returns int
#else
#include <sys/select.h>
#include <unistd.h>
#endif

// Cipher suite list: PSK with AES-128-GCM (single entry + terminator)
static const int ciphersuites[] = {
    MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,
    0
};

DtlsSession::DtlsSession() {}

DtlsSession::~DtlsSession() {
    teardown();
}

bool DtlsSession::init(DtlsSocket socketFd, const sockaddr_in& peerAddr, PskLookupFn pskLookup) {
    if (initialized_) teardown();

    socketFd_ = socketFd;
    peerAddr_ = peerAddr;
    pskLookup_ = pskLookup;

    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctrDrbg_);
    mbedtls_ssl_cookie_init(&cookie_);
    cookieSetup_ = true;
    initialized_ = true;

    // Seed the DRBG
    const char* pers = "audiobridge_dtls_server";
    int ret = mbedtls_ctr_drbg_seed(&ctrDrbg_, mbedtls_entropy_func, &entropy_,
                                     (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        printf("[DTLS] ctr_drbg_seed failed: -0x%04X\n", -ret);
        return false;
    }

    // Configure as DTLS 1.2 server
    ret = mbedtls_ssl_config_defaults(&conf_,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[DTLS] ssl_config_defaults failed: -0x%04X\n", -ret);
        return false;
    }

    // Force DTLS 1.2 only
    mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);

    // Set cipher suite
    mbedtls_ssl_conf_ciphersuites(&conf_, ciphersuites);
    mbedtls_ssl_conf_dtls_anti_replay(&conf_, MBEDTLS_SSL_ANTI_REPLAY_ENABLED);
    mbedtls_ssl_conf_handshake_timeout(&conf_, 250, 2000);

    // Set RNG
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctrDrbg_);

    // Set PSK callback for identity-based lookup
    mbedtls_ssl_conf_psk_cb(&conf_, pskCallback, this);

    // Set up DTLS cookies (HelloVerifyRequest) — server only
    ret = mbedtls_ssl_cookie_setup(&cookie_, mbedtls_ctr_drbg_random, &ctrDrbg_);
    if (ret != 0) {
        printf("[DTLS] cookie_setup failed: -0x%04X\n", -ret);
        return false;
    }
    cookieSetup_ = true;
    mbedtls_ssl_conf_dtls_cookies(&conf_, mbedtls_ssl_cookie_write,
                                   mbedtls_ssl_cookie_check, &cookie_);

    // Setup SSL context
    ret = mbedtls_ssl_setup(&ssl_, &conf_);
    if (ret != 0) {
        printf("[DTLS] ssl_setup failed: -0x%04X\n", -ret);
        return false;
    }

    // Set timer callbacks (required for DTLS retransmission)
    mbedtls_ssl_set_timer_cb(&ssl_, &timer_,
                              mbedtls_timing_set_delay,
                              mbedtls_timing_get_delay);

    // Set BIO callbacks (push model)
    mbedtls_ssl_set_bio(&ssl_, this, bioSend, bioRecvNonBlocking, nullptr);

    // Set client transport ID for cookie verification
    // Use the peer's IP:port as the client ID
    unsigned char clientId[6]; // 4 bytes IP + 2 bytes port
    memcpy(clientId, &peerAddr.sin_addr, 4);
    memcpy(clientId + 4, &peerAddr.sin_port, 2);
    mbedtls_ssl_set_client_transport_id(&ssl_, clientId, sizeof(clientId));

    initialized_ = true;
    isServer_ = true;
    established_ = false;
    printf("[DTLS] Server session initialized for %s:%d\n",
           inet_ntoa(peerAddr_.sin_addr), ntohs(peerAddr_.sin_port));
    return true;
}

bool DtlsSession::initClient(DtlsSocket socketFd, const sockaddr_in& serverAddr,
                               const std::string& identity, const std::vector<uint8_t>& psk) {
    if (initialized_) teardown();

    socketFd_ = socketFd;
    peerAddr_ = serverAddr;

    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctrDrbg_);
    initialized_ = true;
    // Cookies are server-only.
    // cookieSetup_ stays false; teardown() will skip mbedtls_ssl_cookie_free().

    const char* pers = "audiobridge_dtls_client";
    int ret = mbedtls_ctr_drbg_seed(&ctrDrbg_, mbedtls_entropy_func, &entropy_,
                                     (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        printf("[DTLS] ctr_drbg_seed failed: -0x%04X\n", -ret);
        return false;
    }

    // Configure as DTLS 1.2 client
    ret = mbedtls_ssl_config_defaults(&conf_,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[DTLS] ssl_config_defaults failed: -0x%04X\n", -ret);
        return false;
    }

    mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&conf_, ciphersuites);
    mbedtls_ssl_conf_dtls_anti_replay(&conf_, MBEDTLS_SSL_ANTI_REPLAY_ENABLED);
    mbedtls_ssl_conf_handshake_timeout(&conf_, 250, 2000);
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctrDrbg_);

    // Set PSK directly (client knows its own identity and key)
    ret = mbedtls_ssl_conf_psk(&conf_, psk.data(), psk.size(),
                                (const unsigned char*)identity.data(), identity.size());
    if (ret != 0) {
        printf("[DTLS] ssl_conf_psk failed: -0x%04X\n", -ret);
        return false;
    }

    ret = mbedtls_ssl_setup(&ssl_, &conf_);
    if (ret != 0) {
        printf("[DTLS] ssl_setup failed: -0x%04X\n", -ret);
        return false;
    }

    mbedtls_ssl_set_timer_cb(&ssl_, &timer_,
                              mbedtls_timing_set_delay,
                              mbedtls_timing_get_delay);

    // Use non-blocking recv for client: the handshake loop calls continueHandshake()
    // then recvfrom()+pushReceivedData() in the same thread.  If we block inside
    // bioRecv (waiting on recvCv_), we never reach recvfrom() — classic deadlock.
    // bioRecvNonBlocking returns WANT_READ immediately when no data is ready, so
    // the outer loop stays in control and the mbedtls timer handles DTLS retransmit.
    mbedtls_ssl_set_bio(&ssl_, this, bioSend, bioRecvNonBlocking, nullptr);

    initialized_ = true;
    isServer_ = false;
    established_ = false;
    printf("[DTLS] Client session initialized for %s:%d\n",
           inet_ntoa(peerAddr_.sin_addr), ntohs(peerAddr_.sin_port));
    return true;
}

void DtlsSession::pushReceivedData(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(recvMutex_);
    // Bound memory while preserving datagram boundaries.
    if (len == 0 || len > 65535 || recvQueue_.size() >= 32) return;
    recvQueue_.emplace_back(data, data + len);
}

int DtlsSession::continueHandshake() {
    if (!initialized_) return -1;
    int ret = mbedtls_ssl_handshake(&ssl_);
    if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED && isServer_) {
        // A cookie challenge is an expected part of the DTLS handshake.
        ret = mbedtls_ssl_session_reset(&ssl_);
        unsigned char clientId[6];
        memcpy(clientId, &peerAddr_.sin_addr, 4);
        memcpy(clientId + 4, &peerAddr_.sin_port, 2);
        if (ret == 0) ret = mbedtls_ssl_set_client_transport_id(&ssl_, clientId, sizeof(clientId));
        return ret == 0 ? MBEDTLS_ERR_SSL_WANT_READ : ret;
    }
    if (ret == 0) {
        established_ = true;
        printf("[DTLS] Handshake complete! cipher=%s peer=%s:%d\n",
               mbedtls_ssl_get_ciphersuite(&ssl_),
               inet_ntoa(peerAddr_.sin_addr), ntohs(peerAddr_.sin_port));
    } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        char errBuf[128];
        mbedtls_strerror(ret, errBuf, sizeof(errBuf));
        printf("[DTLS] continueHandshake: error -0x%04X (%s)\n", -ret, errBuf);
    }
    return ret;
}

int DtlsSession::send(const uint8_t* data, size_t len) {
    if (!established_) return -1;

    int ret;
    do {
        ret = mbedtls_ssl_write(&ssl_, data, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret < 0) {
        char errBuf[128];
        mbedtls_strerror(ret, errBuf, sizeof(errBuf));
        printf("[DTLS] send error: %s (-0x%04X)\n", errBuf, -ret);
    }
    return ret;
}

int DtlsSession::recv(uint8_t* buf, size_t maxLen) {
    if (!established_) return -1;

    int ret = mbedtls_ssl_read(&ssl_, buf, maxLen);

    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        return 0; // No data available
    }

    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        printf("[DTLS] Peer sent close_notify\n");
        established_ = false;
        return -1;
    }

    if (ret < 0) {
        char errBuf[128];
        mbedtls_strerror(ret, errBuf, sizeof(errBuf));
        printf("[DTLS] recv error: %s (-0x%04X)\n", errBuf, -ret);
        return ret;
    }

    return ret;
}

void DtlsSession::teardown() {
    if (!initialized_) return;

    printf("[DTLS] teardown: starting (isServer=%d, cookieSetup=%d, established=%d)\n",
           (int)isServer_, (int)cookieSetup_, (int)established_.load());

    // Mark as not initialized first to prevent re-entry
    initialized_ = false;
    isServer_ = false;

    if (established_) {
        // Best-effort close_notify — don't crash if it fails
        established_ = false;
        try {
            mbedtls_ssl_close_notify(&ssl_);
        } catch (...) {
            // Ignore — we're shutting down
        }
    }

    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&conf_);
    if (cookieSetup_) {
        // Only free cookies on the server path — client never calls cookie_setup()
        // and calling cookie_free() on a half-initialized ctx crashes (null md_info).
        printf("[DTLS] Freeing cookie ctx (server path)\n");
        mbedtls_ssl_cookie_free(&cookie_);
        cookieSetup_ = false;
    }
    mbedtls_ctr_drbg_free(&ctrDrbg_);
    mbedtls_entropy_free(&entropy_);

    socketFd_ = -1;

    // Clear receive buffer
    {
        std::lock_guard<std::mutex> lock(recvMutex_);
        recvQueue_.clear();
    }

    printf("[DTLS] Session torn down\n");
}

// --- BIO Callbacks ---

int DtlsSession::bioSend(void* ctx, const unsigned char* buf, size_t len) {
    auto* self = static_cast<DtlsSession*>(ctx);

    ssize_t sent = sendto(self->socketFd_, (const char*)buf, len, 0,
                          (const sockaddr*)&self->peerAddr_, sizeof(self->peerAddr_));
    if (sent < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        printf("[DTLS] bioSend: sendto failed len=%zu err=%d\n", len, err);
#else
        printf("[DTLS] bioSend: sendto failed len=%zu errno=%d\n", len, errno);
#endif
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)sent;
}

// Both peers drive the socket and handshake in one thread. Never block here.
int DtlsSession::bioRecvNonBlocking(void* ctx, unsigned char* buf, size_t len) {
    auto* self = static_cast<DtlsSession*>(ctx);
    std::lock_guard<std::mutex> lock(self->recvMutex_);
    if (self->recvQueue_.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
    auto packet = std::move(self->recvQueue_.front());
    self->recvQueue_.pop_front();
    if (packet.size() > len) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(buf, packet.data(), packet.size());
    return static_cast<int>(packet.size());
}

// --- PSK Callback ---

int DtlsSession::pskCallback(void* ctx, mbedtls_ssl_context* ssl,
                               const unsigned char* identity, size_t identityLen) {
    auto* self = static_cast<DtlsSession*>(ctx);

    std::string id(reinterpret_cast<const char*>(identity), identityLen);
    printf("[DTLS] PSK lookup for identity: %s\n", id.c_str());

    std::vector<uint8_t> psk;
    if (!self->pskLookup_ || !self->pskLookup_(id, psk)) {
        printf("[DTLS] PSK not found for identity: %s\n", id.c_str());
        return -1;
    }

    int ret = mbedtls_ssl_set_hs_psk(ssl, psk.data(), psk.size());
    if (ret != 0) {
        printf("[DTLS] set_hs_psk failed: -0x%04X\n", -ret);
        return ret;
    }

    return 0;
}

// --- Static Helpers ---

std::vector<uint8_t> DtlsSession::hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i + j];
            byte <<= 4;
            if (c >= '0' && c <= '9') byte |= (c - '0');
            else if (c >= 'a' && c <= 'f') byte |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte |= (c - 'A' + 10);
            else return {};
        }
        bytes.push_back(byte);
    }
    return bytes;
}

std::string DtlsSession::bytesToHex(const std::vector<uint8_t>& bytes) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        result.push_back(hex[b >> 4]);
        result.push_back(hex[b & 0x0f]);
    }
    return result;
}
