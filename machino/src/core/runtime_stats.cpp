#include "core/runtime_stats.hpp"

namespace machino {

RuntimeStats& RuntimeStats::get() {
    static RuntimeStats s;
    return s;
}

} // namespace machino
