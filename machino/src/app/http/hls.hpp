// Application: HLS for majestic's /hls - fMP4 segments (RFC 8216 version 7,
// EXT-X-MAP), held in RAM, cut at key frames. Pure, host-tested; the server
// feeds it access units and serves what it holds.
//
// Memory is the constraint on this camera, so the window is short (a few
// segments) and bounded in bytes as well as count: the oldest segment goes
// first when either limit is hit. Each frame is its own moof+mdat inside the
// segment - valid CMAF, and the same fragments /ws/video sends.
#pragma once
#include "app/http/fmp4.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace machino { namespace hls {

class Segmenter {
public:
    struct Limits {
        int    target_ms = 2000;        // cut at the first key frame after this
        size_t max_segments = 4;
        size_t max_bytes = 6u << 20;    // all held segments together
    };
    Segmenter() : Segmenter(Limits{}) {}
    explicit Segmenter(Limits l) : lim_(l) {}

    // One Annex-B access unit. `discontinuity`: frames were dropped before it
    // (the segment in progress is abandoned; the next one starts at a key).
    void feed(const uint8_t* annexb, size_t n, bool key, int64_t pts_us, int width, int height,
              bool discontinuity = false);

    bool ready() const { return !segs_.empty(); }
    // The live playlist; `prefix` is prepended to every URI ("" = relative).
    std::string playlist(const std::string& prefix = "") const;
    // init.mp4 (current parameter sets); empty before the first key frame.
    const std::vector<uint8_t>& init() const { return init_; }
    // "seg<N>.m4s" -> the bytes, while still held.
    bool segment(uint64_t seq, std::vector<uint8_t>& out) const;
    // Parses "seg<N>.m4s"; false for any other name.
    static bool parse_segment_name(const std::string& name, uint64_t& seq);

    size_t held_bytes() const;
    uint64_t first_seq() const { return segs_.empty() ? next_seq_ : segs_.front().seq; }

private:
    struct Seg { uint64_t seq; uint32_t dur90k; std::vector<uint8_t> data; bool disc; };
    void close_current();
    void trim();

    Limits lim_;
    std::vector<uint8_t> init_, sps_, pps_;
    std::deque<Seg> segs_;
    std::vector<uint8_t> cur_;
    uint64_t cur_start_ = 0, cur_end_ = 0;
    bool     cur_open_ = false;
    bool     await_key_ = true;
    bool     pending_disc_ = false;   // the next segment follows a new init
    uint64_t next_seq_ = 0;
    uint32_t frag_seq_ = 1;
    fmp4::Timeline tl_;
};

}} // namespace machino::hls
