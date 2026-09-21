#include "app/webrtc/stun.hpp"
#include "app/http/websocket.hpp"   // ws::sha1
#include <cstring>

namespace machino { namespace webrtc {

namespace {

constexpr uint32_t MAGIC = 0x2112A442;

uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void wr16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void wr32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }

} // namespace

void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[20]) {
    uint8_t k[64] = {0};
    if (key_len > 64) ws::sha1(key, key_len, k);            // hashed-down long key
    else memcpy(k, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = (uint8_t)(k[i] ^ 0x36); opad[i] = (uint8_t)(k[i] ^ 0x5c); }
    std::vector<uint8_t> inner; inner.reserve(64 + msg_len);
    inner.insert(inner.end(), ipad, ipad + 64);
    inner.insert(inner.end(), msg, msg + msg_len);
    uint8_t ih[20];
    ws::sha1(inner.data(), inner.size(), ih);
    uint8_t outer[84];
    memcpy(outer, opad, 64); memcpy(outer + 64, ih, 20);
    ws::sha1(outer, 84, out);
}

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xffffffffu;
}

bool is_stun(const uint8_t* p, size_t n) {
    return n >= 20 && (p[0] & 0xc0) == 0 && rd32(p + 4) == MAGIC;
}

StunRequest parse_binding_request(const uint8_t* p, size_t n, const std::string& pwd) {
    StunRequest r;
    if (!is_stun(p, n)) return r;
    if (rd16(p) != 0x0001) return r;                        // Binding Request only
    const size_t mlen = rd16(p + 2);
    if (20 + mlen > n) return r;
    memcpy(r.tid, p + 8, 12);
    size_t at = 20;
    while (at + 4 <= 20 + mlen) {
        const uint16_t type = rd16(p + at);
        const uint16_t alen = rd16(p + at + 2);
        const uint8_t* av = p + at + 4;
        if (at + 4 + alen > 20 + mlen) break;
        if (type == 0x0006) r.username.assign((const char*)av, alen);
        else if (type == 0x0025) r.use_candidate = true;    // USE-CANDIDATE (flag)
        else if (type == 0x0008 && alen == 20) {
            // MESSAGE-INTEGRITY: HMAC over the message up to this attribute,
            // with the length field REWRITTEN as if it ended right after it.
            std::vector<uint8_t> copy(p, p + at);
            const uint16_t adj = (uint16_t)(at + 24 - 20);
            copy[2] = (uint8_t)(adj >> 8); copy[3] = (uint8_t)adj;
            uint8_t mac[20];
            hmac_sha1((const uint8_t*)pwd.data(), pwd.size(), copy.data(), copy.size(), mac);
            r.integrity_ok = memcmp(mac, av, 20) == 0;
        }
        at += 4 + alen;
        at += (4 - (alen & 3)) & 3;                         // 32-bit padding
    }
    r.ok = true;
    return r;
}

std::vector<uint8_t> binding_response(const uint8_t tid[12], uint32_t peer_ip4,
                                      uint16_t peer_port, const std::string& pwd) {
    std::vector<uint8_t> b;
    wr16(b, 0x0101);                                        // Binding Success
    wr16(b, 0);                                             // length, patched below
    wr32(b, MAGIC);
    b.insert(b.end(), tid, tid + 12);
    // XOR-MAPPED-ADDRESS (ip4)
    wr16(b, 0x0020); wr16(b, 8);
    b.push_back(0); b.push_back(0x01);
    wr16(b, (uint16_t)(peer_port ^ (MAGIC >> 16)));
    wr32(b, peer_ip4 ^ MAGIC);
    // MESSAGE-INTEGRITY over everything so far with length as-if it ends after it
    const uint16_t len_mi = (uint16_t)(b.size() - 20 + 24);
    b[2] = (uint8_t)(len_mi >> 8); b[3] = (uint8_t)len_mi;
    uint8_t mac[20];
    hmac_sha1((const uint8_t*)pwd.data(), pwd.size(), b.data(), b.size(), mac);
    wr16(b, 0x0008); wr16(b, 20);
    b.insert(b.end(), mac, mac + 20);
    // FINGERPRINT over everything so far with the final length
    const uint16_t len_fp = (uint16_t)(b.size() - 20 + 8);
    b[2] = (uint8_t)(len_fp >> 8); b[3] = (uint8_t)len_fp;
    const uint32_t fp = crc32(b.data(), b.size()) ^ 0x5354554eu;
    wr16(b, 0x8028); wr16(b, 4);
    wr32(b, fp);
    return b;
}

}} // namespace machino::webrtc
