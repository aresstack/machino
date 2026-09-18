// Machino core: tiny leveled logger (stderr + optional syslog). Bounded line
// length, no allocation on the hot path.
#pragma once
#include <cstdarg>

namespace machino {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void log_set_level(LogLevel lvl);
void log_set_syslog(bool on);
LogLevel log_level();

void log_write(LogLevel lvl, const char* mod, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

} // namespace machino

#define LOGE(mod, ...) ::machino::log_write(::machino::LogLevel::Error, mod, __VA_ARGS__)
#define LOGW(mod, ...) ::machino::log_write(::machino::LogLevel::Warn,  mod, __VA_ARGS__)
#define LOGI(mod, ...) ::machino::log_write(::machino::LogLevel::Info,  mod, __VA_ARGS__)
#define LOGD(mod, ...) do { if (::machino::log_level() >= ::machino::LogLevel::Debug) \
                              ::machino::log_write(::machino::LogLevel::Debug, mod, __VA_ARGS__); } while (0)
