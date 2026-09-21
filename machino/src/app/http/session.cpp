#include "app/http/session.hpp"
#include <cstdio>
#include <random>

namespace machino { namespace http {

bool SessionGate::is_public(const std::string& method, const std::string& path) {
    if (method == "GET" && (path == "/login.html" || path == "/favicon.ico")) return true;
    if (method == "POST" && path == "/login") return true;
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string SessionGate::form_value(const std::string& body, const std::string& key) {
    // application/x-www-form-urlencoded: k=v&k=v, '+' is space, %XX is a byte.
    size_t p = 0;
    while (p < body.size()) {
        size_t amp = body.find('&', p);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', p);
        if (eq != std::string::npos && eq < amp && body.compare(p, eq - p, key) == 0) {
            std::string out;
            for (size_t i = eq + 1; i < amp; ++i) {
                char c = body[i];
                if (c == '+') out += ' ';
                else if (c == '%' && i + 2 < amp) {
                    int h = hexval(body[i + 1]), l = hexval(body[i + 2]);
                    if (h >= 0 && l >= 0) { out += (char)(h * 16 + l); i += 2; }
                    else out += c;
                } else out += c;
            }
            return out;
        }
        p = amp + 1;
    }
    return "";
}

std::string SessionGate::cookie_value(const std::string& header, const std::string& name) {
    // "a=1; machino_session=abc; b=2"
    size_t p = 0;
    while (p < header.size()) {
        while (p < header.size() && (header[p] == ' ' || header[p] == ';')) ++p;
        size_t eq = header.find('=', p);
        size_t end = header.find(';', p);
        if (end == std::string::npos) end = header.size();
        if (eq != std::string::npos && eq < end && header.compare(p, eq - p, name) == 0)
            return header.substr(eq + 1, end - eq - 1);
        p = end + 1;
    }
    return "";
}

bool SessionGate::authed(const std::string& cookie_header, int64_t now_ms) {
    const std::string tok = cookie_value(cookie_header, COOKIE);
    if (tok.empty()) return false;
    auto it = tokens_.find(tok);
    if (it == tokens_.end()) return false;
    if (now_ms > it->second) { tokens_.erase(it); return false; }
    return true;
}

std::string SessionGate::new_token() {
    // Not host-seeded state: fresh entropy per token. 128 bits as hex.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx",
                  (unsigned long long)rng(), (unsigned long long)rng());
    return std::string(buf, 32);
}

void SessionGate::evict(int64_t now_ms) {
    for (auto it = tokens_.begin(); it != tokens_.end();)
        it = (now_ms > it->second) ? tokens_.erase(it) : ++it;
    while (tokens_.size() >= MAX_SESSIONS) {   // still full: drop the earliest expiry
        auto oldest = tokens_.begin();
        for (auto it = tokens_.begin(); it != tokens_.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        tokens_.erase(oldest);
    }
}

SessionGate::LoginResult SessionGate::login(const std::string& body, int64_t now_ms) {
    const std::string user = form_value(body, "username");
    const std::string pass = form_value(body, "password");
    const bool remember = form_value(body, "remember") == "1";
    if (user.empty()) return {400, ""};
    if (!check_ || !check_(user, pass)) return {403, ""};   // webui shows "Invalid username or password."
    evict(now_ms);
    const std::string tok = new_token();
    tokens_[tok] = now_ms + (remember ? REMEMBER_MS : SESSION_MS);
    std::string sc = std::string("Set-Cookie: ") + COOKIE + "=" + tok + "; Path=/; HttpOnly; SameSite=Lax";
    if (remember) sc += "; Max-Age=2592000";
    sc += "\r\n";
    return {200, sc};
}

void SessionGate::logout(const std::string& cookie_header) {
    const std::string tok = cookie_value(cookie_header, COOKIE);
    if (!tok.empty()) tokens_.erase(tok);
}

std::string SessionGate::clear_cookie() {
    return std::string("Set-Cookie: ") + COOKIE + "=; Path=/; Max-Age=0\r\n";
}

}} // namespace machino::http
