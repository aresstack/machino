// RTSP auth (AP3): the component that decides whether a connection may reach
// the media path at all. Wire-level and host-testable; the point is that
// auth-off is byte-identical to before, auth-on refuses anything short of the
// right credential, and the decision is per connection.
#include "app/rtsp/rtsp_auth.hpp"
#include <cstdio>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define RCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned)(unsigned char)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (unsigned)(unsigned char)in[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += i + 1 < in.size() ? T[(v >> 6) & 63] : '=';
        o += i + 2 < in.size() ? T[v & 63] : '=';
    }
    return o;
}

// The camera credential source answers yes/no only (crypt against
// /etc/shadow); this fake has the same shape and counts how often it is asked.
struct Creds {
    int calls = 0;
    bool operator()(const std::string& u, const std::string& p) {
        ++calls;
        return u == "root" && p == "s3cret";
    }
};

RtspAuthConfig on() { RtspAuthConfig c; c.enabled = true; return c; }

} // namespace

void run_rtsp_auth_tests() {
    const int64_t T0 = 1000000;
    const std::string OK_BASIC = "Basic " + b64("root:s3cret");

    // disabled: every request passes untouched and the credential source is
    // never consulted (previous behaviour, unchanged)
    {
        Creds cr; RtspAuthConfig cfg; cfg.enabled = false;   // AP7: no longer the default
        RtspAuth a(cfg, std::ref(cr)); RtspAuth::Ctx ctx;
        RCHECK(!a.required());
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "", T0) == RtspAuth::Verdict::Ok);
        RCHECK(a.check(ctx, "PLAY", "/ch0", "garbage", T0) == RtspAuth::Verdict::Ok);
        RCHECK(cr.calls == 0);
    }

    // enabled, nothing presented: Missing, and the challenge offers Basic only
    {
        Creds cr; RtspAuth a(on(), std::ref(cr)); RtspAuth::Ctx ctx;
        RCHECK(a.required());
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "", T0) == RtspAuth::Verdict::Missing);
        const std::string ch = a.challenge(ctx, T0);
        RCHECK(ch.find("Basic realm=") != std::string::npos);
        RCHECK(ch.find("Digest") == std::string::npos);   // no HA1 provider -> not offered
        RCHECK(!ctx.nonce.empty());
    }

    // wrong user / wrong password / malformed / unknown scheme, then the right one
    {
        Creds cr; RtspAuth a(on(), std::ref(cr)); RtspAuth::Ctx ctx;
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "Basic " + b64("admin:s3cret"), T0) == RtspAuth::Verdict::Bad);
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "Basic " + b64("root:wrong"), T0) == RtspAuth::Verdict::Bad);
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "Basic " + b64("noseparator"), T0) == RtspAuth::Verdict::Bad);
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "Basic !!!not-base64!!!", T0) == RtspAuth::Verdict::Bad);
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", "Bearer whatever", T0) == RtspAuth::Verdict::Bad);
        RCHECK(!ctx.authed);
        RCHECK(a.check(ctx, "DESCRIBE", "/ch0", OK_BASIC, T0) == RtspAuth::Verdict::Ok);
        RCHECK(ctx.authed);
        RtspAuth::Ctx c2;
        RCHECK(a.check(c2, "DESCRIBE", "/ch0", "Basic " + b64("root:"), T0) == RtspAuth::Verdict::Bad);
    }

    // the same credential works for every method and BOTH mounts: the stream
    // id must never change the rule
    {
        Creds cr; RtspAuth a(on(), std::ref(cr)); RtspAuth::Ctx ctx;
        const char* methods[] = {"DESCRIBE", "SETUP", "PLAY"};
        const char* urls[] = {"/ch0", "/ch1", "/stream=0", "/stream=1"};
        for (const char* m : methods)
            for (const char* u : urls)
                RCHECK(a.check(ctx, m, u, OK_BASIC, T0) == RtspAuth::Verdict::Ok);
    }

    // state is PER CONNECTION: one authorised context never authorises another
    {
        Creds cr; RtspAuth a(on(), std::ref(cr));
        RtspAuth::Ctx c1, c2;
        RCHECK(a.check(c1, "PLAY", "/ch0", OK_BASIC, T0) == RtspAuth::Verdict::Ok);
        RCHECK(c1.authed && !c2.authed);
        RCHECK(a.check(c2, "PLAY", "/ch0", "", T0) == RtspAuth::Verdict::Missing);
        a.challenge(c1, T0); a.challenge(c2, T0);
        RCHECK(!c1.nonce.empty() && c1.nonce != c2.nonce);   // nonces are not shared
    }

    // no credential source wired: refuse rather than fail open
    {
        RtspAuth a(on(), nullptr); RtspAuth::Ctx ctx;
        RCHECK(a.required());
        RCHECK(a.check(ctx, "PLAY", "/ch0", OK_BASIC, T0) == RtspAuth::Verdict::Bad);
    }

    // Digest is only served when a stored secret exists. With an HA1 provider
    // a correct RFC 7616 response is accepted; a wrong one, an unknown user
    // and an expired nonce are not.
    {
        Creds cr;
        RtspAuthConfig cfg = on(); cfg.offer_digest = true; cfg.nonce_lifetime_s = 10;
        auto ha1 = [](const std::string& u, const std::string& realm) -> std::string {
            return u == "root" ? RtspAuth::ha1_hex(u, realm, "s3cret") : std::string();
        };
        RtspAuth a(cfg, std::ref(cr), ha1);
        RCHECK(a.digest_usable());
        RtspAuth::Ctx ctx;
        const std::string ch = a.challenge(ctx, T0);
        RCHECK(ch.find("Digest realm=") != std::string::npos);
        RCHECK(ch.find("nonce=") != std::string::npos);
        RCHECK(ch.find("Basic realm=") != std::string::npos);   // both offered here

        auto hdr = [&](const std::string& user, const std::string& pass, const std::string& nonce) {
            const std::string resp = RtspAuth::digest_response(user, pass, cfg.realm, nonce,
                                                               "PLAY", "/ch0", "00000001", "cn", "auth");
            return "Digest username=\"" + user + "\", realm=\"" + cfg.realm + "\", nonce=\"" + nonce +
                   "\", uri=\"/ch0\", qop=auth, nc=00000001, cnonce=\"cn\", response=\"" + resp + "\"";
        };
        RCHECK(a.check(ctx, "PLAY", "/ch0", hdr("root", "s3cret", ctx.nonce), T0) == RtspAuth::Verdict::Ok);
        RCHECK(ctx.authed);
        RtspAuth::Ctx c2; a.challenge(c2, T0);
        RCHECK(a.check(c2, "PLAY", "/ch0", hdr("root", "wrong", c2.nonce), T0) == RtspAuth::Verdict::Bad);
        RtspAuth::Ctx c3; a.challenge(c3, T0);
        RCHECK(a.check(c3, "PLAY", "/ch0", hdr("nobody", "s3cret", c3.nonce), T0) == RtspAuth::Verdict::Bad);
        // a nonce we never issued, and one that has aged out, are both Stale
        RtspAuth::Ctx c4; a.challenge(c4, T0);
        RCHECK(a.check(c4, "PLAY", "/ch0", hdr("root", "s3cret", "someoneelsesnonce"), T0) == RtspAuth::Verdict::Stale);
        RCHECK(a.check(c4, "PLAY", "/ch0", hdr("root", "s3cret", c4.nonce), T0 + 11000) == RtspAuth::Verdict::Stale);
        RCHECK(a.challenge(c4, T0 + 11000, true).find("stale=true") != std::string::npos);
    }

    // without a provider the Digest branch refuses instead of falling through
    {
        Creds cr; RtspAuthConfig cfg = on(); cfg.offer_digest = true;
        RtspAuth a(cfg, std::ref(cr)); RtspAuth::Ctx ctx;
        RCHECK(!a.digest_usable());
        RCHECK(a.challenge(ctx, T0).find("Digest") == std::string::npos);
        RCHECK(a.check(ctx, "PLAY", "/ch0", "Digest username=\"root\", nonce=\"x\", response=\"y\"", T0)
               == RtspAuth::Verdict::Bad);
    }

    // header parameter parsing: quoted, unquoted, and a lookalike name
    {
        const std::string h = "Digest username=\"root\", realm=\"Machino\", nc=00000001, "
                              "cnonce=\"abc\", qop=auth, response=\"deadbeef\", myusername=\"no\"";
        RCHECK(RtspAuth::auth_param(h, "username") == "root");
        RCHECK(RtspAuth::auth_param(h, "realm") == "Machino");
        RCHECK(RtspAuth::auth_param(h, "nc") == "00000001");
        RCHECK(RtspAuth::auth_param(h, "qop") == "auth");
        RCHECK(RtspAuth::auth_param(h, "response") == "deadbeef");
        RCHECK(RtspAuth::auth_param(h, "absent").empty());
    }

    // the digest formula is deterministic, password-sensitive and qop-aware
    {
        const std::string r1 = RtspAuth::digest_response("root", "s3cret", "Machino", "abc", "DESCRIBE", "/ch0", "", "", "");
        const std::string r2 = RtspAuth::digest_response("root", "s3cret", "Machino", "abc", "DESCRIBE", "/ch0", "", "", "");
        const std::string r3 = RtspAuth::digest_response("root", "other",  "Machino", "abc", "DESCRIBE", "/ch0", "", "", "");
        RCHECK(r1.size() == 32 && r1 == r2 && r1 != r3);
        const std::string q = RtspAuth::digest_response("root", "s3cret", "Machino", "abc", "DESCRIBE", "/ch0", "00000001", "cn", "auth");
        RCHECK(q.size() == 32 && q != r1);
        RCHECK(RtspAuth::ha1_hex("root", "Machino", "s3cret").size() == 32);
    }
}
