#include "core/json.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace machino {

bool Json::is_integer() const {
    return t_ == Type::Number && std::isfinite(n_) && n_ == std::floor(n_) && n_ >= -9.2e18 && n_ <= 9.2e18;
}

Json& Json::set(const std::string& key, const Json& v) {
    for (auto& m : members_) if (m.first == key) { m.second = v; return *this; }
    members_.emplace_back(key, v);
    return *this;
}

const Json* Json::get(const std::string& key) const {
    for (const auto& m : members_) if (m.first == key) return &m.second;
    return nullptr;
}

static void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

void Json::dump_to(std::string& out) const {
    switch (t_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += b_ ? "true" : "false"; break;
        case Type::Number: {
            char b[32];
            if (is_integer()) snprintf(b, sizeof b, "%lld", (long long)n_);
            else snprintf(b, sizeof b, "%.6g", n_);
            out += b; break;
        }
        case Type::String: dump_string(s_, out); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < items_.size(); ++i) { if (i) out += ','; items_[i].dump_to(out); }
            out += ']'; break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < members_.size(); ++i) { if (i) out += ','; dump_string(members_[i].first, out); out += ':'; members_[i].second.dump_to(out); }
            out += '}'; break;
    }
}

std::string Json::dump() const { std::string s; dump_to(s); return s; }

// ---- parser -----------------------------------------------------------------
namespace {
struct Parser {
    const std::string& t; size_t p = 0; std::string& err; const Json::Limits& lim;
    Parser(const std::string& text, std::string& e, const Json::Limits& l) : t(text), err(e), lim(l) {}
    bool fail(const char* why) { if (err.empty()) err = std::string(why) + " at " + std::to_string(p); return false; }
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p; }
    bool lit(const char* s) { size_t n = strlen(s); if (t.compare(p, n, s) == 0) { p += n; return true; } return false; }
    bool str(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("expected string");
        ++p; out.clear();
        while (true) {
            if (p >= t.size()) return fail("unterminated string");
            unsigned char c = t[p++];
            if (c == '"') break;
            if (c == '\\') {
                if (p >= t.size()) return fail("bad escape");
                char e = t[p++];
                switch (e) {
                    case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break; case 't': out += '\t'; break;
                    case 'u': {
                        if (p + 4 > t.size()) return fail("bad \\u escape");
                        unsigned v = 0; for (int i = 0; i < 4; ++i) { char h = t[p++]; v <<= 4;
                            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0'); else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10); else return fail("bad \\u escape"); }
                        if (v < 0x80) out += (char)v;
                        else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 0x3F)); }
                        else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 0x3F)); out += (char)(0x80 | (v & 0x3F)); }
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else if (c < 0x20) return fail("control character in string");
            else out += (char)c;
            if (out.size() > lim.max_string) return fail("string too long");
        }
        return true;
    }
    bool value(Json& out, size_t depth) {
        if (depth > lim.max_depth) return fail("nesting too deep");
        ws();
        if (p >= t.size()) return fail("unexpected end");
        char c = t[p];
        if (c == '{') {
            ++p; out = Json::object(); ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            while (true) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (p >= t.size() || t[p] != ':') return fail("expected ':'"); ++p;
                Json v; if (!value(v, depth + 1)) return false;
                if (out.has(k)) return fail("duplicate key");
                out.set(k, v);
                ws(); if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p; out = Json::array(); ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            while (true) {
                Json v; if (!value(v, depth + 1)) return false;
                out.push(v);
                ws(); if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { std::string s; if (!str(s)) return false; out = Json::string(s); return true; }
        if (lit("true"))  { out = Json::boolean(true);  return true; }
        if (lit("false")) { out = Json::boolean(false); return true; }
        if (lit("null"))  { out = Json::null();         return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = p; if (t[p] == '-') ++p;
            if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("bad number");
            while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p;
            if (p < t.size() && t[p] == '.') { ++p; if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("bad number"); while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p; }
            if (p < t.size() && (t[p] == 'e' || t[p] == 'E')) { ++p; if (p < t.size() && (t[p] == '+' || t[p] == '-')) ++p; if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("bad number"); while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p; }
            if (p - start > 40) return fail("number too long");
            double d = strtod(t.substr(start, p - start).c_str(), nullptr);
            if (!std::isfinite(d)) return fail("number out of range");
            out = Json::number(d); return true;
        }
        return fail("unexpected character");
    }
};
} // namespace

bool Json::parse(const std::string& text, Json& out, std::string& err, const Limits& lim) {
    err.clear();
    if (text.size() > lim.max_bytes) { err = "document too large"; return false; }
    Parser ps(text, err, lim);
    Json v;
    if (!ps.value(v, 1)) return false;
    ps.ws();
    if (ps.p != text.size()) { err = "trailing characters at " + std::to_string(ps.p); return false; }
    out = v;
    return true;
}

const Json* json_path(const Json& root, const std::string& path) {
    const Json* cur = &root; size_t p = 0;
    while (p <= path.size()) {
        size_t d = path.find('.', p);
        std::string key = path.substr(p, d == std::string::npos ? std::string::npos : d - p);
        if (!cur->is_object()) return nullptr;
        cur = cur->get(key);
        if (!cur) return nullptr;
        if (d == std::string::npos) break;
        p = d + 1;
    }
    return cur;
}

} // namespace machino
