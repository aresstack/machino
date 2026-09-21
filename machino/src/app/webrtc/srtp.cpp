#include "app/webrtc/srtp.hpp"
#include "app/webrtc/stun.hpp"     // hmac_sha1
#include <cstring>

namespace machino { namespace webrtc {

namespace {

// 16-byte counter block with a 16-bit block counter in the last two bytes.
void ctr_block(const uint8_t iv[16], uint16_t n, uint8_t out[16]) {
    memcpy(out, iv, 16);
    out[14] = (uint8_t)(n >> 8);
    out[15] = (uint8_t)n;
}

// RFC 3711 4.1.1: keystream XORed over buf.
void cm_xor(const Aes128& key, const uint8_t iv[16], uint8_t* buf, size_t len) {
    uint8_t blk[16], ks[16];
    for (size_t off = 0, n = 0; off < len; off += 16, ++n) {
        ctr_block(iv, (uint16_t)n, blk);
        key.encrypt(blk, ks);
        const size_t take = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < take; ++i) buf[off + i] ^= ks[i];
    }
}

// SRTP/SRTCP IV: (salt * 2^16) XOR (ssrc * 2^64) XOR (index * 2^16), as bytes:
// salt at 0..13, ssrc XORed at 4..7, the 48-bit index XORed at 8..13.
void make_iv(const uint8_t salt[14], uint32_t ssrc, uint64_t index48, uint8_t iv[16]) {
    memcpy(iv, salt, 14);
    iv[14] = iv[15] = 0;
    iv[4] ^= (uint8_t)(ssrc >> 24); iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);  iv[7] ^= (uint8_t)ssrc;
    for (int i = 0; i < 6; ++i) iv[8 + i] ^= (uint8_t)(index48 >> (8 * (5 - i)));
}

} // namespace

void aes_cm_keystream(const Aes128& key, const uint8_t iv16[16], uint8_t* buf, size_t len) {
    memset(buf, 0, len);
    cm_xor(key, iv16, buf, len);
}

// RFC 3711 4.3.1 with kdr = 0 (DTLS-SRTP default): x = master_salt with the
// label XORed into byte 7; the derived key is the AES-CM keystream under the
// master key with IV = x || 0x0000.
void srtp_kdf(const SrtpKey& mk, uint8_t label, uint8_t* out, size_t out_len) {
    uint8_t iv[16];
    memcpy(iv, mk.master_salt, 14);
    iv[7] ^= label;
    iv[14] = iv[15] = 0;
    Aes128 k(mk.master_key);
    memset(out, 0, out_len);
    cm_xor(k, iv, out, out_len);
}

SrtpSession::Dir::Dir(const SrtpKey& mk, bool rtcp)
    : cipher([&] { uint8_t ck[16]; srtp_kdf(mk, rtcp ? 3 : 0, ck, 16); return Aes128(ck); }()) {
    srtp_kdf(mk, rtcp ? 4 : 1, auth, 20);
    srtp_kdf(mk, rtcp ? 5 : 2, salt, 14);
}

SrtpSession::SrtpSession(const SrtpKey& out, const SrtpKey& in)
    : rtp_out_(out, false), rtcp_out_(out, true), rtcp_in_(in, true) {}

bool SrtpSession::protect_rtp(std::vector<uint8_t>& pkt) {
    if (pkt.size() < 12) return false;
    const uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) | ((uint32_t)pkt[10] << 8) | pkt[11];
    const uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (seq_seen_ && seq < last_seq_ && (uint16_t)(last_seq_ - seq) > 0x8000) ++roc_;   // our own wrap
    last_seq_ = seq; seq_seen_ = true;
    const uint64_t index = ((uint64_t)roc_ << 16) | seq;
    uint8_t iv[16];
    make_iv(rtp_out_.salt, ssrc, index, iv);
    cm_xor(rtp_out_.cipher, iv, pkt.data() + 12, pkt.size() - 12);
    // tag: HMAC(auth, packet || ROC), truncated to 80 bits
    std::vector<uint8_t> m(pkt);
    m.push_back((uint8_t)(roc_ >> 24)); m.push_back((uint8_t)(roc_ >> 16));
    m.push_back((uint8_t)(roc_ >> 8));  m.push_back((uint8_t)roc_);
    uint8_t mac[20];
    hmac_sha1(rtp_out_.auth, 20, m.data(), m.size(), mac);
    pkt.insert(pkt.end(), mac, mac + 10);
    return true;
}

bool SrtpSession::protect_rtcp(std::vector<uint8_t>& pkt) {
    if (pkt.size() < 8) return false;
    const uint32_t ssrc = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    const uint32_t index = ++rtcp_index_ & 0x7fffffff;
    uint8_t iv[16];
    make_iv(rtcp_out_.salt, ssrc, index, iv);
    cm_xor(rtcp_out_.cipher, iv, pkt.data() + 8, pkt.size() - 8);
    const uint32_t ei = 0x80000000u | index;                    // E bit: encrypted
    pkt.push_back((uint8_t)(ei >> 24)); pkt.push_back((uint8_t)(ei >> 16));
    pkt.push_back((uint8_t)(ei >> 8));  pkt.push_back((uint8_t)ei);
    uint8_t mac[20];
    hmac_sha1(rtcp_out_.auth, 20, pkt.data(), pkt.size(), mac);
    pkt.insert(pkt.end(), mac, mac + 10);
    return true;
}

bool SrtpSession::unprotect_rtcp(std::vector<uint8_t>& pkt) {
    if (pkt.size() < 8 + 4 + 10) return false;
    const size_t tag_at = pkt.size() - 10;
    uint8_t mac[20];
    hmac_sha1(rtcp_in_.auth, 20, pkt.data(), tag_at, mac);
    if (memcmp(mac, pkt.data() + tag_at, 10) != 0) return false;
    const size_t ei_at = tag_at - 4;
    const uint32_t ei = ((uint32_t)pkt[ei_at] << 24) | ((uint32_t)pkt[ei_at+1] << 16) |
                        ((uint32_t)pkt[ei_at+2] << 8) | pkt[ei_at+3];
    const uint32_t index = ei & 0x7fffffff;
    // sliding 64-entry replay window
    if (in_replay_high_ == 0 && in_replay_mask_ == 0 && index == 0) { /* first ever may be 0 */ }
    if (index > in_replay_high_) {
        const uint64_t shift = index - in_replay_high_;
        in_replay_mask_ = shift >= 64 ? 0 : in_replay_mask_ << shift;
        in_replay_mask_ |= 1;
        in_replay_high_ = index;
    } else {
        const uint64_t back = in_replay_high_ - index;
        if (back >= 64) return false;                           // too old
        const uint64_t bit = 1ull << back;
        if (in_replay_mask_ & bit) return false;                // replayed
        in_replay_mask_ |= bit;
    }
    if (ei & 0x80000000u) {
        const uint32_t ssrc = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
        uint8_t iv[16];
        make_iv(rtcp_in_.salt, ssrc, index, iv);
        cm_xor(rtcp_in_.cipher, iv, pkt.data() + 8, ei_at - 8);
    }
    pkt.resize(ei_at);                                          // strip E+index and tag
    return true;
}

}} // namespace machino::webrtc
