// AP10 follow-up: an UNCLAIMED camera streams nothing.
//
// Upstream is explicit - "it streams nothing, answers RTSP and ONVIF with
// 401" - but until now only the HTTP surface honoured that. RTSP stayed
// reachable on a camera nobody had set up, because rtsp.auth defaults to off
// and nothing else consulted the claim state. These tests pin the fix.
#include "app/rtsp/rtsp_auth.hpp"
#include <cstdio>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define CCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {
bool good_cred(const std::string& u, const std::string& p) { return u == "root" && p == "pw"; }
const char* BASIC = "Basic cm9vdDpwdw==";          // root:pw
} // namespace

void run_rtsp_claim_tests() {
    // rtsp.auth off + claimed: unchanged, no challenge at all. This is the
    // configuration every camera runs today, so it must be byte-identical.
    {
        RtspAuthConfig c; c.enabled = false;                 // AP7: explicit; the default is now ON
        RtspAuth a(c, good_cred, nullptr, [] { return true; }, false);
        CCHECK(!a.required());
        CCHECK(a.claimed());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Ok);
    }

    // rtsp.auth off + UNCLAIMED: authentication becomes mandatory anyway.
    // This is the gap - before the fix this answered Ok and the stream was
    // reachable on a camera that had never been set up.
    {
        RtspAuthConfig c;
        RtspAuth a(c, good_cred, nullptr, [] { return false; }, false);
        CCHECK(!a.claimed());
        CCHECK(a.required());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Bad);
        // ... and a credential that WOULD be right on a claimed camera is
        // still refused: there is nothing to be right about yet
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", BASIC, 1000) == RtspAuth::Verdict::Bad);
        CCHECK(!ctx.authed);
    }

    // rtsp.auth on + UNCLAIMED: refused for the same reason
    {
        RtspAuthConfig c; c.enabled = true;
        RtspAuth a(c, good_cred, nullptr, [] { return false; }, false);
        CCHECK(a.required());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", BASIC, 1000) == RtspAuth::Verdict::Bad);
        CCHECK(!ctx.authed);
    }

    // rtsp.auth on + claimed: the ordinary path is untouched
    {
        RtspAuthConfig c; c.enabled = true;
        RtspAuth a(c, good_cred, nullptr, [] { return true; }, false);
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", BASIC, 1000) == RtspAuth::Verdict::Ok);
        RtspAuth::Ctx ctx2;
        CCHECK(a.check(ctx2, "DESCRIBE", "rtsp://cam/ch0", "Basic cm9vdDp4", 1000) == RtspAuth::Verdict::Bad);
        RtspAuth::Ctx ctx3;
        CCHECK(a.check(ctx3, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Missing);
    }

    // system.unsafe outranks everything, unclaimed included - upstream calls
    // that the supported way to run a deliberately-open camera, and says so in
    // exactly those terms.
    {
        RtspAuthConfig c; c.enabled = true;
        RtspAuth a(c, good_cred, nullptr, [] { return false; }, /*unsafe=*/true);
        CCHECK(a.unsafe());
        CCHECK(!a.required());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Ok);
    }

    // no claim predicate at all = assume claimed, so every existing caller and
    // every existing test keeps the behaviour it had
    {
        RtspAuthConfig c; c.enabled = true;
        RtspAuth a(c, good_cred);
        CCHECK(a.claimed());
        CCHECK(!a.unsafe());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", BASIC, 1000) == RtspAuth::Verdict::Ok);
    }

    // the predicate is consulted per request, not cached: a camera claimed
    // while the daemon runs starts serving without a restart, which is what
    // makes the SSH door and the web door agree immediately
    {
        bool claimed = false;
        RtspAuthConfig c; c.enabled = false;                 // AP7: explicit; the default is now ON
        RtspAuth a(c, good_cred, nullptr, [&claimed] { return claimed; }, false);
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", BASIC, 1000) == RtspAuth::Verdict::Bad);
        claimed = true;
        CCHECK(!a.required());
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Ok);
    }
}

// AP7: the DEFAULT. The stock WebUI has no rtsp.auth key at all - stream-urls.cgi
// carries two mutually exclusive notes and main.js picks between them on
// system.unsafe alone:
//
//   const note = $(unsafe === true || unsafe === 'true' ? '#ep-unsafe' : '#ep-auth');
//
// So "authenticates as root unless unsafe" IS the contract, and a default of
// off meant the page promised authentication the camera did not perform.
void run_rtsp_auth_default_tests() {
    // out of the box: required, on both units
    {
        RtspAuthConfig c;                                   // untouched defaults
        CCHECK(c.enabled);
        RtspAuth a(c, good_cred, nullptr, [] { return true; }, false);
        CCHECK(a.required());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) != RtspAuth::Verdict::Ok);
        RtspAuth::Ctx ctx2;
        CCHECK(a.check(ctx2, "DESCRIBE", "rtsp://cam/ch1", "", 1000) != RtspAuth::Verdict::Ok);
    }
    // system.unsafe outranks it, on both units
    {
        RtspAuthConfig c;
        RtspAuth a(c, good_cred, nullptr, [] { return true; }, true);   // unsafe
        CCHECK(!a.required());
        RtspAuth::Ctx ctx;
        CCHECK(a.check(ctx, "DESCRIBE", "rtsp://cam/ch0", "", 1000) == RtspAuth::Verdict::Ok);
        RtspAuth::Ctx ctx2;
        CCHECK(a.check(ctx2, "PLAY", "rtsp://cam/ch1", "", 1000) == RtspAuth::Verdict::Ok);
    }
    // an explicit opt-out still works - it is a setting, not a removal
    {
        RtspAuthConfig c; c.enabled = false;
        RtspAuth a(c, good_cred, nullptr, [] { return true; }, false);
        CCHECK(!a.required());
    }
    // and an UNCLAIMED camera challenges whatever any of that says: there is
    // no credential yet, so nothing could be right
    {
        RtspAuthConfig c; c.enabled = false;
        RtspAuth a(c, good_cred, nullptr, [] { return false; }, false);
        CCHECK(a.required());
    }
}
