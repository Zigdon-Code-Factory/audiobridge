#include "dtls_session.h"

#include <mbedtls/error.h>
#include <mbedtls/base64.h>
#include <mbedtls/debug.h>

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

bool DtlsSession::init(int socketFd, const sockaddr_in& peerAddr, PskLookupFn pskLookup) {
    if (initialized_) teardown();

    socketFd_ = socketFd;
    peerAddr_ = peerAddr;
    pskLookup_ = pskLookup;

    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctrDrbg_);
    mbedtls_ssl_cookie_init(&cookie_);

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

    // Set RNG
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctrDrbg_);

    // Set PSK callback for identity-based lookup
    mbedtls_ssl_conf_psk_cb(&conf_, pskCallback, this);

    // Set up DTLS cookies (HelloVerifyRequest)
    ret = mbedtls_ssl_cookie_setup(&cookie_, mbedtls_ctr_drbg_random, &ctrDrbg_);
    if (ret != 0) {
        printf("[DTLS] cookie_setup failed: -0x%04X\n", -ret);
        return false;
    }
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
    mbedtls_ssl_set_bio(&ssl_, this, bioSend, nullptr, bioRecv);

    // Set client transport ID for cookie verification
    // Use the peer's IP:port as the client ID
    unsigned char clientId[6]; // 4 bytes IP + 2 bytes port
    memcpy(clientId, &peerAddr.sin_addr, 4);
    memcpy(clientId + 4, &peerAddr.sin_port, 2);
    mbedtls_ssl_set_client_transport_id(&ssl_, clientId, sizeof(clientId));

    initialized_ = true;
    established_ = false;
    printf("[DTLS] Server session initialized for %s:%d\n",
           inet_ntoa(peerAddr_.sin_addr), ntohs(peerAddr_.sin_port));
    return true;
}

bool DtlsSession::initClient(int socketFd, const sockaddr_in& serverAddr,
                               const std::string& identity, const std::vector<uint8_t>& psk) {
    if (initialized_) teardown();

    socketFd_ = socketFd;
    peerAddr_ = serverAddr;

    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctrDrbg_);

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

    mbedtls_ssl_set_bio(&ssl_, this, bioSend, nullptr, bioRecv);

    initialized_ = true;
    established_ = false;
    printf("[DTLS] Client session initialized for %s:%d\n",
           inet_ntoa(peerAddr_.sin_addr), ntohs(peerAddr_.sin_port));
    return true;
}

void DtlsSession::pushReceivedData(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(recvMutex_);
    recvBuf_.assign(data, data + len);
    recvReady_ = true;
    recvCv_.notify_one();
}

int DtlsSession::continueHandshake() {
    int ret = mbedtls_ssl_handshake(&ssl_);
    if (ret == 0) {
        established_ = true;
        printf("[DTLS] Handshake complete, cipher: %s\n",
               mbedtls_ssl_get_ciphersuite(&ssl_));
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

    if (established_) {
        // Best-effort close_notify
        mbedtls_ssl_close_notify(&ssl_);
    }

    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&conf_);
    mbedtls_ssl_cookie_free(&cookie_);
    mbedtls_ctr_drbg_free(&ctrDrbg_);
    mbedtls_entropy_free(&entropy_);

    established_ = false;
    initialized_ = false;
    socketFd_ = -1;

    // Clear receive buffer
    {
        std::lock_guard<std::mutex> lock(recvMutex_);
        recvBuf_.clear();
        recvReady_ = false;
    }

    printf("[DTLS] Session torn down\n");
}

// --- BIO Callbacks ---

int DtlsSession::bioSend(void* ctx, const unsigned char* buf, size_t len) {
    auto* self = static_cast<DtlsSession*>(ctx);

    ssize_t sent = sendto(self->socketFd_, (const char*)buf, len, 0,
                          (const sockaddr*)&self->peerAddr_, sizeof(self->peerAddr_));
    if (sent < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)sent;
}

int DtlsSession::bioRecv(void* ctx, unsigned char* buf, size_t len, uint32_t timeout) {
    auto* self = static_cast<DtlsSession*>(ctx);

    std::unique_lock<std::mutex> lock(self->recvMutex_);

    if (!self->recvReady_) {
        if (timeout == 0) {
            // Non-blocking: no data available
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        // Wait for data up to timeout
        auto waitResult = self->recvCv_.wait_for(lock, std::chrono::milliseconds(timeout),
                                                  [self] { return self->recvReady_; });
        if (!waitResult) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
    }

    // Copy data from internal buffer
    size_t copyLen = std::min(len, self->recvBuf_.size());
    memcpy(buf, self->recvBuf_.data(), copyLen);
    self->recvBuf_.clear();
    self->recvReady_ = false;

    return (int)copyLen;
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

std::string DtlsSession::generatePsk() {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctrDrbg);

    const char* pers = "audiobridge_psk_gen";
    mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy,
                           (const unsigned char*)pers, strlen(pers));

    uint8_t key[32];
    mbedtls_ctr_drbg_random(&ctrDrbg, key, sizeof(key));

    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_entropy_free(&entropy);

    return bytesToHex(std::vector<uint8_t>(key, key + sizeof(key)));
}

std::string DtlsSession::pskToBase64(const std::string& pskHex) {
    auto bytes = hexToBytes(pskHex);
    size_t olen = 0;
    // Get required output length
    mbedtls_base64_encode(nullptr, 0, &olen, bytes.data(), bytes.size());
    std::vector<unsigned char> out(olen);
    mbedtls_base64_encode(out.data(), out.size(), &olen, bytes.data(), bytes.size());
    return std::string(out.begin(), out.begin() + olen);
}

std::string DtlsSession::base64ToPskHex(const std::string& b64) {
    size_t olen = 0;
    mbedtls_base64_decode(nullptr, 0, &olen,
                           (const unsigned char*)b64.data(), b64.size());
    std::vector<unsigned char> out(olen);
    int ret = mbedtls_base64_decode(out.data(), out.size(), &olen,
                                     (const unsigned char*)b64.data(), b64.size());
    if (ret != 0) return "";
    out.resize(olen);
    return bytesToHex(std::vector<uint8_t>(out.begin(), out.end()));
}

std::vector<uint8_t> DtlsSession::hexToBytes(const std::string& hex) {
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
