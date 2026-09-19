// Port: hardware JPEG encoding. One capture = one current frame as a complete
// JFIF/JPEG byte stream. The adapter owns channel/group allocation and any
// vendor structures; the core sees bytes.
//
// The instance is ephemeral by design ("no consumer, no pipeline"): the
// pipeline manager creates it on snapshot demand and destroys it after a short
// grace, it is never kept alive merely because the API endpoint exists.
#pragma once
#include "core/result.hpp"
#include <cstdint>
#include <vector>

namespace machino {

struct JpegParams {
    int width   = 0;      // 0 = the platform's main-stream size
    int height  = 0;
    int quality = 80;     // 1..99
    int buffers = 1;
};

class IJpegEncoder {
public:
    virtual ~IJpegEncoder() = default;
    // Blocks up to timeout_ms for a fresh frame. `out` is replaced.
    virtual Result capture(std::vector<uint8_t>& out, int timeout_ms) = 0;
    virtual int    channel() const = 0;
};

} // namespace machino
