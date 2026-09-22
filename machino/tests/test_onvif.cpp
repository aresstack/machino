// AP11: the ONVIF minimal stack. The weight here is on the two things that
// can actually hurt - the unauthenticated XML scanner and the WS-Security
// check - because both run before any credential has been accepted.
#include "app/onvif/onvif_service.hpp"
#include "app/onvif/soap.hpp"
#include "core/config.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::onvif;

extern int g_fail_ext, g_pass_ext;
#define LCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const int64_t NOW = 1609556645;   // 2021-01-02T03:04:05Z

std::string env_body(const std::string& body, const std::string& header = "") {
    return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">" +
           (header.empty() ? "" : "<s:Header>" + header + "</s:Header>") +
           "<s:Body>" + body + "</s:Body></s:Envelope>";
}

std::string wsse(const std::string& user, const std::string& pass,
                 const std::string& nonce_b64, const std::string& created, bool digest) {
    const char* type = digest
        ? "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest"
        : "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText";
    return "<wsse:Security><wsse:UsernameToken>"
           "<wsse:Username>" + user + "</wsse:Username>"
           "<wsse:Password Type=\"" + type + "\">" + pass + "</wsse:Password>"
           "<wsse:Nonce>" + nonce_b64 + "</wsse:Nonce>"
           "<wsu:Created>" + created + "</wsu:Created>"
           "</wsse:UsernameToken></wsse:Security>";
}

std::vector<MediaProfile> two_profiles() {
    MediaProfile a; a.token = "main"; a.name = "Main"; a.width = 1920; a.height = 1080;
    a.fps = 25; a.bitrate_kbps = 3000; a.rtsp_path = "/ch0";
    MediaProfile b; b.token = "sub"; b.name = "Sub"; b.width = 640; b.height = 360;
    b.fps = 15; b.bitrate_kbps = 512; b.rtsp_path = "/ch1";
    return {a, b};
}

OnvifService make(OnvifConfig cfg, bool claimed = true) {
    OnvifService s(cfg, [](const std::string& u, const std::string& p) {
        return u == "root" && p == "shadowpass";      // the /etc/shadow stand-in
    });
    s.set_profiles(two_profiles());
    s.set_ports(80, 554);
    s.set_claimed(claimed);
    DeviceInfo d; d.firmware = "test"; d.serial = "SN1"; d.hardware_id = "T40NN";
    s.set_device(d);
    return s;
}

OnvifConfig on_cfg(const std::string& password = "") {
    OnvifConfig c;
    c.enabled = true;
    c.password = password;
    return c;
}

OnvifService::Request rq(const std::string& body, const std::string& authz = "") {
    OnvifService::Request r;
    r.path = "/onvif/device_service";
    r.body = body;
    r.authorization = authz;
    r.host = "192.168.1.10";
    return r;
}

bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

} // namespace

