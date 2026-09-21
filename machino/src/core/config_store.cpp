#include "core/config_store.hpp"
#include "core/log.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace machino {

static const char* MOD = "CONFIG";

static void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// key of a config line ("" for comments/blank/invalid)
static std::string line_key(const std::string& line) {
    std::string s = line; size_t h = s.find('#'); if (h != std::string::npos) s.erase(h);
    size_t eq = s.find('='); if (eq == std::string::npos) return "";
    std::string k = s.substr(0, eq); trim(k); return k;
}

std::string config_text_get(const std::string& text, const std::string& key) {
    size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        std::string line = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line_key(line) != key) continue;
        std::string s = line; size_t h = s.find('#'); if (h != std::string::npos) s.erase(h);
        std::string v = s.substr(s.find('=') + 1); trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

std::string rewrite_config_text(const std::string& text, const KeyValues& kv, unsigned revision) {
    KeyValues all = kv;
    all.emplace_back("config.revision", std::to_string(revision));
    std::vector<bool> done(all.size(), false);
    std::string out; size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        std::string line = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = (nl == std::string::npos) ? text.size() : nl + 1;
        std::string k = line_key(line);
        bool replaced = false, drop = false;
        if (!k.empty()) for (size_t i = 0; i < all.size(); ++i) if (all[i].first == k) {
            if (done[i]) drop = true;                       // later duplicate removed
            else { out += k + " = " + all[i].second + "\n"; done[i] = true; replaced = true; }
            break;
        }
        if (drop || replaced) continue;
        out += line; out += '\n';
    }
    bool first = true;
    for (size_t i = 0; i < all.size(); ++i) if (!done[i]) {
        if (first) { if (!out.empty() && out.back() != '\n') out += '\n'; out += "\n# --- managed by the Machino API ---\n"; first = false; }
        out += all[i].first + " = " + all[i].second + "\n";
    }
    return out;
}

std::string remove_config_keys_text(const std::string& text, const std::vector<std::string>& keys, unsigned revision) {
    // Drop the listed keys' lines, then let the rewrite set the new revision
    // (and nothing else): an unset key must vanish, not become an empty value.
    std::string pruned; size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        std::string line = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = (nl == std::string::npos) ? text.size() : nl + 1;
        const std::string k = line_key(line);
        bool drop = false;
        if (!k.empty()) for (const std::string& r : keys) if (r == k) { drop = true; break; }
        if (drop) continue;
        pruned += line; pruned += '\n';
    }
    return rewrite_config_text(pruned, KeyValues{}, revision);
}

bool ConfigStore::load(std::string& err) {
    FILE* f = fopen(path_.c_str(), "r");
    if (!f) { err = "cannot open " + path_; return false; }
    std::string text; char buf[512];
    while (fgets(buf, sizeof buf, f)) text += buf;
    fclose(f);
    std::lock_guard<std::mutex> lk(m_);
    text_ = text;
    std::string r = config_text_get(text_, "config.revision");
    revision_ = r.empty() ? 1u : (unsigned)strtoul(r.c_str(), nullptr, 10);
    if (revision_ == 0) revision_ = 1;
    return true;
}

std::string ConfigStore::get(const std::string& key) const { std::lock_guard<std::mutex> lk(m_); return config_text_get(text_, key); }

bool ConfigStore::write_atomic(const std::string& data, std::string& err) {
    std::string tmp = path_ + ".tmp";
#ifndef _WIN32
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { err = "cannot create " + tmp; return false; }
    size_t off = 0;
    while (off < data.size()) { ssize_t w = write(fd, data.data() + off, data.size() - off); if (w <= 0) { close(fd); unlink(tmp.c_str()); err = "write failed"; return false; } off += (size_t)w; }
    if (fsync(fd) != 0) { close(fd); unlink(tmp.c_str()); err = "fsync failed"; return false; }
    close(fd);
    if (rename(tmp.c_str(), path_.c_str()) != 0) { unlink(tmp.c_str()); err = "rename failed"; return false; }
    // fsync the directory so the rename itself is durable
    std::string dir = path_; size_t sl = dir.rfind('/'); dir = (sl == std::string::npos) ? "." : (sl == 0 ? "/" : dir.substr(0, sl));
    int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
#else
    FILE* f = fopen(tmp.c_str(), "wb"); if (!f) { err = "cannot create " + tmp; return false; }
    fwrite(data.data(), 1, data.size(), f); fclose(f);
    remove(path_.c_str());
    if (rename(tmp.c_str(), path_.c_str()) != 0) { err = "rename failed"; return false; }
    return true;
#endif
}

bool ConfigStore::commit(const KeyValues& kv, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    unsigned next = revision_ + 1;
    std::string data = rewrite_config_text(text_, kv, next);
    if (!write_atomic(data, err)) { LOGE(MOD, "persist failed: %s", err.c_str()); return false; }
    text_ = data; revision_ = next;
    LOGI(MOD, "persisted %lu setting(s) to %s (revision %u)", (unsigned long)kv.size(), path_.c_str(), revision_);
    return true;
}

bool ConfigStore::commit_remove(const std::vector<std::string>& keys, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    unsigned next = revision_ + 1;
    std::string data = remove_config_keys_text(text_, keys, next);
    if (!write_atomic(data, err)) { LOGE(MOD, "unset persist failed: %s", err.c_str()); return false; }
    text_ = data; revision_ = next;
    LOGI(MOD, "removed %lu setting(s) from %s (revision %u)", (unsigned long)keys.size(), path_.c_str(), revision_);
    return true;
}

} // namespace machino
