// Application: minimal fragmented-MP4 (CMAF-style) muxer for the majestic
// /ws/video contract - pure functions, host-testable. The stock webui's MSE
// player (upstream preview.js) expects exactly:
//   * one binary INIT segment: ftyp + moov (avc1 + avcC, mvex/trex),
//   * then ONE fragment PER FRAME: moof(mfhd, traf(tfhd default-base-is-moof,
//     tfdt, trun)) + mdat, where mdat carries AVCC length-prefixed NALs
//     (Annex-B start codes must be converted),
//   * mime "video/mp4; codecs=\"avc1.PPCCLL\"" (PPCCLL from the SPS bytes).
// A prft prefix is OPTIONAL (the client's readPrft tolerates its absence).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace fmp4 {

// "avc1.PPCCLL" from the raw SPS NAL (payload including the NAL header byte).
std::string codec_string(const std::vector<uint8_t>& sps);

// ftyp + moov. width/height are the coded dimensions (from the encoder
// configuration); timescale is the media timescale used by fragments.
std::vector<uint8_t> init_segment(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                                  int width, int height, uint32_t timescale);

// AP15: a ProducerReferenceTime box (ISO 14496-12), 32 bytes, version 1.
// Prepended to a fragment it tells the player the wall-clock instant the frame
// was captured; upstream preview.js readPrft() samples it for its latency
// read-out and then appends from the moof that follows. `ntp` is an NTP
// timestamp (seconds since 1900 in the high 32 bits, binary fraction in the
// low 32). Emitted only when the camera clock is plausible - a camera without
// a set clock would otherwise report an invented latency.
std::vector<uint8_t> prft(uint32_t track_id, uint64_t ntp, uint64_t media_time);

// One frame: moof + mdat. `decode_time` and `duration` are in the init
// segment's timescale; `sample` is the AVCC-converted access unit.
std::vector<uint8_t> fragment(uint32_t sequence, uint64_t decode_time, uint32_t duration,
                              const std::vector<uint8_t>& sample, bool key);

// Annex-B access unit -> AVCC (4-byte big-endian lengths). SPS/PPS/AUD NALs
// are dropped - parameter sets live in the init segment's avcC, and repeating
// them inside an avc1 track is not what the byte-stream parser expects.
std::vector<uint8_t> annexb_to_avcc(const uint8_t* data, size_t len);

}} // namespace machino::fmp4
