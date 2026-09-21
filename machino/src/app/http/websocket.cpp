#include "app/http/websocket.hpp"
#include <cstring>

namespace machino { namespace ws {

// Compact SHA-1 (FIPS 180-1). Only used for the WebSocket accept key - this
// is an integrity-free handshake token, not a security primitive.
void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t total_bits = (uint64_t)len * 8;
    uint8_t block[64];
    size_t off = 0;
    bool appended = false, length_done = false;
    while (!length_done) {
        size_t n = 0;
        if (off < len) {
            n = len - off < 64 ? len - off : 64;
            std::memcpy(block, data + off, n);
            off += n;
        }
        if (n < 64) {
            if (!appended) { block[n++] = 0x80; appended = true; }
            if (n <= 56) {
                std::memset(block + n, 0, 56 - n);
                for (int i = 0; i < 8; ++i) block[56 + i] = (uint8_t)(total_bits >> (56 - 8 * i));
                length_done = true;
            } else {
                std::memset(block + n, 0, 64 - n);
            }
        }
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
        for (int i = 16; i < 80; ++i) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                      k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

std::string base64(const uint8_t* data, size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? T[v & 63] : '=';
    }
    return out;
}

std::string accept_key(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string joined = key + GUID;
    uint8_t digest[20];
    sha1((const uint8_t*)joined.data(), joined.size(), digest);
    return base64(digest, 20);
}

std::string handshake_response(const std::string& key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
}

static std::string frame_with(uint8_t opcode, const void* payload, size_t len) {
    std::string out;
    out.reserve(len + 10);
    out += (char)(0x80 | opcode);                     // FIN + opcode
    if (len < 126) out += (char)len;
    else if (len < 65536) {
        out += (char)126;
        out += (char)(len >> 8); out += (char)(len & 0xff);
    } else {
        out += (char)127;
        for (int i = 7; i >= 0; --i) out += (char)((uint64_t)len >> (8 * i));
    }
    out.append((const char*)payload, len);
    return out;
}

std::string frame(bool text, const void* payload, size_t len) { return frame_with(text ? 1 : 2, payload, len); }
std::string pong_frame(const std::string& p) { return frame_with(10, p.data(), p.size()); }
std::string close_frame() { return frame_with(8, "", 0); }

Parse parse_frame(const std::string& in, size_t& consumed, int& opcode, std::string& payload, size_t max_payload) {
    if (in.size() < 2) return Parse::Incomplete;
    const uint8_t b0 = (uint8_t)in[0], b1 = (uint8_t)in[1];
    if (!(b0 & 0x80)) return Parse::Bad;              // fragmented: not part of this contract
    if (b0 & 0x70) return Parse::Bad;                 // RSV bits: no extensions negotiated
    opcode = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    if (!masked) return Parse::Bad;                   // clients MUST mask (RFC 6455 5.1)
    uint64_t len = b1 & 0x7f;
    size_t p = 2;
    if (len == 126) {
        if (in.size() < 4) return Parse::Incomplete;
        len = ((uint64_t)(uint8_t)in[2] << 8) | (uint8_t)in[3];
        p = 4;
    } else if (len == 127) {
        if (in.size() < 10) return Parse::Incomplete;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)in[2 + i];
        p = 10;
    }
    if (len > max_payload) return Parse::Bad;
    if (in.size() < p + 4 + len) return Parse::Incomplete;
    const uint8_t* mask = (const uint8_t*)in.data() + p;
    payload.clear();
    payload.reserve((size_t)len);
    for (uint64_t i = 0; i < len; ++i)
        payload += (char)((uint8_t)in[p + 4 + i] ^ mask[i & 3]);
    consumed = p + 4 + (size_t)len;
    return Parse::Ok;
}

}} // namespace machino::ws
