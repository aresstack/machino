#include "app/rtsp/rtsp_auth.hpp"

#include <mbedtls/md5.h>

#include <cstdio>
#include <cstring>
#include <random>


namespace machino {

namespace {

std::string md5_hex(const std::string& in) {
    unsigned char h[16];
    mbedtls_md5((const unsigned char*)in.data(), in.size(), h);
    char out[33];
    for (int i = 0; i < 16; ++i) snprintf(out + i * 2, 3, "%02x", h[i]);
    return std::string(out, 32);
}

std::string new_nonce() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33];
    snprintf(buf, sizeof buf, "%016llx%016llx",
             (unsigned long long)rng(), (unsigned long long)rng());
    return std::string(buf, 32);
}

// RFC 4648 base64 decode, tolerant of missing padding.
bool b64_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xff); }
    }
    return true;
}

bool starts_ci(const std::string& s, const char* p) {
    const size_t n = strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)p[i])) return false;
    return true;
}

// Constant-time-ish compare so a wrong digest cannot be probed byte by byte.
bool same(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

} // namespace

std::string RtspAuth::auth_param(const std::string& header, const char* name) {
    const std::string key = std::string(name) + "=";
    size_t at = 0;
    while ((at = header.find(key, at)) != std::string::npos) {
        // must be at a token boundary, not the tail of another parameter
        const bool boundary = at == 0 || header[at - 1] == ',' || header[at - 1] == ' ';
        at += key.size();
        if (!boundary) continue;
        if (at < header.size() && header[at] == '"') {
            const size_t e = header.find('"', at + 1);
            if (e == std::string::npos) return "";
            return header.substr(at + 1, e - at - 1);
        }
        size_t e = header.find_first_of(", \t\r\n", at);
        if (e == std::string::npos) e = header.size();
        return header.substr(at, e - at);
    }
    return "";
}

std::string RtspAuth::ha1_hex(const std::string& user, const std::string& realm,
                              const std::string& password) {
    return md5_hex(user + ":" + realm + ":" + password);
}

std::string RtspAuth::digest_response(const std::string& user, const std::string& pass,
                                      const std::string& realm, const std::string& nonce,
                                      const std::string& method, const std::string& uri,
                                      const std::string& nc, const std::string& cnonce,
                                      const std::string& qop) {
    const std::string ha1 = ha1_hex(user, realm, pass);
    const std::string ha2 = md5_hex(method + ":" + uri);
    if (qop.empty()) return md5_hex(ha1 + ":" + nonce + ":" + ha2);
    return md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
}

std::string RtspAuth::challenge(Ctx& ctx, int64_t now_ms, bool stale) const {
    ctx.nonce = new_nonce();
    ctx.issued_ms = now_ms;
    std::string out;
    if (digest_usable()) {
        out += "Digest realm=\"" + cfg_.realm + "\", nonce=\"" + ctx.nonce + "\", qop=\"auth\"";
        if (stale) out += ", stale=true";
    }
    if (cfg_.offer_basic) {
        if (!out.empty()) out += "\n";
        out += "Basic realm=\"" + cfg_.realm + "\"";
    }
    return out;
}

RtspAuth::Verdict RtspAuth::check(Ctx& ctx, const std::string& method, const std::string& uri,
                                  const std::string& authz, int64_t now_ms) const {
    if (!required()) return Verdict::Ok;
    // An UNCLAIMED camera refuses every credential outright rather than
    // letting one through the schemes below. In practice /etc/shadow would
    // reject them anyway - an empty hash matches nothing - but relying on that
    // would make a security property an accident of another function's
    // behaviour, and the point here is that there is nothing to be right about
    // until the camera has been set up.
    if (!claimed()) return Verdict::Bad;
    if (authz.empty()) return Verdict::Missing;
    if (!check_) return Verdict::Bad;

    if (cfg_.offer_basic && starts_ci(authz, "Basic ")) {
        std::string dec;
        if (!b64_decode(authz.substr(6), dec)) return Verdict::Bad;
        const size_t c = dec.find(':');
        if (c == std::string::npos) return Verdict::Bad;
        // The password substring is never copied into any longer-lived object
        // and never logged.
        const bool ok = check_(dec.substr(0, c), dec.substr(c + 1));
        if (ok) ctx.authed = true;
        return ok ? Verdict::Ok : Verdict::Bad;
    }

    if (digest_usable() && starts_ci(authz, "Digest ")) {
        const std::string user  = auth_param(authz, "username");
        const std::string nonce = auth_param(authz, "nonce");
        const std::string resp  = auth_param(authz, "response");
        const std::string uri_p = auth_param(authz, "uri");
        const std::string qop   = auth_param(authz, "qop");
        const std::string nc    = auth_param(authz, "nc");
        const std::string cn    = auth_param(authz, "cnonce");
        if (user.empty() || nonce.empty() || resp.empty()) return Verdict::Bad;
        if (ctx.nonce.empty() || nonce != ctx.nonce) return Verdict::Stale;
        if (cfg_.nonce_lifetime_s > 0 && now_ms - ctx.issued_ms > (int64_t)cfg_.nonce_lifetime_s * 1000)
            return Verdict::Stale;
        const std::string ha1 = ha1_(user, cfg_.realm);
        if (ha1.empty()) return Verdict::Bad;            // no stored secret for this user
        const std::string ha2  = md5_hex(method + ":" + (uri_p.empty() ? uri : uri_p));
        const std::string want = qop.empty()
            ? md5_hex(ha1 + ":" + nonce + ":" + ha2)
            : md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cn + ":" + qop + ":" + ha2);
        if (!same(want, resp)) return Verdict::Bad;
        ctx.authed = true;
        return Verdict::Ok;
    }

    return Verdict::Bad;
}

} // namespace machino
