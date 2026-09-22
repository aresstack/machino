// AP10: the unclaimed / first-run flow. Contract read out of the upstream
// clone (www/setup.html, tests/setup-page.test.js, CLAUDE.md), so these tests
// pin what the STOCK page needs, not what would be convenient.
#include "app/http/session.hpp"
#include "app/http/setup.hpp"
#include "core/config.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::http;

extern int g_fail_ext, g_pass_ext;
#define LCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A stand-in for the camera: one string is the whole persisted state, which is
// exactly how the real thing works (root's shadow hash and nothing else).
struct FakeCamera {
    std::string stored;          // "" = unclaimed
    bool   claim_fails = false;
    bool   write_silently_lost = false;   // chpasswd says ok, nothing lands
    bool   eula_on_image = false;
    bool   can_install_key = false;
    bool   key_install_fails = false;
    int    claims = 0;
    std::string installed_key;

    ClaimState state() const { return stored.empty() ? ClaimState::Unclaimed : ClaimState::Claimed; }

    bool claim(const std::string& pw, std::string& err) {
        ++claims;
        if (claim_fails) { err = "The system refused the password."; return false; }
        if (!write_silently_lost) stored = pw;
        return true;                       // exit status 0 either way
    }
    bool verify(const std::string& user, const std::string& pass) const {
        return user == "root" && !stored.empty() && stored == pass;
    }
    bool install_key(const std::string& k, std::string& err) {
        if (key_install_fails) { err = "the store is read-only"; return false; }
        installed_key = k;
        return true;
    }
};

SetupGate make_gate(FakeCamera& cam) {
    SetupGate::KeyFn key = nullptr;
    if (cam.can_install_key)
        key = [&cam](const std::string& k, std::string& e) { return cam.install_key(k, e); };
    return SetupGate([&cam] { return cam.state(); },
                     [&cam](const std::string& p, std::string& e) { return cam.claim(p, e); },
                     [&cam](const std::string& u, const std::string& p) { return cam.verify(u, p); },
                     [&cam] { return cam.eula_on_image; },
                     key);
}

const char* GOOD_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyMaterialHere0123 me@host";

std::string form(const std::string& pw, const std::string& confirm) {
    return "password=" + pw + "&confirm=" + confirm;
}

} // namespace

