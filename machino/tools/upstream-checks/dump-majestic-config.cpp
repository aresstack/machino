// Prints the majestic-shaped config THIS build serves, so the upstream
// checkers next door can be run against it without a camera and without
// restarting a daemon that must not be restarted.
//
// It is called with an EMPTY native config, so what it prints is the part the
// compat layer adds by itself - jpeg, nightMode, audio. The sections copied
// from a real configuration (video0, sensor, image, ...) are absent here by
// construction, and their absence is not a finding.
//
//   g++ -std=c++17 -O1 -fno-exceptions -fno-rtti -Isrc -o dump
//       tools/upstream-checks/dump-majestic-config.cpp
//       src/app/compat/majestic_webui.cpp src/core/json.cpp
//   ./dump > cfg.json
//
// (one line; the continuations are left out because a backslash at the end of
// a // comment splices the next line into it, and -Werror catches that)
#include "app/compat/majestic_webui.hpp"
#include <cstdio>

int main() {
    using namespace machino;
    const Json out = compat::majestic_config(Json::object(), Json::object());
    std::printf("%s\n", out.dump().c_str());
    return 0;
}
