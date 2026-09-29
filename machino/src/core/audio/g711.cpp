#include "core/audio/g711.hpp"

namespace machino { namespace audio {

static const int kBias = 0x84;
static const int kClip = 32635;

uint8_t ulaw_encode(int16_t pcm) {
    int s = pcm;
    const int sign = (s >> 8) & 0x80;
    if (sign) s = -s;
    if (s > kClip) s = kClip;
    s += kBias;
    int exp = 7;
    for (int m = 0x4000; (s & m) == 0 && exp > 0; m >>= 1) --exp;
    const int mant = (s >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

uint8_t alaw_encode(int16_t pcm) {
    int s = pcm;
    const int sign = ((~s) >> 8) & 0x80;       // A-law: bit set for POSITIVE samples
    if (!sign) s = -s;
    if (s > kClip) s = kClip;
    if (s >= 256) {
        int exp = 7;
        for (int m = 0x4000; (s & m) == 0 && exp > 0; m >>= 1) --exp;
        const int mant = (s >> (exp + 3)) & 0x0F;
        return (uint8_t)((sign | (exp << 4) | mant) ^ 0x55);
    }
    return (uint8_t)((sign | (s >> 4)) ^ 0x55);
}

int16_t ulaw_decode(uint8_t in) {
    const uint8_t u = (uint8_t)~in;
    int t = ((u & 0x0F) << 3) + kBias;
    t <<= ((unsigned)u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (kBias - t) : (t - kBias));
}

int16_t alaw_decode(uint8_t in) {
    const uint8_t a = in ^ 0x55;
    int t = (a & 0x0F) << 4;
    const int seg = ((unsigned)a & 0x70) >> 4;
    switch (seg) {
        case 0:  t += 8; break;
        case 1:  t += 0x108; break;
        default: t += 0x108; t <<= seg - 1;
    }
    return (int16_t)((a & 0x80) ? t : -t);
}

void ulaw_encode(const int16_t* pcm, size_t n, uint8_t* out) { for (size_t i = 0; i < n; ++i) out[i] = ulaw_encode(pcm[i]); }
void alaw_encode(const int16_t* pcm, size_t n, uint8_t* out) { for (size_t i = 0; i < n; ++i) out[i] = alaw_encode(pcm[i]); }
void ulaw_decode(const uint8_t* in, size_t n, int16_t* out) { for (size_t i = 0; i < n; ++i) out[i] = ulaw_decode(in[i]); }
void alaw_decode(const uint8_t* in, size_t n, int16_t* out) { for (size_t i = 0; i < n; ++i) out[i] = alaw_decode(in[i]); }

}} // namespace machino::audio