void run_setup_tests() {
    // ---- a fresh camera is UNCLAIMED and only answers the claim flow -------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);
        LCHECK(g.unclaimed());
        LCHECK(g.state() == ClaimState::Unclaimed);

        // the only paths an unclaimed camera serves
        LCHECK(SetupGate::is_setup_path("POST", "/setup"));
        LCHECK(SetupGate::is_setup_path("GET", "/setup.html"));
        LCHECK(SetupGate::is_setup_path("GET", "/favicon.ico"));
        LCHECK(SetupGate::is_setup_path("GET", "/eula.en.txt"));
        LCHECK(SetupGate::is_setup_path("GET", "/eula.zh-CN.txt"));
        // and nothing else - notably not the ordinary admin surface
        LCHECK(!SetupGate::is_setup_path("GET", "/api/v1/config"));
        LCHECK(!SetupGate::is_setup_path("POST", "/api/v1/config"));
        LCHECK(!SetupGate::is_setup_path("GET", "/login.html"));
        LCHECK(!SetupGate::is_setup_path("POST", "/login"));
        LCHECK(!SetupGate::is_setup_path("GET", "/ws/video"));
        LCHECK(!SetupGate::is_setup_path("GET", "/api/v1/osd"));
        LCHECK(!SetupGate::is_setup_path("POST", "/setup.html"));
        LCHECK(!SetupGate::is_setup_path("GET", "/setup"));
        // an eula path must not be a way out of the web root, nor a wildcard
        LCHECK(!SetupGate::is_setup_path("GET", "/eula.../../etc/shadow.txt"));
        LCHECK(!SetupGate::is_setup_path("GET", "/eula.en.txt.sh"));
        LCHECK(!SetupGate::is_setup_path("GET", "/eulax.en.txt"));
    }

    // ---- setup with valid data claims the camera and mints a session ------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);
        SetupOutcome o = g.post(form("correcthorse", "correcthorse"));
        LCHECK(o.status == 200);
        LCHECK(o.body.empty());          // an empty body is what sends the page to live.cgi
        LCHECK(o.mint_session);
        LCHECK(cam.stored == "correcthorse");
        LCHECK(g.state() == ClaimState::Claimed);
        LCHECK(!g.unclaimed());
    }

    // ---- a second setup after the claim is refused, and does no work ------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);
        LCHECK(g.post(form("correcthorse", "correcthorse")).status == 200);
        const int after_first = cam.claims;
        SetupOutcome o = g.post(form("somethingelse", "somethingelse"));
        LCHECK(o.status == 403);
        LCHECK(!o.mint_session);
        LCHECK(cam.claims == after_first);      // the password helper was never run again
        LCHECK(cam.stored == "correcthorse");   // and the original password stands
    }

    // ---- bad input, all refused with a message and no state change --------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);

        struct Case { const char* body; int status; };
        const Case cases[] = {
            {"",                                            400},   // nothing at all
            {"password=&confirm=",                          400},   // empty
            {"password=short1&confirm=short1",              400},   // under 8
            {"password=1234567&confirm=1234567",            400},   // one under 8
            {"password=correcthorse&confirm=",              400},   // no confirmation
            {"password=correcthorse&confirm=correcthors",   400},   // mismatch
            {"confirm=correcthorse",                        400},   // no password
        };
        for (const Case& c : cases) {
            SetupOutcome o = g.post(c.body);
            LCHECK(o.status == c.status);
            LCHECK(!o.body.empty());        // the page prints what the camera said
            LCHECK(!o.mint_session);
        }
        LCHECK(cam.claims == 0);
        LCHECK(g.unclaimed());              // still correctable, which is the point

        // exactly 8 is accepted: the boundary is the page's own minlength
        LCHECK(g.post(form("12345678", "12345678")).status == 200);
    }

    // ---- oversized input ---------------------------------------------------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);
        const std::string huge(SetupGate::MAX_PASSWORD + 1, 'a');
        SetupOutcome o = g.post(form(huge, huge));
        LCHECK(o.status == 400);
        LCHECK(cam.claims == 0);
        // the largest allowed password still works
        const std::string big(SetupGate::MAX_PASSWORD, 'a');
        LCHECK(g.post(form(big, big)).status == 200);

        FakeCamera cam2;
        SetupGate g2 = make_gate(cam2);
        const std::string body(SetupGate::MAX_BODY + 1, 'x');
        LCHECK(g2.post(body).status == 413);
        LCHECK(cam2.claims == 0);
    }

    // ---- a password that would forge a second shadow record ----------------
    {
        // "root:<password>" goes to chpasswd on ONE line. A colon or a newline
        // there is not a weak password, it is a second record.
        const char* bad[] = {"pass:word1", "pass%0Aword1", "pass%0Dword1",
                             "root:x%0Aroot2:y"};
        for (const char* b : bad) {
            FakeCamera cam;
            SetupGate g = make_gate(cam);
            const std::string body = std::string("password=") + b + "&confirm=" + b;
            SetupOutcome o = g.post(body);
            LCHECK(o.status == 400);
            LCHECK(cam.claims == 0);
            LCHECK(g.unclaimed());
        }
    }

    // ---- the EULA gate -----------------------------------------------------
    {
        FakeCamera cam;
        cam.eula_on_image = true;
        SetupGate g = make_gate(cam);

        // enforced whenever the document is on the image, and checked here
        // even though the page already checked
        LCHECK(g.post(form("correcthorse", "correcthorse")).status == 400);
        LCHECK(cam.claims == 0);
        LCHECK(g.post(form("correcthorse", "correcthorse") + "&eula=").status == 400);
        LCHECK(g.post(form("correcthorse", "correcthorse") + "&eula=no").status == 400);
        LCHECK(g.post(form("correcthorse", "correcthorse") + "&eula=accepted&eula_lang=en").status == 200);
        LCHECK(cam.stored == "correcthorse");

        // no document on the image: not demanded, and an absent parameter is
        // not an error
        FakeCamera plain;
        SetupGate g2 = make_gate(plain);
        LCHECK(g2.post(form("correcthorse", "correcthorse")).status == 200);
    }

    // ---- the write that claims to have worked and did not -------------------
    {
        // An exit status is not proof the hash was written, which is why
        // upstream re-authenticates. A lost write must NOT report success and
        // must NOT mint a session.
        FakeCamera cam;
        cam.write_silently_lost = true;
        SetupGate g = make_gate(cam);
        SetupOutcome o = g.post(form("correcthorse", "correcthorse"));
        LCHECK(o.status == 500);
        LCHECK(!o.mint_session);
        LCHECK(g.unclaimed());             // and the camera is honestly still unclaimed
        LCHECK(o.body.find("still unclaimed") != std::string::npos);
    }

    // ---- the helper simply failing ------------------------------------------
    {
        FakeCamera cam;
        cam.claim_fails = true;
        SetupGate g = make_gate(cam);
        SetupOutcome o = g.post(form("correcthorse", "correcthorse"));
        LCHECK(o.status == 500);
        LCHECK(!o.mint_session);
        LCHECK(g.unclaimed());
        LCHECK(!o.body.empty());
    }

    // ---- the optional SSH key ----------------------------------------------
    {
        // A key the server refuses must cost a 400 on a camera that is STILL
        // unclaimed: /setup is gone afterwards, so a form you can correct is
        // the only second chance there is.
        const char* refused[] = {
            "not a key",
            "ssh-rsa",
            "ssh-magic AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyMaterial",
            "-----BEGIN OPENSSH PRIVATE KEY-----",
            "ssh-ed25519 AAAA$$$notbase64$$$ me@host",
            "ssh-ed25519 AAAA me@host",
        };
        for (const char* k : refused) {
            FakeCamera cam;
            cam.can_install_key = true;
            SetupGate g = make_gate(cam);
            SetupOutcome o = g.post(form("correcthorse", "correcthorse") + "&sshkey=" + k);
            LCHECK(o.status == 400);
            LCHECK(cam.claims == 0);        // refused BEFORE the camera was claimed
            LCHECK(g.unclaimed());
        }

        // a two-line key would smuggle in a second authorized_keys entry
        {
            std::string err;
            LCHECK(!SetupGate::valid_public_key(std::string(GOOD_KEY) + "\nssh-rsa AAAAB3NzaC1yc2EAAAA evil", err));
            LCHECK(SetupGate::valid_public_key(GOOD_KEY, err));
            LCHECK(SetupGate::valid_public_key("ssh-rsa AAAAB3NzaC1yc2EAAAADAQAB me@host", err));
            LCHECK(!SetupGate::valid_public_key("", err));
            LCHECK(!SetupGate::valid_public_key(std::string(SetupGate::MAX_SSHKEY + 1, 'a'), err));
        }

        // a good key with an installer: claimed, key installed, clean success
        {
            FakeCamera cam;
            cam.can_install_key = true;
            SetupGate g = make_gate(cam);
            SetupOutcome o = g.post(form("correcthorse", "correcthorse") + "&sshkey=" +
                                    "ssh-ed25519+AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyMaterialHere0123+me@host");
            LCHECK(o.status == 200);
            LCHECK(o.body.empty());
            LCHECK(o.mint_session);
            LCHECK(cam.installed_key.find("ssh-ed25519") == 0);
        }

        // the installer failing AFTER the claim must not undo the claim: the
        // camera really is set up, so it is a 200 with a message
        {
            FakeCamera cam;
            cam.can_install_key = true;
            cam.key_install_fails = true;
            SetupGate g = make_gate(cam);
            SetupOutcome o = g.post(form("correcthorse", "correcthorse") + "&sshkey=" +
                                    "ssh-ed25519+AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyMaterialHere0123+me@host");
            LCHECK(o.status == 200);
            LCHECK(!o.body.empty());        // "claimed, with something left undone"
            LCHECK(o.mint_session);
            LCHECK(g.state() == ClaimState::Claimed);
        }

        // no installer wired at all: same shape, so the key is never silently
        // dropped
        {
            FakeCamera cam;                 // can_install_key = false
            SetupGate g = make_gate(cam);
            SetupOutcome o = g.post(form("correcthorse", "correcthorse") + "&sshkey=" +
                                    "ssh-ed25519+AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyMaterialHere0123+me@host");
            LCHECK(o.status == 200);
            LCHECK(o.body.find("SSH key") != std::string::npos);
            LCHECK(o.mint_session);
        }
    }

    // ---- persistence across a restart --------------------------------------
    {
        // The state lives in the camera, not in the gate: a new gate over the
        // same camera sees the claim, which is what a daemon restart is.
        FakeCamera cam;
        {
            SetupGate g = make_gate(cam);
            LCHECK(g.post(form("correcthorse", "correcthorse")).status == 200);
        }
        SetupGate restarted = make_gate(cam);
        LCHECK(!restarted.unclaimed());
        LCHECK(restarted.post(form("correcthorse", "correcthorse")).status == 403);

        // and the login path works against the very same record - there is no
        // second credential store
        LCHECK(cam.verify("root", "correcthorse"));
        LCHECK(!cam.verify("root", "wrong"));
        LCHECK(!cam.verify("admin", "correcthorse"));
    }

    // ---- login after setup, through the real SessionGate -------------------
    {
        FakeCamera cam;
        SetupGate g = make_gate(cam);
        SessionGate sg([&cam](const std::string& u, const std::string& p) { return cam.verify(u, p); });

        // before the claim nobody can log in, even with an empty password
        LCHECK(sg.login("username=root&password=", 1000).status == 403);
        LCHECK(sg.login("username=root&password=correcthorse", 1000).status == 403);

        LCHECK(g.post(form("correcthorse", "correcthorse")).status == 200);

        SessionGate::LoginResult lr = sg.login("username=root&password=correcthorse", 1000);
        LCHECK(lr.status == 200);
        LCHECK(!lr.set_cookie.empty());
        LCHECK(sg.login("username=root&password=wrong", 1000).status == 403);

        // the session minted by the setup flow is a real one the gate accepts
        const std::string sc = sg.mint(2000);
        LCHECK(sc.find("Set-Cookie: session=") == 0);
        LCHECK(sc.find("HttpOnly") != std::string::npos);
        LCHECK(sc.find("SameSite=Strict") != std::string::npos);
        const size_t a = sc.find('=') + 1, b = sc.find(';');
        const std::string tok = sc.substr(a, b - a);
        LCHECK(sg.authed("session=" + tok, 2000));
        LCHECK(!sg.authed("session=madeup", 2000));
    }

    // ---- system.unsafe parses and defaults off -----------------------------
    {
        AppConfig cfg;
        std::string err;
        LCHECK(!cfg.system.unsafe);                       // off unless asked for
        LCHECK(parse_config_text("system.unsafe = true\n", cfg, err));
        LCHECK(cfg.system.unsafe);
        AppConfig c2;
        LCHECK(parse_config_text("system.unsafe = false\n", c2, err));
        LCHECK(!c2.system.unsafe);
        AppConfig c3;
        LCHECK(parse_config_text("system.unsafe = maybe\n", c3, err));
        LCHECK(!c3.system.unsafe);                        // unparseable never opens it
    }
}
