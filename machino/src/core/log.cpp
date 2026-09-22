#include "core/log.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <syslog.h>

namespace machino {

static LogLevel g_level = LogLevel::Info;
static bool     g_syslog = false;

// Optional process-owned file sink. busybox start-stop-daemon -b forces the
// daemon's stdio to /dev/null, so a shell redirect on the launcher never
// reaches us; an fd we open ourselves does. Path from MACHINO_LOGFILE, opened
// lazily on first log; absent env = no file (unchanged production default).
static FILE* g_file = nullptr;
static bool  g_file_tried = false;

void log_set_level(LogLevel lvl) { g_level = lvl; }
LogLevel log_level() { return g_level; }
void log_set_syslog(bool on) {
    if (on && !g_syslog) openlog("machino", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    if (!on && g_syslog) closelog();
    g_syslog = on;
}

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
    if (!g_file_tried) {
        g_file_tried = true;
        if (const char* p = getenv("MACHINO_LOGFILE")) g_file = fopen(p, "ae");
    }
    if (g_file) { fputs(line, g_file); fflush(g_file); }
    if (g_syslog) {
        int p = lvl == LogLevel::Error ? LOG_ERR : lvl == LogLevel::Warn ? LOG_WARNING
              : lvl == LogLevel::Info ? LOG_INFO : LOG_DEBUG;
        syslog(p, "%s: %s", mod, msg);
    }
}

} // namespace machino
