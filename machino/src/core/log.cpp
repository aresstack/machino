#include "core/log.hpp"
#include "core/log_file.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <syslog.h>

namespace machino {

static LogLevel g_level = LogLevel::Info;
static bool     g_syslog = false;

void log_set_level(LogLevel lvl) { g_level = lvl; }
LogLevel log_level() { return g_level; }

void log_set_syslog(bool on, const char* ident) {
    if (on && !g_syslog) openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    if (!on && g_syslog) closelog();
    g_syslog = on;
}

void log_set_file(const char* path, size_t max_bytes) { log_file_open(path, max_bytes); }

static const char* lvl_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "ERR";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Info:  return "INF";
        case LogLevel::Debug: return "DBG";
    }
    return "???";
}

void log_write(LogLevel lvl, const char* mod, const char* fmt, ...) {
    if (lvl > g_level) return;
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    char line[640];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%s] %-8s %s\n",
                           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000),
                           lvl_tag(lvl), mod, msg);
    fputs(line, stderr);

    log_file_write(line);
    if (g_syslog) {
        int p = lvl == LogLevel::Error ? LOG_ERR : lvl == LogLevel::Warn ? LOG_WARNING
              : lvl == LogLevel::Info ? LOG_INFO : LOG_DEBUG;
        syslog(p, "%s: %s", mod, msg);
    }
}

} // namespace machino
