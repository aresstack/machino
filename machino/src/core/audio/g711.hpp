// Machino core: ITU-T G.711 companding, PCM16 <-> mu-law / A-law. Pure code,
// no tables to keep in RAM beyond the stack; the same algorithm the C
// prototype shipped (src/codec/g711.c), held to reference values by
// tests/test_audio.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace machino { namespace audio {

uint8_t ulaw_encode(int16_t pcm);
uint8_t alaw_encode(int16_t pcm);
int16_t ulaw_decode(uint8_t u);
int16_t alaw_decode(uint8_t a);

void ulaw_encode(const int16_t* pcm, size_t n, uint8_t* out);
void alaw_encode(const int16_t* pcm, size_t n, uint8_t* out);

}} // namespace machino::audio
