// One-way migration: an existing OpenIPC majestic.yaml -> a native machino
// config. This is a MIGRATION, not a live bridge (that is majestic_webui.*):
// it runs once at install/import time and is deliberately lossy and honest.
// Every source key is classified, so an operator can see exactly what carried
// over, what was converted, and what majestic did that machino does not.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace machino { namespace compat {

enum class Disposition {
    Mapped,       // copied 1:1 to a machino key
    Converted,    // carried over but the value was transformed (units/scale)
    Ignored,      // machino handles this elsewhere, so the value is dropped safely
    Unsupported,  // a real majestic feature machino does not implement
    Invalid,      // present but unusable (bad value, or needs data we do not have)
};
const char* disposition_name(Disposition d);

struct MigrationEntry {
    std::string source_key;     // dotted majestic key, e.g. "video0.bitrate"
    std::string source_value;
    std::string target_key;     // machino config key (empty unless Mapped/Converted)
    std::string value;          // machino value (empty unless Mapped/Converted)
    Disposition disp = Disposition::Ignored;
    std::string note;
};

struct MigrationResult {
    bool ok = false;                 // false only on a fundamentally unparseable document
    std::string error;               // set when !ok
    std::vector<MigrationEntry> entries;
    int mapped = 0, converted = 0, ignored = 0, unsupported = 0, invalid = 0;
    std::vector<std::pair<std::string, std::string>> config;   // machino key=value to write (Mapped + Converted)
};

// Parse and classify. The parser handles the indentation-based subset majestic
// uses (nested maps, scalar leaves, comments, quotes); lists and anchors are
// recorded as unsupported rather than guessed.
MigrationResult migrate_majestic_yaml(const std::string& yaml_text);

// A human-readable, line-per-key report (for the installer log / --dry-run).
std::string migration_report(const MigrationResult& r);

// The resulting machino.conf body (Mapped + Converted keys only), with a header
// comment noting the migration.
std::string to_machino_conf(const MigrationResult& r);

}} // namespace machino::compat
