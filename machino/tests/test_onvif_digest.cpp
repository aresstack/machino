// ONVIF HTTP Digest - the second half of what upstream says onvif.password
// unlocks ("WSSE PasswordDigest and HTTP Digest auth so legacy clients
// (ODM v2.2.x) and digest-only ones (tinyCam Monitor) work"). Only the WSSE
// half existed before.
//
// The hashing itself is RtspAuth's, already covered by its own tests; what is
// tested here is the part that is new: the stateless nonce, the challenge, and
// every way the exchange can fail.
#include "app/onvif/onvif_service.hpp"
#include "app/rtsp/rtsp_auth.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::onvif;

extern int g_fail_ext, g_pass_ext;
#define GCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const int64_t NOW = 1609556645;
const char* URI = "/onvif/device_service";

// A fixed nonce secret so the tests are deterministic. In the daemon this
// comes from /dev/urandom - see the review note on why it must NOT be the
// password.
const char* SECRET = "test-nonce-secret-0123456789";

OnvifService make(const std::string& password, const char* secret = SECRET) {
    OnvifConfig c;
    c.enabled = true;
    c.password = password;
    OnvifService s(c, [](const std::string& u, const std::string& p) {
        return u == "root" && p == "shadowpass";
    }, secret ? secret : "");
    s.set_ports(80, 554);
    return s;
}

// Build the header a client would send for this nonce.
std::string digest_header(const std::string& user, const std::string& pass,
                          const std::string& realm, const std::string& nonce,
                          const std::string& method, const std::string& uri,
                          const std::string& nc = "00000001",
                          const std::string& cnonce = "abc123",
                          const std::string& qop = "auth") {
    const std::string resp = RtspAuth::digest_response(user, pass, realm, nonce,
                                                       method, uri, nc, cnonce, qop);
    return "Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
           "\", uri=\"" + uri + "\", response=\"" + resp + "\", qop=" + qop +
           ", nc=" + nc + ", cnonce=\"" + cnonce + "\"";
}

bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

std::string envelope_probe() {
    return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
           "<s:Body><tds:GetDeviceInformation/></s:Body></s:Envelope>";
}

} // namespace

