// Application: Ogg encapsulation of Opus (RFC 3533 pages, RFC 7845 mapping)
// for majestic's /audio.opus - what `ffplay http://cam/audio.opus`, VLC and a
// browser <audio> element play. Pure, host-tested.
//
// The stream is: a BOS page with OpusHead, a page with OpusTags, then one
// page per Opus packet (20 ms). Granule positions count 48 kHz samples, as
// RFC 7845 fixes regardless of the input rate.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace ogg {

// CRC-32 as Ogg defines it (polynomial 0x04c11db7, no reflection, init 0).
uint32_t crc(const uint8_t* p, size_t n);

class OpusWriter {
public:
    OpusWriter(uint32_t serial, int input_rate, uint16_t pre_skip)
        : serial_(serial), rate_(input_rate), pre_skip_(pre_skip) {}
    // OpusHead + OpusTags pages; call once, before the first packet.
    std::string headers();
    // One page carrying one packet of `samples48` 48 kHz samples.
    std::string packet(const std::vector<uint8_t>& opus, uint32_t samples48);

private:
    std::string page(const std::string& body, uint8_t flags, uint64_t granule);
    uint32_t serial_;
    int      rate_;
    uint16_t pre_skip_;
    uint32_t seq_ = 0;
    uint64_t granule_ = 0;
};

}} // namespace machino::ogg
