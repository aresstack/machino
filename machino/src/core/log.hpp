// Machino core: tiny leveled logger (stderr + optional file + optional
// syslog). Bounded line length, no allocation on the hot path.
#pragma once
#include <cstdarg>
#include <cstddef>

namespace machino {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void log_set_level(LogLevel lvl);
LogLevel log_level();

// `ident` is the syslog program name. In drop-in mode it must be "majestic":
// the stock log viewer filters its "majestic" source on that exact token, and
// the process already renames itself for pidof/killall compatibility.
void log_set_syslog(bool on, const char* ident = "machino");

// Own file sink, independent of stdout/stderr: busybox start-stop-daemon -b
// forces the daemon's stdio to /dev/null, so a redirect on the launcher never
// reaches us. Rotated at `max_bytes` keeping one previous generation, so a
// long-running camera cannot fill a small overlay. Path from MACHINO_LOGFILE
// when this is not called explicitly.
void log_set_file(const char* path, size_t max_bytes = 256 * 1024);

void log_write(LogLevel lvl, const char* mod, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

} // namespace machino

#define LOGE(mod, ...) ::machino::log_write(::machino::LogLevel::Error, mod, __VA_ARGS__)
#define LOGW(mod, ...) ::machino::log_write(::machino::LogLevel::Warn,  mod, __VA_ARGS__)
#define LOGI(mod, ...) ::machino::log_write(::machino::LogLevel::Info,  mod, __VA_ARGS__)
#define LOGD(mod, ...) do { if (::machino::log_level() >= ::machino::LogLevel::Debug) \
                              ::machino::log_write(::machino::LogLevel::Debug, mod, __VA_ARGS__); } while (0)
