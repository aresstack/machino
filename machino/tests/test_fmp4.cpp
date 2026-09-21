// fMP4 muxer for the majestic /ws/video contract: structural checks a browser
// byte-stream parser would enforce (box sizes walk exactly, avcC carries the
// parameter sets, trun's data offset lands on the mdat payload).
#include "app/http/fmp4.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define FCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
    return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) | b[at + 3];
}

// Top-level boxes must tile the buffer exactly; returns the concatenated types.
std::string walk(const std::vector<uint8_t>& b) {
    std::string types;
    size_t p = 0;
    while (p + 8 <= b.size()) {
        uint32_t size = rd32(b, p);
        if (size < 8 || p + size > b.size()) return types + "!bad";
        types.append((const char*)&b[p + 4], 4);
        p += size;
    }
    if (p != b.size()) types += "!tail";
    return types;
}

bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(&hay[i], needle.data(), needle.size()) == 0) return true;
    return false;
}

bool has_tag(const std::vector<uint8_t>& b, const char* t) {
    return contains(b, std::vector<uint8_t>(t, t + 4));
}

} // namespace

void run_fmp4_tests() {
    // Real-shaped parameter sets (payloads incl. the NAL header byte).
    const std::vector<uint8_t> sps = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50};
    const std::vector<uint8_t> pps = {0x68, 0xeb, 0xe3, 0xcb};

    FCHECK(fmp4::codec_string(sps) == "avc1.64001f");

    // Annex-B -> AVCC: SPS/PPS/AUD dropped, slices carried with 4-byte lengths.
    std::vector<uint8_t> au;
    auto add = [&](std::initializer_list<uint8_t> nal) {
        const uint8_t sc[4] = {0, 0, 0, 1};
        au.insert(au.end(), sc, sc + 4);
        au.insert(au.end(), nal.begin(), nal.end());
    };
    add({0x09, 0x10});                    // AUD - dropped
    add({0x67, 0x64, 0x00, 0x1f});        // SPS - dropped (lives in avcC)
    add({0x68, 0xeb});                    // PPS - dropped
    add({0x65, 0x88, 0x80, 0x11});        // IDR slice - kept
    add({0x06, 0x05, 0x01});              // SEI - kept
    std::vector<uint8_t> avcc = fmp4::annexb_to_avcc(au.data(), au.size());
    FCHECK(avcc.size() == 4 + 4 + 4 + 3);
    FCHECK(rd32(avcc, 0) == 4 && avcc[4] == 0x65);
    FCHECK(rd32(avcc, 8) == 3 && avcc[12] == 0x06);

    // Init segment: ftyp+moov tile exactly; avc1/avcC/trex present; the
    // parameter sets are embedded verbatim.
    std::vector<uint8_t> init = fmp4::init_segment(sps, pps, 1920, 1080, 90000);
    FCHECK(walk(init) == "ftypmoov");
    FCHECK(has_tag(init, "avc1") && has_tag(init, "avcC") && has_tag(init, "trex") && has_tag(init, "mvex"));
    FCHECK(contains(init, sps) && contains(init, pps));

    // Fragment: moof+mdat tile exactly; the trun data offset points at the
    // first mdat payload byte; the payload arrives verbatim; key/non-key
    // fragments differ only in the sample flags.
    std::vector<uint8_t> frag = fmp4::fragment(7, 1234567, 4500, avcc, true);
    FCHECK(walk(frag) == "moofmdat");
    uint32_t moof_size = rd32(frag, 0);
    FCHECK(std::memcmp(&frag[moof_size + 8], avcc.data(), avcc.size()) == 0);   // mdat payload
    // find trun, read its data_offset (fullbox 12 bytes + sample count 4)
    size_t trun_at = 0;
    for (size_t i = 0; i + 4 <= frag.size(); ++i)
        if (std::memcmp(&frag[i], "trun", 4) == 0) { trun_at = i - 4; break; }
    FCHECK(trun_at > 0);
    uint32_t data_offset = rd32(frag, trun_at + 12 + 4);
    FCHECK(data_offset == moof_size + 8);
    // sequence number sits in mfhd (fullbox 12 after the header)
    size_t mfhd_at = 0;
    for (size_t i = 0; i + 4 <= frag.size(); ++i)
        if (std::memcmp(&frag[i], "mfhd", 4) == 0) { mfhd_at = i - 4; break; }
    FCHECK(rd32(frag, mfhd_at + 12) == 7);

    std::vector<uint8_t> frag2 = fmp4::fragment(8, 1239067, 4500, avcc, false);
    FCHECK(walk(frag2) == "moofmdat");
    FCHECK(frag2.size() == frag.size());
    FCHECK(has_tag(frag, "tfdt") && has_tag(frag, "tfhd"));
    // the only difference besides seq/tfdt is the sample flags word
    FCHECK(!std::equal(frag.begin(), frag.end(), frag2.begin()));
}
