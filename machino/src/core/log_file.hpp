// The logger's own file sink, split out so it contains no syslog (and so the
// host tests exercise the shipped code rather than a stub).
//
// Why a sink of our own at all: busybox start-stop-daemon -b forces the
// daemon's stdio to /dev/null, so a shell redirect on the launcher never
// reaches us - an fd we open ourselves does. Rotated at `max_bytes` keeping
// one previous generation, so a long-running camera cannot fill a small
// overlay.
#pragma once
#include <cstddef>

namespace machino {

// Explicit path wins; otherwise MACHINO_LOGFILE is used on first write.
// max_bytes 0 = never rotate.
void log_file_open(const char* path, size_t max_bytes = 256 * 1024);

// Appends one already-formatted line and rotates when the file grew past the
// cap. No-op when no path was configured.
void log_file_write(const char* line);

void log_file_close();

// current size of the live file, for tests/diagnostics
size_t log_file_bytes();

} // namespace machino
