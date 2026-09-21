// SRTP/SRTCP (RFC 3711), profile SRTP_AES128_CM_HMAC_SHA1_80 only - exactly
// what DTLS-SRTP negotiates with every browser. The camera SENDS RTP (protect)
// and RECEIVES RTCP (unprotect, for PLI), plus protects its own sender
// reports. Keys come exclusively from the DTLS exporter (never from SDP).
// Pure: AES via aes.hpp, HMAC via stun.hpp; KDF and counter mode are pinned
// to independently computed RFC 3711 vectors in the tests.
#pragma once
#include "app/webrtc/aes.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace machino { namespace webrtc {

// One direction's key material as DTLS-SRTP hands it over.
struct SrtpKey {
    uint8_t master_key[16];
    uint8_t master_salt[14];
};

class SrtpSession {
public:
    // `out` protects what we send (RTP + our RTCP), `in` checks what the
    // browser sends (its RTCP; it sends no RTP on a sendonly track).
    SrtpSession(const SrtpKey& out, const SrtpKey& in);

    // Protects one RTP packet in place (appends the 10-byte tag). The caller
    // passes the FULL packet (12-byte header + payload); seq is read from the
    // header, the rollover counter is tracked here.
    bool protect_rtp(std::vector<uint8_t>& pkt);

    // Protects one RTCP compound packet (appends E+index and the tag).
    bool protect_rtcp(std::vector<uint8_t>& pkt);

    // Verifies and decrypts one incoming SRTCP packet in place; false on bad
    // auth, replay or malformed input. On success `pkt` is the plain RTCP.
    bool unprotect_rtcp(std::vector<uint8_t>& pkt);

private:
    struct Dir {
        Aes128  cipher;           // session encryption key
        uint8_t salt[14];         // session salt
        uint8_t auth[20];         // session auth key
        explicit Dir(const SrtpKey& mk, bool rtcp);
    };
    Dir  rtp_out_, rtcp_out_, rtcp_in_;
    uint32_t roc_ = 0;            // sender rollover counter
    uint16_t last_seq_ = 0;
    bool     seq_seen_ = false;
    uint32_t rtcp_index_ = 0;     // our SRTCP index
    uint64_t in_replay_high_ = 0; // highest accepted incoming SRTCP index
    uint64_t in_replay_mask_ = 0; // 64-wide replay window below it
};

// Exposed for the vector tests: RFC 3711 4.3 key derivation (kdr = 0) and
// the 4.1.1 AES-CM keystream.
void srtp_kdf(const SrtpKey& mk, uint8_t label, uint8_t* out, size_t out_len);
void aes_cm_keystream(const Aes128& key, const uint8_t iv16[16], uint8_t* buf, size_t len);

}} // namespace machino::webrtc
