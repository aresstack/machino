// Machino core: an encoded access unit (one frame, Annex-B byte stream with
// start codes) and a fixed-size pool of them. The encoder adapter fills a
// pooled unit in place (vector capacity is reused), StreamHub fans it out as
// shared_ptr<const>. No per-frame heap allocation once the pool is warm;
// when every slot is still referenced by slow consumers the frame is dropped.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace machino {

struct AccessUnit {
    std::vector<uint8_t> data;   // Annex-B (00 00 00 01 NAL ...)
    int64_t  pts_us = 0;         // platform timestamp, microseconds (capture time domain)
    int64_t  fetched_us = 0;     // CLOCK_MONOTONIC when the encoder handed it over (latency accounting)
    bool     key    = false;     // contains an IDR slice
    uint32_t seq    = 0;         // encoder frame sequence
};

using AuPtr = std::shared_ptr<const AccessUnit>;

class AuPool {
public:
    AuPool(size_t slots, size_t reserve_bytes) {
        slots_.reserve(slots);
        for (size_t i = 0; i < slots; ++i) {
            auto au = std::make_shared<AccessUnit>();
            au->data.reserve(reserve_bytes);
            slots_.push_back(au);
        }
    }
    // A slot nobody else references, or nullptr (counted) when all are busy.
    std::shared_ptr<AccessUnit> acquire() {
        for (size_t n = 0; n < slots_.size(); ++n) {
            auto& s = slots_[next_];
            next_ = (next_ + 1) % slots_.size();
            if (s.use_count() == 1) return s;
        }
        ++exhausted_;
        return nullptr;
    }
    unsigned exhausted() const { return exhausted_; }
private:
    std::vector<std::shared_ptr<AccessUnit>> slots_;
    size_t   next_ = 0;
    unsigned exhausted_ = 0;
};

} // namespace machino
