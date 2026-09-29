#include "app/http/hls.hpp"
#include "app/rtsp/h264_nal.hpp"
#include <cstdio>

namespace machino { namespace hls {

void Segmenter::close_current() {
    if (!cur_open_) return;
    cur_open_ = false;
    if (cur_.empty()) return;
    Seg s;
    s.seq = next_seq_++;
    s.dur90k = (uint32_t)(cur_end_ - cur_start_);
    s.data.swap(cur_);
    s.disc = pending_disc_;
    pending_disc_ = false;
    segs_.push_back(std::move(s));
    cur_.clear();
    trim();
}

// The newest complete segment always stays: a player needs something.
void Segmenter::trim() {
    // By count, and by TOTAL bytes (finished plus the segment in progress).
    // Counting only the finished ones let the window hold max_bytes ON TOP of
    // a cur_ of up to another half budget; together with the floor of two
    // that is bounded at 1.5x max_bytes worst case (feed caps one segment at
    // max_bytes/2), not the ~3x the uncounted variant allowed. Never below
    // two finished segments - a player needs a window.
    auto finished = [this] { size_t n = 0; for (const Seg& s : segs_) n += s.data.size(); return n; };
    while (segs_.size() > lim_.max_segments ||
           (segs_.size() > 2 && finished() + cur_.size() > lim_.max_bytes)) {
        // RFC 8216: dropping a segment that carries the DISCONTINUITY tag
        // MUST increment EXT-X-DISCONTINUITY-SEQUENCE.
        if (segs_.front().disc) ++disc_seq_;
        segs_.pop_front();
    }
}

void Segmenter::feed(const uint8_t* annexb, size_t n, bool key, int64_t pts_us, int width, int height,
                     bool discontinuity) {
    if (!annexb || !n) return;
    if (discontinuity) {                         // the frames in progress lost their references
        cur_.clear(); cur_open_ = false; await_key_ = true;
    }
    if (key) {
        std::vector<uint8_t> sps, pps;
        if (h264::extract_params(annexb, n, sps, pps) && !sps.empty() && !pps.empty() &&
            (sps != sps_ || pps != pps_)) {
            // New parameter sets: a new init; everything held refers to the
            // old one, so the window restarts behind a discontinuity.
            if (!init_.empty()) {
                // Die weggeworfenen Segmente verlassen die Playlist: ein noch
                // im Fenster stehender Disc-Tag zaehlt dabei in die Sequence
                // (gleiche MUST-Regel wie beim Trimmen).
                for (const Seg& s : segs_) if (s.disc) ++disc_seq_;
                segs_.clear(); cur_.clear(); cur_open_ = false; pending_disc_ = true; ++init_gen_;
            }
            sps_ = sps; pps_ = pps;
            init_ = fmp4::init_segment(sps_, pps_, width, height, 90000);
        }
        if (init_.empty()) return;
        await_key_ = false;
    }
    if (await_key_ || init_.empty()) return;

    uint32_t dur = 0;
    const uint64_t dts = tl_.next(pts_us, dur);
    if (key && cur_open_ && (dts - cur_start_) * 1000 / 90000 >= (uint64_t)lim_.target_ms) close_current();
    if (!cur_open_) {
        if (!key) return;                         // a segment starts on a key frame
        cur_open_ = true; cur_start_ = dts;
    }
    const std::vector<uint8_t> sample = fmp4::annexb_to_avcc(annexb, n);
    if (sample.empty()) return;
    const std::vector<uint8_t> f = fmp4::fragment(frag_seq_++, dts, dur, sample, key);
    cur_.insert(cur_.end(), f.begin(), f.end());
    cur_end_ = dts + dur;
    trim();
    // A GOP far longer than the target must not grow without bound: half the
    // byte budget per segment (as the base had it). A full-budget segment
    // plus the two-finished floor held up to ~3x the window in RAM - on a
    // 48 MB camera that is an OOM, not a feature.
    if (cur_.size() > lim_.max_bytes / 2) { cur_.clear(); cur_open_ = false; await_key_ = true; }
}

std::string Segmenter::playlist(const std::string& prefix) const {
    uint32_t maxd = 0;
    for (const Seg& s : segs_) if (s.dur90k > maxd) maxd = s.dur90k;
    const unsigned target = (unsigned)((maxd + 89999) / 90000);
    char b[160];
    std::string p = "#EXTM3U\n#EXT-X-VERSION:7\n";
    snprintf(b, sizeof b, "#EXT-X-TARGETDURATION:%u\n#EXT-X-MEDIA-SEQUENCE:%llu\n",
             target ? target : 1, (unsigned long long)first_seq());
    p += b;
    p += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    // Der Tag reist mit dem ersten Segment der neuen Generation (unten);
    // die SEQUENCE zaehlt nur ENTFERNTE Discontinuities (RFC 8216). Beides
    // zusammen aus init_gen_ abzuleiten zaehlte doppelt und liess die Nummer
    // eines Segments zwischen zwei Reloads SINKEN, sobald das getaggte
    // Segment aus dem Fenster fiel - hls.js/Safari resetten dann die Timeline.
    if (disc_seq_) { snprintf(b, sizeof b, "#EXT-X-DISCONTINUITY-SEQUENCE:%u\n", disc_seq_); p += b; }
    p += "#EXT-X-MAP:URI=\"" + prefix + init_name() + "\"\n";
    for (const Seg& s : segs_) {
        if (s.disc) p += "#EXT-X-DISCONTINUITY\n";
        snprintf(b, sizeof b, "#EXTINF:%.3f,\n%sseg%llu.m4s\n", s.dur90k / 90000.0, prefix.c_str(),
                 (unsigned long long)s.seq);
        p += b;
    }
    return p;
}

std::string Segmenter::init_name() const {
    return init_gen_ ? "init" + std::to_string(init_gen_) + ".mp4" : "init.mp4";
}

bool Segmenter::parse_init_name(const std::string& name, unsigned& gen) {
    if (name == "init.mp4") { gen = 0; return true; }
    if (name.size() < 9 || name.compare(0, 4, "init") != 0 || name.compare(name.size() - 4, 4, ".mp4") != 0) return false;
    unsigned v = 0;
    for (size_t i = 4; i < name.size() - 4; ++i) {
        if (name[i] < '0' || name[i] > '9' || i > 12) return false;
        v = v * 10 + (unsigned)(name[i] - '0');
    }
    gen = v;
    return true;
}

bool Segmenter::segment(uint64_t seq, std::vector<uint8_t>& out) const {
    for (const Seg& s : segs_) if (s.seq == seq) { out = s.data; return true; }
    return false;
}

bool Segmenter::parse_segment_name(const std::string& name, uint64_t& seq) {
    if (name.size() < 8 || name.compare(0, 3, "seg") != 0 || name.compare(name.size() - 4, 4, ".m4s") != 0) return false;
    uint64_t v = 0;
    for (size_t i = 3; i < name.size() - 4; ++i) {
        if (name[i] < '0' || name[i] > '9' || i > 22) return false;
        v = v * 10 + (uint64_t)(name[i] - '0');
    }
    seq = v;
    return true;
}

size_t Segmenter::held_bytes() const {
    size_t n = cur_.size();
    for (const Seg& s : segs_) n += s.data.size();
    return n;
}

}} // namespace machino::hls
