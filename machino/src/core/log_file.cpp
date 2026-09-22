// THREAD SAFETY. Every thread in the daemon logs: the HTTP poll loop, one
// thread per RTSP client, the pipeline, and the WS-Discovery responder. stdio
// locks a FILE* internally, so concurrent fputs would have been fine on its
// own - but rotate() CLOSES that FILE* and opens another, and a thread already
// inside fputs on the old one is then writing through a freed handle. The byte
// counter was a plain read-modify-write across those same threads.
//
// One mutex over the whole sink. Logging is per event, not per frame, so the
// cost is nothing next to the fflush that is already there.
#include "core/log_file.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace machino {

namespace {

// Binary append: the sink writes its own newlines, so a host that would
// translate them (Windows, where the unit tests run) must not.
FILE* open_append(const std::string& p) {
    FILE* f = fopen(p.c_str(), "ab");
#ifndef _WIN32
    // do not leak the log into the logread child we fork for /ws/logs
    if (f) fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
#endif
    return f;
}

FILE*       g_file = nullptr;
bool        g_tried = false;
std::string g_path;
size_t      g_max = 256 * 1024;
size_t      g_bytes = 0;

// Guards every global above. Recursive is not needed: nothing under the lock
// logs.
std::mutex& mu() { static std::mutex m; return m; }

// Keep exactly one previous generation: <path> and <path>.1. Two small files
// bound the worst case at 2 * max_bytes, which a small overlay can live with;
// a full rotation set would only cost more flash writes.
void rotate() {
    if (!g_file) return;
    fclose(g_file);
    g_file = nullptr;
    const std::string prev = g_path + ".1";
    remove(prev.c_str());
    rename(g_path.c_str(), prev.c_str());
    g_file = open_append(g_path);
    g_bytes = 0;
}

void open_once() {
    if (g_tried) return;
    g_tried = true;
    if (g_path.empty()) {
        const char* env = getenv("MACHINO_LOGFILE");
        if (!env || !*env) return;
        g_path = env;
    }
    g_file = open_append(g_path);
    if (!g_file) return;
    const long pos = ftell(g_file);
    g_bytes = pos > 0 ? (size_t)pos : 0;
}
} // namespace

void log_file_open(const char* path, size_t max_bytes) {
    std::lock_guard<std::mutex> lk(mu());
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_path = path ? path : "";
    g_max = max_bytes;
    g_tried = false;
    g_bytes = 0;
}

void log_file_close() {
    std::lock_guard<std::mutex> lk(mu());
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_tried = true;
}

size_t log_file_bytes() {
    std::lock_guard<std::mutex> lk(mu());
    return g_bytes;
}

void log_file_write(const char* line) {
    std::lock_guard<std::mutex> lk(mu());
    open_once();
    if (!g_file || !line) return;
    const size_t n = strlen(line);
    fputs(line, g_file);
    fflush(g_file);
    g_bytes += n;
    if (g_max && g_bytes >= g_max) rotate();
}

} // namespace machino
