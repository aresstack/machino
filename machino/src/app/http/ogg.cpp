#include "app/http/ogg.hpp"

namespace machino { namespace ogg {

uint32_t crc(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
            table[i] = r;
        }
        init = true;
    }
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) c = (c << 8) ^ table[((c >> 24) ^ p[i]) & 0xff];
    return c;
}

static void le16(std::string& s, uint16_t v) { s += (char)(v & 0xff); s += (char)(v >> 8); }
static void le32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xff); }
static void le64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s += (char)((v >> (8 * i)) & 0xff); }

std::string OpusWriter::page(const std::string& body, uint8_t flags, uint64_t granule) {
    // Lacing: 255-byte segments, the last one shorter (a body that is an exact
    // multiple of 255 ends with a zero-length segment). One packet per page,
    // and every packet here is far below the 255*255 page limit.
    std::string lacing;
    size_t left = body.size();
    while (left >= 255) { lacing += (char)255; left -= 255; }
    lacing += (char)left;
    std::string p = "OggS";
    p += (char)0;                                   // version
    p += (char)flags;
    le64(p, granule);
    le32(p, serial_);
    le32(p, seq_++);
    le32(p, 0);                                     // CRC, filled below
    p += (char)lacing.size();
    p += lacing;
    p += body;
    const uint32_t c = crc(reinterpret_cast<const uint8_t*>(p.data()), p.size());
    for (int i = 0; i < 4; ++i) p[22 + i] = (char)((c >> (8 * i)) & 0xff);
    return p;
}

std::string OpusWriter::headers() {
    std::string head = "OpusHead";
    head += (char)1;                                // version
    head += (char)1;                                // channels
    le16(head, pre_skip_);
    le32(head, (uint32_t)rate_);                    // input rate, informational
    le16(head, 0);                                  // output gain
    head += (char)0;                                // mapping family 0: mono/stereo
    std::string tags = "OpusTags";
    const std::string vendor = "machino";
    le32(tags, (uint32_t)vendor.size());
    tags += vendor;
    le32(tags, 0);                                  // no user comments
    std::string out = page(head, 0x02, 0);          // beginning of stream
    out += page(tags, 0x00, 0);
    return out;
}

std::string OpusWriter::packet(const std::vector<uint8_t>& opus, uint32_t samples48) {
    granule_ += samples48;
    return page(std::string(opus.begin(), opus.end()), 0x00, granule_);
}

}} // namespace machino::ogg
