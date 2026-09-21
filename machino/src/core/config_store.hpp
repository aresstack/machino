// Machino core: persistent configuration store for the API-managed keys.
// Keeps the user's file intact (comments, other keys, order) and rewrites
// only the managed `key = value` lines; unknown managed keys are appended.
// Saves atomically: temp file -> fsync -> rename. Carries a monotonically
// increasing revision (`config.revision`) for optimistic concurrency.
#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace machino {

using KeyValues = std::vector<std::pair<std::string, std::string>>;

// Pure text transformation (host-testable): replaces or appends `key = value`
// lines (first match wins, later duplicates removed) and sets config.revision.
std::string rewrite_config_text(const std::string& text, const KeyValues& kv, unsigned revision);
// Pure text transformation (host-testable): REMOVES the `key = value` lines of
// the given keys (comments and everything else untouched) and sets
// config.revision. Removing a key returns it to the unconfigured state - the
// majestic-webui reset contract for fields without a schema default.
std::string remove_config_keys_text(const std::string& text, const std::vector<std::string>& keys, unsigned revision);
// Reads a `key = value` from config text ("" when absent).
std::string config_text_get(const std::string& text, const std::string& key);

class ConfigStore {
public:
    explicit ConfigStore(const std::string& path) : path_(path) {}
    bool load(std::string& err);                       // reads the file; revision from config.revision (default 1)
    unsigned revision() const { std::lock_guard<std::mutex> lk(m_); return revision_; }
    std::string get(const std::string& key) const;
    // Applies kv to the text, bumps the revision, writes atomically. Returns false with err.
    bool commit(const KeyValues& kv, std::string& err);
    // Removes the given keys' lines (unset), bumps the revision, writes atomically.
    bool commit_remove(const std::vector<std::string>& keys, std::string& err);
    const std::string& path() const { return path_; }
    std::string text() const { std::lock_guard<std::mutex> lk(m_); return text_; }
private:
    bool write_atomic(const std::string& data, std::string& err);
    mutable std::mutex m_;
    std::string path_;
    std::string text_;
    unsigned    revision_ = 1;
};

} // namespace machino
