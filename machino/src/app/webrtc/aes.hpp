// AES-128 block ENCRYPTION only (FIPS-197): everything SRTP needs - the
// counter-mode keystream and the RFC 3711 key derivation - runs the forward
// cipher on single blocks. No decryption, no modes, no key sizes beyond 128.
// Verified against the FIPS-197 appendix vector in the tests.
#pragma once
#include <cstdint>

namespace machino { namespace webrtc {

struct Aes128 {
    uint32_t rk[44];                       // expanded round keys
    explicit Aes128(const uint8_t key[16]);
    void encrypt(const uint8_t in[16], uint8_t out[16]) const;
};

}} // namespace machino::webrtc
