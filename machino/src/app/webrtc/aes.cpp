#include "app/webrtc/aes.hpp"

namespace machino { namespace webrtc {

namespace {

// S-box computed at first use from the field arithmetic rather than pasted as
// a 256-entry table nobody can eyeball; the FIPS-197 vector in the tests
// pins the result.
struct Tables {
    uint8_t sbox[256];
    Tables() {
        // multiplicative inverse via exp/log over GF(2^8), generator 3
        uint8_t exp[256], log[256];
        uint8_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = x; log[x] = (uint8_t)i;
            uint8_t hx = (uint8_t)(x << 1) ^ (uint8_t)((x & 0x80) ? 0x1b : 0);
            x ^= hx;                        // x *= 3
        }
        exp[255] = exp[0];
        for (int i = 0; i < 256; ++i) {
            uint8_t inv = i == 0 ? 0 : exp[255 - log[(uint8_t)i]];
            uint8_t s = inv;
            s = (uint8_t)(s ^ (uint8_t)((inv << 1) | (inv >> 7))
                            ^ (uint8_t)((inv << 2) | (inv >> 6))
                            ^ (uint8_t)((inv << 3) | (inv >> 5))
                            ^ (uint8_t)((inv << 4) | (inv >> 4)) ^ 0x63);
            sbox[i] = s;
        }
    }
};
const Tables& tabs() { static Tables t; return t; }

uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

} // namespace

Aes128::Aes128(const uint8_t key[16]) {
    const uint8_t* S = tabs().sbox;
    for (int i = 0; i < 4; ++i)
        rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
                ((uint32_t)key[4*i+2] << 8) | key[4*i+3];
    uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
        uint32_t t = rk[i - 1];
        if ((i & 3) == 0) {
            t = (t << 8) | (t >> 24);                          // RotWord
            t = ((uint32_t)S[(t >> 24) & 0xff] << 24) | ((uint32_t)S[(t >> 16) & 0xff] << 16) |
                ((uint32_t)S[(t >> 8) & 0xff] << 8) | S[t & 0xff];
            t ^= (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

void Aes128::encrypt(const uint8_t in[16], uint8_t out[16]) const {
    const uint8_t* S = tabs().sbox;
    uint8_t st[16];
    for (int i = 0; i < 16; ++i) st[i] = in[i];
    auto add_rk = [&](int r) {
        for (int c = 0; c < 4; ++c) {
            uint32_t k = rk[4 * r + c];
            st[4*c]   ^= (uint8_t)(k >> 24);
            st[4*c+1] ^= (uint8_t)(k >> 16);
            st[4*c+2] ^= (uint8_t)(k >> 8);
            st[4*c+3] ^= (uint8_t)k;
        }
    };
    add_rk(0);
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) st[i] = S[st[i]];         // SubBytes
        // ShiftRows (state is column-major: st[4c+r])
        uint8_t t;
        t = st[1]; st[1] = st[5]; st[5] = st[9]; st[9] = st[13]; st[13] = t;
        t = st[2]; st[2] = st[10]; st[10] = t; t = st[6]; st[6] = st[14]; st[14] = t;
        t = st[15]; st[15] = st[11]; st[11] = st[7]; st[7] = st[3]; st[3] = t;
        if (round < 10) {                                      // MixColumns
            for (int c = 0; c < 4; ++c) {
                uint8_t* p = st + 4 * c;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        add_rk(round);
    }
    for (int i = 0; i < 16; ++i) out[i] = st[i];
}

}} // namespace machino::webrtc
