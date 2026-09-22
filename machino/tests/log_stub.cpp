// Host-test logger: stderr only (no syslog dependency on non-Linux hosts).
#include "core/log.hpp"
#include "core/log_file.hpp"
#include <cstdio>

namespace machino {

static LogLevel g_level = LogLevel::Warn;
void log_set_level(LogLevel lvl) { g_level = lvl; }
LogLevel log_level() { return g_level; }
void log_set_syslog(bool, const char*) {}
void log_set_file(const char* p, size_t max) { log_file_open(p, max); }

void log_write(LogLevel lvl, const char* mod, const char* fmt, ...) {
    if (lvl > g_level) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%d] %-8s %s\n", (int)lvl, mod, msg);
}

} // namespace machino