void run_onvif_tests() {
    // ---- the scanner refuses what it cannot safely handle ------------------
    {
        LCHECK(!soap_acceptable(""));
        LCHECK(!soap_acceptable(std::string(MAX_REQUEST + 1, 'x')));
        // entity expansion is the whole reason this is a scanner and not a
        // parser, so it must be refused before anything else looks at it
        LCHECK(!soap_acceptable("<!DOCTYPE foo [<!ENTITY a \"b\">]><s:Envelope/>"));
        LCHECK(!soap_acceptable("<!doctype x><a/>"));
        LCHECK(!soap_acceptable("<x><!ENTITY lol \"lolol\"></x>"));
        LCHECK(soap_acceptable("<s:Envelope><s:Body><tds:GetScopes/></s:Body></s:Envelope>"));
    }

    // ---- action extraction --------------------------------------------------
    {
        std::string a;
        LCHECK(soap_action(env_body("<tds:GetDeviceInformation/>"), a) && a == "GetDeviceInformation");
        LCHECK(soap_action(env_body("<GetProfiles xmlns=\"x\"/>"), a) && a == "GetProfiles");
        LCHECK(soap_action(env_body("<ns99:GetStreamUri><ns99:ProfileToken>t</ns99:ProfileToken></ns99:GetStreamUri>"), a)
               && a == "GetStreamUri");
        // an empty or absent body yields nothing rather than a guess
        LCHECK(!soap_action(env_body(""), a));
        LCHECK(!soap_action("<s:Envelope></s:Envelope>", a));
        LCHECK(!soap_action("not xml at all", a));
        // a comment before the operation must not become the operation
        LCHECK(soap_action(env_body("<!-- hi --><tds:GetScopes/>"), a) && a == "GetScopes");
        // markup hidden inside a comment must NOT become the operation: the
        // scanner and a real parser have to agree about what was asked for
        LCHECK(soap_action(env_body("<!-- <tds:Evil/> --><tds:GetScopes/>"), a) && a == "GetScopes");
        LCHECK(!soap_action(env_body("<!-- unterminated <tds:GetScopes/>"), a));
    }

    // ---- element text and entity decoding ----------------------------------
    {
        std::string v;
        LCHECK(element_text("<a:Username>root</a:Username>", "Username", v) && v == "root");
        LCHECK(element_text("<Username/>", "Username", v) && v.empty());
        LCHECK(!element_text("<Other>x</Other>", "Username", v));
        LCHECK(element_text("<T>a&amp;b&lt;c&gt;d&quot;e&apos;f</T>", "T", v) && v == "a&b<c>d\"e'f");
        // an unknown entity is left alone, never expanded
        LCHECK(element_text("<T>&xxe;</T>", "T", v) && v == "&xxe;");
        std::string t;
        LCHECK(element_attr("<Password Type=\"...PasswordDigest\">x</Password>", "Password", "Type", t)
               && has(t, "PasswordDigest"));
    }

    // ---- base64 refuses garbage rather than accepting it -------------------
    {
        std::string out;
        LCHECK(b64_decode("YWJj", out) && out == "abc");
        LCHECK(b64_decode("YQ==", out) && out == "a");
        LCHECK(!b64_decode("YWJj!", out));
        LCHECK(!b64_decode("YQ==YQ", out));       // data after padding
        LCHECK(b64_decode("", out) && out.empty());
    }

    // ---- xml escaping ------------------------------------------------------
    {
        LCHECK(xml_escape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&apos;");
        // a control character would make the whole document unparseable for
        // the client, so it is dropped rather than embedded
        LCHECK(xml_escape(std::string("a\x01" "b")) == "ab");
    }

    // ---- the digest itself --------------------------------------------------
    {
        // Base64(SHA1(nonce || created || password)), checked against a value
        // computed independently of the code under test.
        const std::string d = OnvifService::password_digest("\x01\x02\x03", "2021-01-02T03:04:05Z", "secret");
        LCHECK(!d.empty() && d.size() == 28);      // 20 bytes -> 28 base64 chars
        LCHECK(d != OnvifService::password_digest("\x01\x02\x03", "2021-01-02T03:04:05Z", "secre7"));
        LCHECK(d != OnvifService::password_digest("\x01\x02\x04", "2021-01-02T03:04:05Z", "secret"));
        LCHECK(d != OnvifService::password_digest("\x01\x02\x03", "2021-01-02T03:04:06Z", "secret"));
    }

    // ---- the UTC parser -----------------------------------------------------
    {
        int64_t t = 0;
        LCHECK(OnvifService::parse_utc_datetime("2021-01-02T03:04:05Z", t) && t == NOW);
        LCHECK(OnvifService::utc_datetime(NOW) == "2021-01-02T03:04:05Z");
        LCHECK(OnvifService::parse_utc_datetime("1970-01-01T00:00:00Z", t) && t == 0);
        LCHECK(OnvifService::parse_utc_datetime("2024-02-29T12:00:00Z", t) && t == 1709208000);
        // anything that is not plainly UTC is refused, never guessed: a
        // misparsed Created would silently widen the replay window
        LCHECK(!OnvifService::parse_utc_datetime("2021-01-02T03:04:05+01:00", t));
        LCHECK(!OnvifService::parse_utc_datetime("2021-01-02 03:04:05Z", t));
        LCHECK(!OnvifService::parse_utc_datetime("", t));
        LCHECK(!OnvifService::parse_utc_datetime("2021-13-02T03:04:05Z", t));
        LCHECK(!OnvifService::parse_utc_datetime("tomorrow", t));
    }

    // ---- GetSystemDateAndTime is unauthenticated, by spec and by need ------
    {
        OnvifService s = make(on_cfg("secret"));
        OnvifService::Response r = s.handle(rq(env_body("<tds:GetSystemDateAndTime/>")), NOW);
        LCHECK(r.status == 200);
        LCHECK(has(r.body, "GetSystemDateAndTimeResponse"));
        LCHECK(has(r.body, "<tt:Year>2021</tt:Year>"));
        LCHECK(has(r.body, "<tt:Hour>3</tt:Hour>"));
        // ... and nothing else is
        LCHECK(s.handle(rq(env_body("<tds:GetDeviceInformation/>")), NOW).status == 401);
    }

    // ---- WSSE PasswordText --------------------------------------------------
    {
        // against the configured cleartext
        OnvifService s = make(on_cfg("secret"));
        LCHECK(s.authenticate(env_body("<x/>", wsse("root", "secret", "", "", false)), "", NOW) == AuthResult::Ok);
        LCHECK(s.authenticate(env_body("<x/>", wsse("root", "wrong", "", "", false)), "", NOW) == AuthResult::Bad);
        LCHECK(s.authenticate(env_body("<x/>", wsse("admin", "secret", "", "", false)), "", NOW) == AuthResult::Bad);

        // with no cleartext configured it falls back to the shadow lookup
        OnvifService t = make(on_cfg());
        LCHECK(t.authenticate(env_body("<x/>", wsse("root", "shadowpass", "", "", false)), "", NOW) == AuthResult::Ok);
        LCHECK(t.authenticate(env_body("<x/>", wsse("root", "secret", "", "", false)), "", NOW) == AuthResult::Bad);
    }

    // ---- WSSE PasswordDigest -------------------------------------------------
    {
        const std::string nonce_raw = "0123456789abcdef";
        const std::string nonce_b64 = "MDEyMzQ1Njc4OWFiY2RlZg==";
        const std::string created = "2021-01-02T03:04:05Z";
        const std::string good = OnvifService::password_digest(nonce_raw, created, "secret");

        {
            OnvifService s = make(on_cfg("secret"));
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)), "", NOW)
                   == AuthResult::Ok);
            // the very same message a second time is a replay
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)), "", NOW)
                   == AuthResult::Stale);
        }
        {
            OnvifService s = make(on_cfg("secret"));
            // a wrong digest must NOT burn the nonce: otherwise anyone could
            // lock out the real client by guessing first
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", "AAAA", nonce_b64, created, true)), "", NOW)
                   == AuthResult::Bad);
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)), "", NOW)
                   == AuthResult::Ok);
        }
        {
            OnvifService s = make(on_cfg("secret"));
            // outside the freshness window, in both directions
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)),
                                  "", NOW + OnvifService::CLOCK_SKEW_S + 1) == AuthResult::Stale);
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)),
                                  "", NOW - OnvifService::CLOCK_SKEW_S - 1) == AuthResult::Stale);
            // just inside it is fine
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)),
                                  "", NOW + OnvifService::CLOCK_SKEW_S) == AuthResult::Ok);
        }
        {
            // no cleartext configured: a digest cannot be judged at all, and
            // saying so beats calling a possibly-correct credential wrong
            OnvifService s = make(on_cfg());
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, created, true)), "", NOW)
                   == AuthResult::Unverifiable);
        }
        {
            OnvifService s = make(on_cfg("secret"));
            // a digest with no nonce or no timestamp is not acceptable
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, "", created, true)), "", NOW) == AuthResult::Bad);
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, nonce_b64, "", true)), "", NOW) == AuthResult::Bad);
            // a nonce that is not base64 at all
            LCHECK(s.authenticate(env_body("<x/>", wsse("root", good, "!!!!", created, true)), "", NOW) == AuthResult::Bad);
        }
    }

    // ---- HTTP Basic, and no credential at all -------------------------------
    {
        OnvifService s = make(on_cfg("secret"));
        LCHECK(s.authenticate("<s:Envelope/>", "Basic cm9vdDpzZWNyZXQ=", NOW) == AuthResult::Ok);   // root:secret
        LCHECK(s.authenticate("<s:Envelope/>", "Basic cm9vdDp3cm9uZw==", NOW) == AuthResult::Bad);  // root:wrong
        LCHECK(s.authenticate("<s:Envelope/>", "Basic !!!!", NOW) == AuthResult::Bad);
        LCHECK(s.authenticate("<s:Envelope/>", "", NOW) == AuthResult::Missing);
        LCHECK(s.authenticate("<s:Envelope/>", "Bearer abc", NOW) == AuthResult::Missing);
    }

    // ---- claim state and system.unsafe --------------------------------------
    {
        // an unclaimed camera authorises nothing, however good the credential
        OnvifService s = make(on_cfg("secret"), /*claimed=*/false);
        LCHECK(s.authenticate(env_body("<x/>", wsse("root", "secret", "", "", false)), "", NOW)
               == AuthResult::Unclaimed);
        OnvifService::Response r = s.handle(rq(env_body("<tds:GetDeviceInformation/>")), NOW);
        LCHECK(r.status == 401 && has(r.body, "NotAuthorized"));
        // but the clock stays readable, so a client can still see what it is
        LCHECK(s.handle(rq(env_body("<tds:GetSystemDateAndTime/>")), NOW).status == 200);

        // unsafe overrides everything, unclaimed included
        s.set_unsafe(true);
        LCHECK(s.authenticate("<s:Envelope/>", "", NOW) == AuthResult::Ok);
        LCHECK(s.handle(rq(env_body("<tds:GetDeviceInformation/>")), NOW).status == 200);
    }

    // ---- disabled answers 404, never a half-ONVIF ----------------------------
    {
        OnvifConfig c; c.enabled = false;
        OnvifService s = make(c);
        OnvifService::Response r = s.handle(rq(env_body("<tds:GetSystemDateAndTime/>")), NOW);
        LCHECK(r.status == 404);
        LCHECK(!has(r.body, "Envelope"));
    }

    // ---- the operations ------------------------------------------------------
    {
        OnvifService s = make(on_cfg("secret"));
        const std::string auth = "Basic cm9vdDpzZWNyZXQ=";

        auto call = [&](const char* op) {
            return s.handle(rq(env_body(std::string("<tds:") + op + "/>"), auth), NOW);
        };

        OnvifService::Response r = call("GetDeviceInformation");
        LCHECK(r.status == 200);
        LCHECK(has(r.body, "<tds:Manufacturer>OpenIPC</tds:Manufacturer>"));
        LCHECK(has(r.body, "<tds:HardwareId>T40NN</tds:HardwareId>"));

        r = call("GetCapabilities");
        LCHECK(r.status == 200);
        LCHECK(has(r.body, "http://192.168.1.10:80/onvif/device_service"));
        LCHECK(has(r.body, "http://192.168.1.10:80/onvif/media_service"));
        // only what is implemented may be advertised
        LCHECK(!has(r.body, "<tt:PTZ>"));
        LCHECK(!has(r.body, "<tt:Events>"));

        r = call("GetServices");
        LCHECK(has(r.body, "onvif.org/ver10/device/wsdl") && has(r.body, "onvif.org/ver10/media/wsdl"));

        r = call("GetScopes");
        LCHECK(has(r.body, "onvif://www.onvif.org/Profile/Streaming"));

        r = call("GetProfiles");
        LCHECK(r.status == 200);
        LCHECK(has(r.body, "token=\"main\"") && has(r.body, "token=\"sub\""));
        LCHECK(has(r.body, "<tt:Width>1920</tt:Width>") && has(r.body, "<tt:Width>640</tt:Width>"));
        LCHECK(has(r.body, "<tt:Encoding>H264</tt:Encoding>"));

        r = call("GetVideoSources");
        LCHECK(has(r.body, "<tt:Framerate>25</tt:Framerate>"));

        r = call("GetVideoEncoderConfigurations");
        LCHECK(has(r.body, "vec-main") && has(r.body, "vec-sub"));
        LCHECK(has(r.body, "<tt:BitrateLimit>512</tt:BitrateLimit>"));

        // the stream URI, per profile
        r = s.handle(rq(env_body("<trt:GetStreamUri><trt:ProfileToken>sub</trt:ProfileToken></trt:GetStreamUri>"), auth), NOW);
        LCHECK(r.status == 200);
        LCHECK(has(r.body, "rtsp://192.168.1.10:554/ch1"));
        r = s.handle(rq(env_body("<trt:GetStreamUri><trt:ProfileToken>main</trt:ProfileToken></trt:GetStreamUri>"), auth), NOW);
        LCHECK(has(r.body, "rtsp://192.168.1.10:554/ch0"));
        // no token: the first profile, which is what clients that omit it expect
        r = s.handle(rq(env_body("<trt:GetStreamUri/>"), auth), NOW);
        LCHECK(r.status == 200 && has(r.body, "/ch0"));
        // a token we do not have is an error, never a guess at another stream
        r = s.handle(rq(env_body("<trt:GetStreamUri><trt:ProfileToken>nope</trt:ProfileToken></trt:GetStreamUri>"), auth), NOW);
        LCHECK(r.status == 400 && has(r.body, "ter:NoProfile"));

        // anything unimplemented says so rather than answering wrongly
        r = s.handle(rq(env_body("<tptz:ContinuousMove/>"), auth), NOW);
        LCHECK(r.status == 400 && has(r.body, "ter:ActionNotSupported") && has(r.body, "ContinuousMove"));
        r = s.handle(rq(env_body("<trt:GetSnapshotUri/>"), auth), NOW);
        LCHECK(r.status == 400 && has(r.body, "ter:ActionNotSupported"));

        // malformed input is a fault, not a crash and not a 200
        r = s.handle(rq("<!DOCTYPE x><s:Envelope/>", auth), NOW);
        LCHECK(r.status == 400 && has(r.body, "ter:WellFormed"));
        r = s.handle(rq("", auth), NOW);
        LCHECK(r.status == 400);
    }

    // ---- regressions found in review ------------------------------------------
    {
        // A comment must not be able to truncate a credential. The close-tag
        // scan runs on unauthenticated input too, and it did not skip comments.
        std::string v;
        LCHECK(element_text("<P>ab<!-- </P> -->cd</P>", "P", v) && v == "ab<!-- </P> -->cd");
        LCHECK(!element_text("<P>ab<!-- unterminated", "P", v));
        // the ordinary cases still work
        LCHECK(element_text("<P>plain</P>", "P", v) && v == "plain");
        LCHECK(element_text("<a:P>ns</a:P>", "P", v) && v == "ns");

        // A constant-time compare still has to be a CORRECT compare.
        LCHECK(secure_equals("", ""));
        LCHECK(secure_equals("abc", "abc"));
        LCHECK(!secure_equals("abc", "abd"));
        LCHECK(!secure_equals("abc", "ab"));
        LCHECK(!secure_equals("ab", "abc"));
        LCHECK(!secure_equals("", "a"));
        // embedded NULs are compared, not treated as terminators
        const std::string z1(std::string("a") + '\0' + "b");
        const std::string z2(std::string("a") + '\0' + "c");
        LCHECK(z1.size() == 3 && !secure_equals(z1, z2));
        LCHECK(secure_equals(z1, z1));
    }

    // ---- routing and config -------------------------------------------------
    {
        LCHECK(OnvifService::is_onvif_path("/onvif/device_service"));
        LCHECK(OnvifService::is_onvif_path("/onvif/media_service"));
        LCHECK(!OnvifService::is_onvif_path("/onvifx"));
        LCHECK(!OnvifService::is_onvif_path("/api/v1/onvif"));

        AppConfig cfg;
        std::string err;
        LCHECK(!cfg.onvif.enabled);                       // off until hardware-accepted
        LCHECK(cfg.onvif.username == "root");
        LCHECK(cfg.onvif.password.empty());
        LCHECK(parse_config_text("onvif.enabled = true\nonvif.username = cam\nonvif.password = pw\n", cfg, err));
        LCHECK(cfg.onvif.enabled && cfg.onvif.username == "cam" && cfg.onvif.password == "pw");
    }
}
