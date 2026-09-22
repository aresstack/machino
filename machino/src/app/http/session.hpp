// Application: Majestic drop-in session auth for the front door. The contract
// is taken from the STOCK OpenIPC majestic-webui (login.html + main.js):
//   - POST /login   application/x-www-form-urlencoded "username=..&password=.."
//                   (+ "remember=1") -> 200 + Set-Cookie on success, 403 on
//                   wrong credentials ("Invalid username or password.")
//   - POST /logout  -> 200, session invalidated, cookie cleared
//   - every OTHER request without a valid session cookie -> 401 WITHOUT a
//     WWW-Authenticate header (the browser must never pop its native Basic
//     dialog; main.js redirects to /login.html on 401 itself), or 302 to
//     /login.html for a top-level HTML navigation.
//   - public without a session: GET /login.html (self-contained page),
//     POST /login, GET /favicon.ico.
// Pure logic, host-testable: the credential check is injected.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace machino { namespace http {

class SessionGate {
public:
    using CheckFn = std::function<bool(const std::string& user, const std::string& pass)>;

    explicit SessionGate(CheckFn check) : check_(std::move(check)) {}

    // Paths that must work WITHOUT a session, or nobody could ever log in.
    static bool is_public(const std::string& method, const std::string& path);

    // Camera-local callers are trusted without credentials, exactly like
    // Majestic ("majestic waves through requests that originate on the camera
    // itself" - upstream www/cgi-bin/p/majestic.sh). peer is "ip:port".
    static bool is_local_peer(const std::string& peer);

    // Valid session cookie present?
    bool authed(const std::string& cookie_header, int64_t now_ms);

    // CLI/API fallback: "Authorization: Basic base64(user:pass)" validated
    // against the same credential check (per request, no session created).
    bool authed_basic(const std::string& authorization_header);

    struct LoginResult {
        int         status;      // 200 or 403 (400 on malformed body)
        std::string set_cookie;  // full "Set-Cookie: ...\r\n" header line, empty unless 200
    };
    LoginResult login(const std::string& form_body, int64_t now_ms);
    // Mint a session WITHOUT checking credentials. The one caller is the
    // setup flow, which has just set the password and then re-authenticated
    // against it - so the check has already happened, with the plaintext that
    // must not be handed around a second time to repeat it. Returns the full
    // Set-Cookie header line, terminated like the others.
    std::string mint(int64_t now_ms);
    void        logout(const std::string& cookie_header);

    // "Set-Cookie: <name>=; Max-Age=0; Path=/\r\n" - clears the browser cookie.
    static std::string clear_cookie();

    // helpers (exposed for tests)
    static std::string form_value(const std::string& body, const std::string& key);   // url-decoded
    static std::string cookie_value(const std::string& header, const std::string& name);

    size_t sessions() const { return tokens_.size(); }

private:
    // Majestic's cookie: named "session", SameSite=Strict (upstream ptz.cgi
    // relies on Strict). Do not invent a different contract.
    static constexpr const char* COOKIE = "session";
    static constexpr int64_t SESSION_MS  = 12ll * 3600 * 1000;       // no "remember"
    static constexpr int64_t REMEMBER_MS = 30ll * 24 * 3600 * 1000;  // "stay signed in"
    static constexpr size_t  MAX_SESSIONS = 32;                      // oldest evicted

    std::string new_token();
    void        evict(int64_t now_ms);

    CheckFn check_;
    std::unordered_map<std::string, int64_t> tokens_;  // token -> expiry (mono ms)
};

}} // namespace machino::http
