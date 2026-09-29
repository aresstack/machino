#include "app/http/fmp4.hpp"
#include "app/rtsp/h264_nal.hpp"
#include <cstdio>
#include <cstring>

namespace machino { namespace fmp4 {

namespace {

void be16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
void be64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 7; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
void tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
void bytes(std::vector<uint8_t>& b, const void* p, size_t n) { const uint8_t* u = (const uint8_t*)p; b.insert(b.end(), u, u + n); }
void zeros(std::vector<uint8_t>& b, size_t n) { b.insert(b.end(), n, 0); }

// An ISO box: size placeholder now, patched when the payload is complete.
size_t open_box(std::vector<uint8_t>& b, const char* type) {
    size_t at = b.size();
    be32(b, 0);
    tag(b, type);
    return at;
}
void close_box(std::vector<uint8_t>& b, size_t at) {
    uint32_t size = (uint32_t)(b.size() - at);
    b[at] = (uint8_t)(size >> 24); b[at + 1] = (uint8_t)(size >> 16);
    b[at + 2] = (uint8_t)(size >> 8); b[at + 3] = (uint8_t)size;
}
size_t open_full(std::vector<uint8_t>& b, const char* type, uint8_t version, uint32_t flags) {
    size_t at = open_box(b, type);
    b.push_back(version);
    b.push_back((uint8_t)(flags >> 16)); b.push_back((uint8_t)(flags >> 8)); b.push_back((uint8_t)flags);
    return at;
}

} // namespace

std::string codec_string(const std::vector<uint8_t>& sps) {
    if (sps.size() < 4) return "avc1.42e01e";     // baseline 3.0: never invented silently in practice, only a guard
    char out[16];
    std::snprintf(out, sizeof out, "avc1.%02x%02x%02x", sps[1], sps[2], sps[3]);
    return out;
}

std::vector<uint8_t> annexb_to_avcc(const uint8_t* data, size_t len) {
    h264::Nal nals[64];
    size_t n = h264::split(data, len, nals, 64);
    std::vector<uint8_t> out;
    out.reserve(len + 8);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t t = nals[i].type;
        if (t == 7 || t == 8 || t == 9) continue;   // SPS/PPS live in avcC; AUD is byte-stream only
        be32(out, (uint32_t)nals[i].len);
        bytes(out, nals[i].p, nals[i].len);
    }
    return out;
}

namespace {

// A descriptor tag + its one-byte length (every descriptor here is < 128).
void desc(std::vector<uint8_t>& b, uint8_t tagv, size_t len) { b.push_back(tagv); b.push_back((uint8_t)len); }

void ftyp_box(std::vector<uint8_t>& b) {
    // ftyp: iso5 carries the movie-fragment defaults this stream relies on.
    size_t ftyp = open_box(b, "ftyp");
    tag(b, "iso5"); be32(b, 1);
    tag(b, "iso5"); tag(b, "iso6"); tag(b, "mp41");
    close_box(b, ftyp);
}

void mvhd_box(std::vector<uint8_t>& b, uint32_t timescale, uint32_t next_track) {
    size_t mvhd = open_full(b, "mvhd", 0, 0);
    be32(b, 0); be32(b, 0);              // creation, modification
    be32(b, timescale); be32(b, 0);      // timescale, duration (live: 0)
    be32(b, 0x00010000); be16(b, 0x0100); zeros(b, 2 + 8); // rate, volume, reserved
    static const uint32_t unity[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : unity) be32(b, v);
    zeros(b, 6 * 4);
    be32(b, next_track);
    close_box(b, mvhd);
}

void dinf_and_empty_tables_open(std::vector<uint8_t>& b) {
    size_t dinf = open_box(b, "dinf");
    size_t dref = open_full(b, "dref", 0, 0);
    be32(b, 1);
    size_t url = open_full(b, "url ", 0, 1);   // self-contained
    close_box(b, url);
    close_box(b, dref);
    close_box(b, dinf);
}

void empty_tables(std::vector<uint8_t>& b) {
    for (const char* t : {"stts", "stsc", "stsz", "stco"}) {
        size_t s = open_full(b, t, 0, 0);
        if (std::strcmp(t, "stsz") == 0) be32(b, 0);  // sample size field
        be32(b, 0);                                    // entry count
        close_box(b, s);
    }
}

void audio_trak(std::vector<uint8_t>& b, const AudioTrack& a) {
    size_t trak = open_box(b, "trak");
    size_t tkhd = open_full(b, "tkhd", 0, 3);
    be32(b, 0); be32(b, 0);
    be32(b, a.track_id); be32(b, 0);
    be32(b, 0); zeros(b, 8);
    be16(b, 0); be16(b, 1);              // layer, alternate group (audio)
    be16(b, 0x0100); be16(b, 0);         // volume 1.0
    static const uint32_t unity[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : unity) be32(b, v);
    be32(b, 0); be32(b, 0);              // no width/height
    close_box(b, tkhd);

    size_t mdia = open_box(b, "mdia");
    size_t mdhd = open_full(b, "mdhd", 0, 0);
    be32(b, 0); be32(b, 0);
    be32(b, a.timescale()); be32(b, 0);
    be16(b, 0x55c4); be16(b, 0);
    close_box(b, mdhd);
    size_t hdlr = open_full(b, "hdlr", 0, 0);
    be32(b, 0); tag(b, "soun"); zeros(b, 12);
    bytes(b, "MachinoAudio", 13);
    close_box(b, hdlr);
    size_t minf = open_box(b, "minf");
    size_t smhd = open_full(b, "smhd", 0, 0);
    be16(b, 0); be16(b, 0);              // balance, reserved
    close_box(b, smhd);
    dinf_and_empty_tables_open(b);
    size_t stbl = open_box(b, "stbl");
    size_t stsd = open_full(b, "stsd", 0, 0);
    be32(b, 1);
    size_t entry = open_box(b, a.codec == AudioTrack::Opus ? "Opus" : "mp4a");
    zeros(b, 6); be16(b, 1);             // reserved, data ref index
    zeros(b, 8);                         // version, revision, vendor
    be16(b, (uint16_t)a.channels); be16(b, 16);
    be16(b, 0); be16(b, 0);              // pre-defined, reserved
    be32(b, (a.codec == AudioTrack::Opus ? 48000u : (uint32_t)a.sample_rate) << 16);
    if (a.codec == AudioTrack::Opus) {
        size_t dops = open_box(b, "dOps");
        b.push_back(0);                  // version
        b.push_back((uint8_t)a.channels);
        be16(b, a.pre_skip);
        be32(b, (uint32_t)a.sample_rate);
        be16(b, 0);                      // output gain
        b.push_back(0);                  // channel mapping family 0
        close_box(b, dops);
    } else {
        size_t esds = open_full(b, "esds", 0, 0);
        const size_t dsi = a.asc.size();
        const size_t dcd = 13 + 2 + dsi;              // DecoderConfigDescriptor payload
        const size_t esd = 3 + 2 + dcd + 2 + 1;       // ES_Descriptor payload
        desc(b, 0x03, esd);
        be16(b, 0); b.push_back(0);                   // ES_ID, flags
        desc(b, 0x04, dcd);
        b.push_back(0x40);                            // object type: MPEG-4 Audio
        b.push_back(0x15);                            // stream type audio (5 << 2 | 1)
        b.push_back(0); be16(b, 0);                   // buffer size DB
        be32(b, 0); be32(b, 0);                       // max / avg bitrate (unstated)
        desc(b, 0x05, dsi);
        bytes(b, a.asc.data(), dsi);
        desc(b, 0x06, 1);
        b.push_back(0x02);                            // SLConfig predefined MP4
        close_box(b, esds);
    }
    close_box(b, entry);
    close_box(b, stsd);
    empty_tables(b);
    close_box(b, stbl);
    close_box(b, minf);
    close_box(b, mdia);
    close_box(b, trak);
}

void trex_box(std::vector<uint8_t>& b, uint32_t track_id) {
    size_t trex = open_full(b, "trex", 0, 0);
    be32(b, track_id);
    be32(b, 1);                          // default sample description
    be32(b, 0); be32(b, 0); be32(b, 0);
    close_box(b, trex);
}

} // namespace

std::vector<uint8_t> audio_init_segment(const AudioTrack& a) {
    std::vector<uint8_t> b;
    ftyp_box(b);
    size_t moov = open_box(b, "moov");
    mvhd_box(b, a.timescale(), a.track_id + 1);
    audio_trak(b, a);
    size_t mvex = open_box(b, "mvex");
    trex_box(b, a.track_id);
    close_box(b, mvex);
    close_box(b, moov);
    return b;
}

std::vector<uint8_t> init_segment(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                                  int width, int height, uint32_t timescale, const AudioTrack* audio) {
    std::vector<uint8_t> b;
    b.reserve(512 + sps.size() + pps.size());
    const bool with_audio = audio && audio->codec != AudioTrack::None;
    ftyp_box(b);

    size_t moov = open_box(b, "moov");
    {
        mvhd_box(b, timescale, with_audio ? audio->track_id + 1 : 2);

        size_t trak = open_box(b, "trak");
        {
            size_t tkhd = open_full(b, "tkhd", 0, 3);   // enabled + in movie
            be32(b, 0); be32(b, 0);
            be32(b, 1); be32(b, 0);          // track id, reserved
            be32(b, 0);                      // duration (live: 0)
            zeros(b, 8);                     // reserved
            be16(b, 0); be16(b, 0);          // layer, alternate group
            be16(b, 0); be16(b, 0);          // volume (video: 0), reserved
            static const uint32_t unity_t[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
            for (uint32_t v : unity_t) be32(b, v);
            be32(b, (uint32_t)width << 16); be32(b, (uint32_t)height << 16);
            close_box(b, tkhd);

            size_t mdia = open_box(b, "mdia");
            {
                size_t mdhd = open_full(b, "mdhd", 0, 0);
                be32(b, 0); be32(b, 0);
                be32(b, timescale); be32(b, 0);
                be16(b, 0x55c4); be16(b, 0);    // language "und"
                close_box(b, mdhd);

                size_t hdlr = open_full(b, "hdlr", 0, 0);
                be32(b, 0); tag(b, "vide"); zeros(b, 12);
                bytes(b, "MachinoVideo", 13);   // includes the terminating NUL
                close_box(b, hdlr);

                size_t minf = open_box(b, "minf");
                {
                    size_t vmhd = open_full(b, "vmhd", 0, 1);
                    zeros(b, 8);
                    close_box(b, vmhd);

                    size_t dinf = open_box(b, "dinf");
                    {
                        size_t dref = open_full(b, "dref", 0, 0);
                        be32(b, 1);
                        size_t url = open_full(b, "url ", 0, 1);   // self-contained
                        close_box(b, url);
                        close_box(b, dref);
                    }
                    close_box(b, dinf);

                    size_t stbl = open_box(b, "stbl");
                    {
                        size_t stsd = open_full(b, "stsd", 0, 0);
                        be32(b, 1);
                        size_t avc1 = open_box(b, "avc1");
                        zeros(b, 6); be16(b, 1);                 // reserved, data ref index
                        zeros(b, 16);                            // pre-defined/reserved
                        be16(b, (uint16_t)width); be16(b, (uint16_t)height);
                        be32(b, 0x00480000); be32(b, 0x00480000); // 72 dpi
                        be32(b, 0);
                        be16(b, 1);                              // frame count
                        zeros(b, 32);                            // compressor name
                        be16(b, 0x0018); be16(b, 0xffff);        // depth, pre-defined
                        size_t avcc = open_box(b, "avcC");
                        b.push_back(1);                          // configuration version
                        b.push_back(sps.size() > 3 ? sps[1] : 0x42);
                        b.push_back(sps.size() > 3 ? sps[2] : 0xe0);
                        b.push_back(sps.size() > 3 ? sps[3] : 0x1e);
                        b.push_back(0xff);                       // 4-byte NAL lengths
                        b.push_back(0xe1);                       // 1 SPS
                        be16(b, (uint16_t)sps.size()); bytes(b, sps.data(), sps.size());
                        b.push_back(1);                          // 1 PPS
                        be16(b, (uint16_t)pps.size()); bytes(b, pps.data(), pps.size());
                        close_box(b, avcc);
                        close_box(b, avc1);
                        close_box(b, stsd);

                        for (const char* t : {"stts", "stsc", "stsz", "stco"}) {
                            size_t s = open_full(b, t, 0, 0);
                            if (std::strcmp(t, "stsz") == 0) be32(b, 0);  // sample size field
                            be32(b, 0);                                    // entry count
                            close_box(b, s);
                        }
                    }
                    close_box(b, stbl);
                }
                close_box(b, minf);
            }
            close_box(b, mdia);
        }
        close_box(b, trak);
        if (with_audio) audio_trak(b, *audio);

        size_t mvex = open_box(b, "mvex");
        trex_box(b, 1);
        if (with_audio) trex_box(b, audio->track_id);
        close_box(b, mvex);
    }
    close_box(b, moov);
    return b;
}

uint64_t Timeline::next(int64_t pts_us, uint32_t& duration_out) {
    int64_t step_us = FALLBACK_STEP_US;
    if (last_us > 0) {
        const int64_t d = pts_us - last_us;
        if (d > MIN_STEP_US && d < MAX_STEP_US) step_us = d;
    }
    // Rounded, not truncated: a truncation here would be the very drift this
    // class exists to remove, reintroduced through the duration.
    duration_out = (uint32_t)((step_us * 90000 + 500000) / 1000000);

    if (!started) {
        origin_us = pts_us; skew_us = 0; started = true;
    } else if (last_us > 0) {
        const int64_t d = pts_us - last_us;
        // A stalled, repeated, backwards or very long step is absorbed, so the
        // timeline advances by one nominal frame and stays contiguous.
        if (d <= MIN_STEP_US || d >= MAX_STEP_US) skew_us += d - step_us;
    }
    last_us = pts_us;

    int64_t tl = pts_us - origin_us - skew_us;
    if (tl < 0) tl = 0;                                 // monotonic, whatever the clock did
    return (uint64_t)((tl * 90000 + 500000) / 1000000);
}

std::vector<uint8_t> prft(uint32_t track_id, uint64_t ntp, uint64_t media_time) {
    std::vector<uint8_t> b;
    size_t at = open_full(b, "prft", 1, 0);
    be32(b, track_id);
    be64(b, ntp);
    be64(b, media_time);
    close_box(b, at);
    return b;                                          // exactly 32 bytes
}

std::vector<uint8_t> fragment(uint32_t sequence, uint64_t decode_time, uint32_t duration,
                              const std::vector<uint8_t>& sample, bool key, uint32_t track_id) {
    std::vector<uint8_t> b;
    b.reserve(sample.size() + 128);

    size_t moof = open_box(b, "moof");
    {
        size_t mfhd = open_full(b, "mfhd", 0, 0);
        be32(b, sequence);
        close_box(b, mfhd);

        size_t traf = open_box(b, "traf");
        {
            size_t tfhd = open_full(b, "tfhd", 0, 0x020000);   // default-base-is-moof
            be32(b, track_id);
            close_box(b, tfhd);

            size_t tfdt = open_full(b, "tfdt", 1, 0);
            be64(b, decode_time);
            close_box(b, tfdt);

            // trun: data offset + per-sample duration/size/flags, one sample.
            size_t trun = open_full(b, "trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
            be32(b, 1);                                         // sample count
            size_t offset_at = b.size();
            be32(b, 0);                                         // data offset (patched below)
            be32(b, duration);
            be32(b, (uint32_t)sample.size());
            // sync sample: I-frame; otherwise depends-on + non-sync.
            be32(b, key ? 0x02000000u : 0x01010000u);
            close_box(b, trun);
            (void)offset_at;
            close_box(b, traf);
            // data offset: from the START of moof to the first mdat payload
            // byte = moof size + mdat header (8).
            uint32_t off = (uint32_t)(b.size() - moof) + 8;
            b[offset_at] = (uint8_t)(off >> 24); b[offset_at + 1] = (uint8_t)(off >> 16);
            b[offset_at + 2] = (uint8_t)(off >> 8); b[offset_at + 3] = (uint8_t)off;
        }
    }
    close_box(b, moof);

    size_t mdat = open_box(b, "mdat");
    bytes(b, sample.data(), sample.size());
    close_box(b, mdat);
    return b;
}

std::vector<uint8_t> heif_avc_still(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                                    int width, int height, const std::vector<uint8_t>& sample) {
    std::vector<uint8_t> b;
    size_t ftyp = open_box(b, "ftyp");
    tag(b, "avci"); be32(b, 0);
    tag(b, "mif1"); tag(b, "avci"); tag(b, "miaf");
    close_box(b, ftyp);

    size_t meta = open_full(b, "meta", 0, 0);
    size_t hdlr = open_full(b, "hdlr", 0, 0);
    be32(b, 0); tag(b, "pict"); zeros(b, 12); b.push_back(0);
    close_box(b, hdlr);
    size_t pitm = open_full(b, "pitm", 0, 0);
    be16(b, 1);
    close_box(b, pitm);
    size_t iloc = open_full(b, "iloc", 0, 0);
    b.push_back(0x44);                       // offset_size 4, length_size 4
    b.push_back(0x00);                       // base_offset_size 0, reserved
    be16(b, 1);                              // item count
    be16(b, 1);                              // item id
    be16(b, 0);                              // data reference: this file
    be16(b, 1);                              // extent count
    const size_t extent_offset_at = b.size();
    be32(b, 0);                              // patched once the mdat position is known
    be32(b, (uint32_t)sample.size());
    close_box(b, iloc);
    size_t iinf = open_full(b, "iinf", 0, 0);
    be16(b, 1);
    size_t infe = open_full(b, "infe", 2, 0);
    be16(b, 1); be16(b, 0); tag(b, "avc1"); b.push_back(0);
    close_box(b, infe);
    close_box(b, iinf);
    size_t iprp = open_box(b, "iprp");
    size_t ipco = open_box(b, "ipco");
    size_t avcc = open_box(b, "avcC");
    b.push_back(1);
    b.push_back(sps.size() > 3 ? sps[1] : 0x42);
    b.push_back(sps.size() > 3 ? sps[2] : 0xe0);
    b.push_back(sps.size() > 3 ? sps[3] : 0x1e);
    b.push_back(0xff);
    b.push_back(0xe1);
    be16(b, (uint16_t)sps.size()); bytes(b, sps.data(), sps.size());
    b.push_back(1);
    be16(b, (uint16_t)pps.size()); bytes(b, pps.data(), pps.size());
    // ISO/IEC 14496-15: the High profiles carry chroma format and bit depths
    // in the record too, and HEIF readers (libheif) insist on them. The
    // encoder makes 8-bit 4:2:0.
    const uint8_t prof = sps.size() > 1 ? sps[1] : 0;
    if (prof == 100 || prof == 110 || prof == 122 || prof == 144) {
        b.push_back(0xfc | 1);                 // chroma_format_idc 1 (4:2:0)
        b.push_back(0xf8 | 0);                 // bit_depth_luma_minus8
        b.push_back(0xf8 | 0);                 // bit_depth_chroma_minus8
        b.push_back(0);                        // no SPS extensions
    }
    close_box(b, avcc);
    size_t ispe = open_full(b, "ispe", 0, 0);
    be32(b, (uint32_t)width); be32(b, (uint32_t)height);
    close_box(b, ispe);
    close_box(b, ipco);
    size_t ipma = open_full(b, "ipma", 0, 0);
    be32(b, 1);                              // entry count
    be16(b, 1);                              // item id
    b.push_back(2);                          // associations
    b.push_back(0x81);                       // property 1 (avcC), essential
    b.push_back(0x02);                       // property 2 (ispe)
    close_box(b, ipma);
    close_box(b, iprp);
    close_box(b, meta);

    size_t mdat = open_box(b, "mdat");
    const uint32_t payload_at = (uint32_t)b.size();
    bytes(b, sample.data(), sample.size());
    close_box(b, mdat);
    for (int i = 0; i < 4; ++i) b[extent_offset_at + i] = (uint8_t)(payload_at >> (24 - 8 * i));
    return b;
}

}} // namespace machino::fmp4
