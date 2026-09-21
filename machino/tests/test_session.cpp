// Majestic drop-in session auth (SessionGate): the webui contract - form
// login, cookie session, logout, 403 on wrong credentials, public paths.
#include "app/http/session.hpp"
#include <cstdio>
#include <string>

using namespace machino::http;
extern int g_fail_ext, g_pass_ext;
#define SCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_session_tests() {
    // form decoding: '+', %XX, missing keys
    SCHECK(SessionGate::form_value("username=root&password=a%40b+c", "username") == "root");
    SCHECK(SessionGate::form_value("username=root&password=a%40b+c", "password") == "a@b c");
    SCHECK(SessionGate::form_value("username=root", "password").empty());
    SCHECK(SessionGate::form_value("password=x&remember=1", "remember") == "1");

    // cookie extraction from a multi-cookie header
    SCHECK(SessionGate::cookie_value("a=1; machino_session=tok123; b=2", "machino_session") == "tok123");
    SCHECK(SessionGate::cookie_value("machino_session=solo", "machino_session") == "solo");
    SCHECK(SessionGate::cookie_value("other=1", "machino_session").empty());

    // public paths: exactly what the self-contained login page needs
    SCHECK(SessionGate::is_public("GET", "/login.html"));
    SCHECK(SessionGate::is_public("POST", "/login"));
    SCHECK(SessionGate::is_public("GET", "/favicon.ico"));
    SCHECK(!SessionGate::is_public("GET", "/"));
    SCHECK(!SessionGate::is_public("GET", "/metrics"));
    SCHECK(!SessionGate::is_public("GET", "/api/v1/state"));
    SCHECK(!SessionGate::is_public("GET", "/cgi-bin/dashboard.cgi"));
    SCHECK(!SessionGate::is_public("GET", "/login"));   // only POST is the login action

    SessionGate g([](const std::string& u, const std::string& p) { return u == "root" && p == "geheim"; });

    // wrong credentials -> 403 (the webui shows "Invalid username or password.")
    SCHECK(g.login("username=root&password=falsch", 1000).status == 403);
    SCHECK(g.login("password=nix", 1000).status == 400);          // no user at all
    SCHECK(g.sessions() == 0);

    // good login -> 200 + session cookie; the token then authenticates
    SessionGate::LoginResult ok = g.login("username=root&password=geheim", 1000);
    SCHECK(ok.status == 200 && ok.set_cookie.rfind("Set-Cookie: machino_session=", 0) == 0);
    SCHECK(ok.set_cookie.find("HttpOnly") != std::string::npos);
    SCHECK(ok.set_cookie.find("Max-Age") == std::string::npos);   // session cookie without remember
    std::string tok = ok.set_cookie.substr(ok.set_cookie.find('=') + 1);
    tok = tok.substr(0, tok.find(';'));
    SCHECK(tok.size() == 32);
    SCHECK(g.authed("machino_session=" + tok, 2000));
    SCHECK(!g.authed("machino_session=falsch", 2000));
    SCHECK(!g.authed("", 2000));

    // expiry: 12h without remember
    SCHECK(!g.authed("machino_session=" + tok, 1000 + 13ll * 3600 * 1000));

    // remember=1 -> persistent cookie + 30d server-side expiry
    SessionGate::LoginResult rem = g.login("username=root&password=geheim&remember=1", 1000);
    SCHECK(rem.status == 200 && rem.set_cookie.find("Max-Age=2592000") != std::string::npos);
    std::string tok2 = rem.set_cookie.substr(rem.set_cookie.find('=') + 1);
    tok2 = tok2.substr(0, tok2.find(';'));
    SCHECK(g.authed("machino_session=" + tok2, 1000 + 13ll * 3600 * 1000));

    // logout kills exactly that session; the clear header resets the browser
    g.logout("machino_session=" + tok2);
    SCHECK(!g.authed("machino_session=" + tok2, 2000));
    SCHECK(SessionGate::clear_cookie().find("Max-Age=0") != std::string::npos);

    // session cap: the oldest session is evicted, the newest still works
    SessionGate cap([](const std::string&, const std::string&) { return true; });
    std::string first;
    for (int i = 0; i < 40; ++i) {
        SessionGate::LoginResult r = cap.login("username=u&password=p", 1000 + i);
        if (i == 0) { first = r.set_cookie.substr(r.set_cookie.find('=') + 1); first = first.substr(0, first.find(';')); }
    }
    SCHECK(cap.sessions() <= 32);
    SCHECK(!cap.authed("machino_session=" + first, 2000));
}