void run_onvif_digest_tests() {
    const std::string REALM = OnvifService::DIGEST_REALM;

    // ---- the stateless nonce --------------------------------------------------
    {
        OnvifService s = make("secret");
        const std::string n = s.make_nonce(NOW);
        GCHECK(n.find('.') != std::string::npos);
        GCHECK(s.nonce_ok(n, NOW));
        // fresh within the window, in both directions
        GCHECK(s.nonce_ok(n, NOW + OnvifService::CLOCK_SKEW_S));
        GCHECK(!s.nonce_ok(n, NOW + OnvifService::CLOCK_SKEW_S + 1));
        GCHECK(!s.nonce_ok(n, NOW - OnvifService::CLOCK_SKEW_S - 1));

        // a client cannot mint one: the tag binds the timestamp to the password
        char ts[32]; snprintf(ts, sizeof ts, "%lld", (long long)NOW);
        GCHECK(!s.nonce_ok(std::string(ts) + ".0123456789abcdef0123456789abcdef", NOW));
        GCHECK(!s.nonce_ok(std::string(ts) + ".", NOW));
        GCHECK(!s.nonce_ok(".abc", NOW));
        GCHECK(!s.nonce_ok("notanumber.abc", NOW));
        GCHECK(!s.nonce_ok("", NOW));
        GCHECK(!s.nonce_ok("99999999999999999999.abc", NOW));   // overlong timestamp

        // The nonce must NOT depend on the password. challenge() hands it to
        // any unauthenticated caller, so keying it with the password published
        // sha1(known-prefix || password) to the whole network - a
        // single-iteration offline oracle. Found in review, after shipping it.
        OnvifService same_secret_other_pw = make("completely-different");
        GCHECK(same_secret_other_pw.make_nonce(NOW) == n);

        // It must depend on the process secret, so two cameras do not share one
        OnvifService other_secret = make("secret", "a-different-process-secret");
        GCHECK(other_secret.make_nonce(NOW) != n);
        GCHECK(!other_secret.nonce_ok(n, NOW));
    }

    // ---- no entropy for the secret: Digest is neither offered nor accepted ----
    {
        // Fail closed. Without a secret the camera cannot have issued a genuine
        // nonce, so it cannot judge a digest built on one.
        OnvifService s = make("secret", nullptr);
        GCHECK(!has(s.challenge(NOW), "Digest"));
        GCHECK(has(s.challenge(NOW), "Basic"));
        GCHECK(s.authenticate("<s:Envelope/>",
                              digest_header("root", "secret", OnvifService::DIGEST_REALM,
                                            "1.abc", "POST", URI), NOW, "POST")
               == AuthResult::Unverifiable);
        // Basic against the configured password still works
        GCHECK(s.authenticate("<s:Envelope/>", "Basic cm9vdDpzZWNyZXQ=", NOW, "POST") == AuthResult::Ok);
    }

    // ---- the challenge ---------------------------------------------------------
    {
        OnvifService s = make("secret");
        const std::string ch = s.challenge(NOW);
        GCHECK(has(ch, "WWW-Authenticate: Digest realm=\"" + REALM + "\""));
        GCHECK(has(ch, "qop=\"auth\""));
        GCHECK(has(ch, "WWW-Authenticate: Basic realm="));
        GCHECK(ch.size() > 4 && ch.compare(ch.size() - 2, 2, "\r\n") == 0);

        // without a cleartext password Digest cannot be served at all, so it
        // is not offered - offering it would invite a client to try something
        // that can never succeed
        OnvifService t = make("");
        const std::string ct = t.challenge(NOW);
        GCHECK(!has(ct, "Digest"));
        GCHECK(has(ct, "Basic"));

        // with authentication switched off there is nothing to challenge for
        OnvifService u = make("secret");
        u.set_unsafe(true);
        GCHECK(u.challenge(NOW).empty());
    }

    // ---- a correct exchange ------------------------------------------------------
    {
        OnvifService s = make("secret");
        const std::string n = s.make_nonce(NOW);
        const std::string h = digest_header("root", "secret", REALM, n, "POST", URI);
        GCHECK(s.authenticate("<s:Envelope/>", h, NOW, "POST") == AuthResult::Ok);
    }

    // ---- every way it can fail ------------------------------------------------
    {
        OnvifService s = make("secret");
        const std::string n = s.make_nonce(NOW);

        // wrong password
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("root", "wrong", REALM, n, "POST", URI),
                              NOW, "POST") == AuthResult::Bad);
        // wrong user
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("admin", "secret", REALM, n, "POST", URI),
                              NOW, "POST") == AuthResult::Bad);
        // wrong realm
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("root", "secret", "Other", n, "POST", URI),
                              NOW, "POST") == AuthResult::Bad);
        // the METHOD is part of the hash, so a digest computed for GET must
        // not authorise a POST
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("root", "secret", REALM, n, "GET", URI),
                              NOW, "POST") == AuthResult::Bad);
        // so is the uri - and because the CLIENT states it, a digest computed
        // for another resource must not authorise this one even though it
        // hashes consistently with its own declaration
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("root", "secret", REALM, n, "POST", "/other"),
                              NOW, "POST", URI) == AuthResult::Bad);
        // an absolute URL ending in the path is accepted: clients send both
        GCHECK(s.authenticate("<s:Envelope/>",
                              digest_header("root", "secret", REALM, n, "POST",
                                            std::string("http://192.168.1.10") + URI),
                              NOW, "POST", URI) == AuthResult::Ok);
        // a forged nonce is Stale, not Bad: the client should retry with the
        // fresh one rather than be told its password is wrong
        GCHECK(s.authenticate("<s:Envelope/>", digest_header("root", "secret", REALM, "1.deadbeef", "POST", URI),
                              NOW, "POST") == AuthResult::Stale);
        // an expired nonce, likewise
        OnvifService t = make("secret");
        const std::string old_nonce = t.make_nonce(NOW - OnvifService::CLOCK_SKEW_S - 10);
        GCHECK(t.authenticate("<s:Envelope/>", digest_header("root", "secret", REALM, old_nonce, "POST", URI),
                              NOW, "POST") == AuthResult::Stale);
        // truncated headers fail on the realm or the username before the nonce
        // is ever looked at, which is Bad rather than Stale - and Bad is right,
        // since retrying with a fresh nonce would not help
        GCHECK(s.authenticate("<s:Envelope/>", "Digest username=\"root\"", NOW, "POST") == AuthResult::Bad);
        GCHECK(s.authenticate("<s:Envelope/>", "Digest ", NOW, "POST") == AuthResult::Bad);
    }

    // ---- replay -----------------------------------------------------------------
    {
        OnvifService s = make("secret");
        const std::string n = s.make_nonce(NOW);
        const std::string h = digest_header("root", "secret", REALM, n, "POST", URI);
        GCHECK(s.authenticate("<s:Envelope/>", h, NOW, "POST") == AuthResult::Ok);
        // the identical request again is a replay
        GCHECK(s.authenticate("<s:Envelope/>", h, NOW, "POST") == AuthResult::Stale);
        // but the same nonce with the next nc is the normal, allowed case
        const std::string h2 = digest_header("root", "secret", REALM, n, "POST", URI, "00000002");
        GCHECK(s.authenticate("<s:Envelope/>", h2, NOW, "POST") == AuthResult::Ok);

        // a WRONG response must not burn the (nonce, nc) pair the real client
        // is about to use - the same rule as the WSSE path
        OnvifService t = make("secret");
        const std::string tn = t.make_nonce(NOW);
        GCHECK(t.authenticate("<s:Envelope/>",
                              digest_header("root", "wrong", REALM, tn, "POST", URI), NOW, "POST")
               == AuthResult::Bad);
        GCHECK(t.authenticate("<s:Envelope/>",
                              digest_header("root", "secret", REALM, tn, "POST", URI), NOW, "POST")
               == AuthResult::Ok);
    }

    // ---- no cleartext password: cannot be judged, and says so -------------------
    {
        OnvifService s = make("");
        // the nonce is meaningless here, but the point is the verdict
        GCHECK(s.authenticate("<s:Envelope/>",
                              digest_header("root", "shadowpass", REALM, "1.x", "POST", URI), NOW, "POST")
               == AuthResult::Unverifiable);
        // ... while Basic against /etc/shadow still works
        GCHECK(s.authenticate("<s:Envelope/>", "Basic cm9vdDpzaGFkb3dwYXNz", NOW, "POST") == AuthResult::Ok);
    }

    // ---- unclaimed and unsafe still outrank Digest -------------------------------
    {
        OnvifService s = make("secret");
        const std::string n = s.make_nonce(NOW);
        const std::string h = digest_header("root", "secret", REALM, n, "POST", URI);
        s.set_claimed(false);
        GCHECK(s.authenticate("<s:Envelope/>", h, NOW, "POST") == AuthResult::Unclaimed);
        s.set_unsafe(true);
        GCHECK(s.authenticate("<s:Envelope/>", h, NOW, "POST") == AuthResult::Ok);
    }

    // ---- end to end through handle(), including the 401 carrying a challenge -----
    {
        OnvifService s = make("secret");
        OnvifService::Request r;
        r.path = URI;
        r.body = envelope_probe();
        r.host = "192.168.1.10";
        r.method = "POST";

        OnvifService::Response resp = s.handle(r, NOW);
        GCHECK(resp.status == 401);
        GCHECK(has(resp.extra_headers, "Digest realm="));
        GCHECK(has(resp.extra_headers, "nonce="));
        GCHECK(has(resp.body, "NotAuthorized"));

        // the client extracts the nonce from that challenge and retries
        const size_t a = resp.extra_headers.find("nonce=\"") + 7;
        const size_t b = resp.extra_headers.find('"', a);
        const std::string nonce = resp.extra_headers.substr(a, b - a);
        GCHECK(s.nonce_ok(nonce, NOW));

        r.authorization = digest_header("root", "secret", REALM, nonce, "POST", URI);
        OnvifService::Response ok = s.handle(r, NOW);
        GCHECK(ok.status == 200);
        GCHECK(has(ok.body, "GetDeviceInformationResponse"));
        GCHECK(ok.extra_headers.empty());        // nothing to challenge on success
    }
}
