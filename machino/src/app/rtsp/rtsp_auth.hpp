// RTSP authentication, kept out of the parser and socket code so it can be
// reasoned about (and host-tested) on its own.
//
// PROVEN contract (stock webui www/cgi-bin/stream-urls.cgi, the oracle):
//   "These endpoints authenticate as user root with the same password you use
//    for this WebUI" and, when system.unsafe is set, "Authentication is
//    switched off for every endpoint".
// So Majestic has NO separate RTSP account: the credential is the system
// account, exactly like the WebUI session login, and the kill switch is the
// global system.unsafe - not an rtsp-specific one. This class therefore takes
// the SAME CheckFn the HTTP session gate uses (crypt(3) against /etc/shadow)
// and never stores a password of its own.
//
// DERIVED (strongly implied, confirm with the capture): Digest needs
// HA1 = MD5(user:realm:password), i.e. the plaintext password or a stored
// HA1. crypt(3) against /etc/shadow is one-way and can only answer "is this
// password right?". So a camera whose RTSP credential IS the system account
// cannot serve Digest from /etc/shadow - Basic is the only scheme that works
// with this credential source. Digest is therefore off by default, and the
// code path exists but requires an explicit HA1 provider (a stored secret),
// never a password oracle.
//
// BLOCKED_T31_CAPTURE: the exact wire behaviour (does Majestic offer Basic
// only, or Digest too from some other secret; the realm string; whether it
// sends stale=true) still needs a real Majestic handshake. Nothing about it
// is invented here.
#pragma once
#include "core/config.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace machino {

class RtspAuth {
public:
    // (username, password) -> accepted. The same shadow_check the WebUI uses.
    using CheckFn = std::function<bool(const std::string&, const std::string&)>;
    // (username, realm) -> HA1 hex, or "" when this user has no stored secret.
    // Only set when a Digest-capable credential store exists; absent means
    // Digest cannot be served (see the note above).
    using Ha1Fn = std::function<std::string(const std::string&, const std::string&)>;

    RtspAuth(const RtspAuthConfig& cfg, CheckFn check, Ha1Fn ha1 = nullptr)
        : cfg_(cfg), check_(std::move(check)), ha1_(std::move(ha1)) {}

    bool required() const { return cfg_.enabled && (digest_usable() || cfg_.offer_basic); }
    bool digest_usable() const { return cfg_.offer_digest && ha1_ != nullptr; }

    // Per-CONNECTION state. A nonce belongs to one connection and dies with
    // it; nothing here holds a credential.
    struct Ctx {
        std::string nonce;
        int64_t     issued_ms = 0;
        bool        authed = false;
    };

    enum class Verdict { Ok, Missing, Bad, Stale };

    // `authz` is the raw Authorization header value ("" when absent).
    Verdict check(Ctx& ctx, const std::string& method, const std::string& uri,
                  const std::string& authz, int64_t now_ms) const;

    // WWW-Authenticate value(s) for a 401, newline-separated when both schemes
    // are offered (the caller emits one header line each). Mints/refreshes the
    // connection's nonce.
    std::string challenge(Ctx& ctx, int64_t now_ms, bool stale = false) const;

    // HA1 = MD5(user:realm:password). A Digest-capable credential store must
    // produce this; /etc/shadow cannot, which is why Basic is the default.
    static std::string ha1_hex(const std::string& user, const std::string& realm,
                               const std::string& password);

    // exposed for tests: RFC 7616 MD5 digest response for these inputs
    static std::string digest_response(const std::string& user, const std::string& pass,
                                       const std::string& realm, const std::string& nonce,
                                       const std::string& method, const std::string& uri,
                                       const std::string& nc, const std::string& cnonce,
                                       const std::string& qop);
    // exposed for tests: value of one quoted/unquoted param out of an auth header
    static std::string auth_param(const std::string& header, const char* name);

private:
    RtspAuthConfig cfg_;
    CheckFn        check_;
    Ha1Fn          ha1_;
};

} // namespace machino
