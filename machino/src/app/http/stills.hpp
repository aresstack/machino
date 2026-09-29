// Application: majestic's uncompressed still, /image.yuv420 - pure,
// host-tested.
//
// The contract is the stock WebUI's (preview-still.js): `?crop=XxYxWxH` in
// MAIN-channel pixels, and the answer says what it holds in headers -
// X-Frame-Width/-Height, X-Pixel-Format (NV12), X-Stride-Luma/-Chroma. The
// page probes with a 16x16 crop and only offers the feature when exactly
// 16x16 comes back, so the crop is honoured to the pixel (on the 2x2 chroma
// grid NV12 has; the page asks on that grid already).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace stills {

struct Crop { int x = 0, y = 0, w = 0, h = 0; };

// "XxYxWxH" (decimal, non-negative, W and H > 0). False on anything else.
bool parse_crop(const std::string& s, Crop& out);

// Cuts `want` out of a tightly packed NV12 frame (w x h, stride w), aligned
// down to even x/y and even w/h, clamped to the frame. Writes a tightly
// packed NV12 of `got.w` x `got.h`. False when nothing of the crop lies
// inside the frame.
bool nv12_crop(const uint8_t* nv12, int w, int h, const Crop& want,
               std::vector<uint8_t>& out, Crop& got);

// The response headers that describe a tightly packed NV12 of w x h.
std::string yuv_headers(int w, int h);

}} // namespace machino::stills
