#pragma once

#include "dtls_session.h"
#include <mbedtls/ecdh.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <array>
#include <cstdio>

// Wire v2 pairing: commit(client public key + identity), server public key,
// client reveal, human comparison, then mutual DTLS PSK authentication.
// The commitment prevents an active intermediary from choosing a key after
// learning the other side's short authentication string. Never transmit the SAS
// or the derived PSK. A fresh exchange and explicit approval are required on retry.
class SecurePairing {
public:
    SecurePairing() {
        mbedtls_ecdh_init(&ecdh_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&rng_);
    }
    ~SecurePairing() {
        mbedtls_ecdh_free(&ecdh_);
        mbedtls_ctr_drbg_free(&rng_);
        mbedtls_entropy_free(&entropy_);
    }
    SecurePairing(const SecurePairing&) = delete;
    SecurePairing& operator=(const SecurePairing&) = delete;

    bool generate() {
        const unsigned char label[] = "AudioBridge pairing v2";
        if (mbedtls_ctr_drbg_seed(&rng_, mbedtls_entropy_func, &entropy_,
                                 label, sizeof(label) - 1) != 0 ||
            mbedtls_ecdh_setup(&ecdh_, MBEDTLS_ECP_DP_CURVE25519) != 0) return false;
        // TLS ECPoint encoding: one length byte followed by the 32-byte X25519 key.
        unsigned char pub[33];
        size_t len = 0;
        if (mbedtls_ecdh_make_public(&ecdh_, &len, pub, sizeof(pub),
                                    mbedtls_ctr_drbg_random, &rng_) != 0 ||
            len != sizeof(pub) || pub[0] != 32) return false;
        publicKey_ = DtlsSession::bytesToHex({pub + 1, pub + sizeof(pub)});
        return true;
    }

    const std::string& publicKey() const { return publicKey_; }

    static bool validField(const std::string& s, size_t maxLen) {
        if (s.empty() || s.size() > maxLen) return false;
        for (unsigned char c : s) if (c < 32 || c == 127 || c == '|') return false;
        return true;
    }
    static bool validKey(const std::string& s) {
        return s.size() == 64 && DtlsSession::hexToBytes(s).size() == 32;
    }
    static bool validStoredKey(const std::string& s) {
        return s.size() == 67 && s.compare(0, 3, "v2:") == 0 && validKey(s.substr(3));
    }
    static std::string commitment(const std::string& name, const std::string& id,
                                  const std::string& pub) {
        if (!validField(name, 128) || !validField(id, 128) || !validKey(pub)) return {};
        const std::string input = "AB_PAIR2_COMMIT|" + name + "|" + id + "|" + pub;
        unsigned char hash[32];
        if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()),
                           input.size(), hash, 0) != 0) return {};
        return DtlsSession::bytesToHex({hash, hash + sizeof(hash)});
    }

    bool derive(const std::string& peerPublic, const std::string& name,
                const std::string& id, bool server, std::string& storedKey,
                std::string& code) {
        storedKey.clear();
        code.clear();
        if (publicKey_.empty() || !validKey(peerPublic) ||
            !validField(name, 128) || !validField(id, 128)) return false;
        auto peer = DtlsSession::hexToBytes(peerPublic);
        peer.insert(peer.begin(), 32);
        if (mbedtls_ecdh_read_public(&ecdh_, peer.data(), peer.size()) != 0) return false;
        std::array<unsigned char, 32> secret{};
        size_t len = 0;
        int ret = mbedtls_ecdh_calc_secret(&ecdh_, &len, secret.data(), secret.size(),
                                         mbedtls_ctr_drbg_random, &rng_);
        unsigned char nonzero = 0;
        for (auto b : secret) nonzero |= b;
        if (ret != 0 || len != secret.size() || nonzero == 0) {
            mbedtls_platform_zeroize(secret.data(), secret.size());
            return false;
        }
        const std::string transcript = "AB_PAIR2|" + name + "|" + id + "|" +
            (server ? peerPublic : publicKey_) + "|" + (server ? publicKey_ : peerPublic);
        unsigned char salt[32], output[36];
        const unsigned char info[] = "AudioBridge v2 PSK and SAS";
        ret = mbedtls_sha256(reinterpret_cast<const unsigned char*>(transcript.data()),
                             transcript.size(), salt, 0);
        if (ret == 0) ret = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
            salt, sizeof(salt), secret.data(), secret.size(), info, sizeof(info) - 1,
            output, sizeof(output));
        mbedtls_platform_zeroize(secret.data(), secret.size());
        if (ret == 0) {
            storedKey = "v2:" + DtlsSession::bytesToHex({output, output + 32});
            uint32_t value = (uint32_t(output[32]) << 24) | (uint32_t(output[33]) << 16) |
                             (uint32_t(output[34]) << 8) | output[35];
            char digits[7];
            std::snprintf(digits, sizeof(digits), "%06u", unsigned(value % 1000000));
            code = digits;
        }
        mbedtls_platform_zeroize(output, sizeof(output));
        return ret == 0;
    }

private:
    mbedtls_ecdh_context ecdh_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context rng_;
    std::string publicKey_;
};
