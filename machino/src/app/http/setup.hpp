// Application: the unclaimed / first-run flow, as the STOCK majestic-webui
// implements it. Contract read out of the upstream clone (www/setup.html,
// tests/setup-page.test.js, CLAUDE.md "Unclaimed cameras (first boot)"), not
// inferred from majestic's behaviour.
//
// THE STATE IS NOT STORED ANYWHERE OF OUR OWN. Upstream is explicit: the claim
// state IS the emptiness of root's hash field in /etc/shadow - "nothing else
// records the state, so whichever door sets the password, the other sees it
// immediately". The other door is SSH (`openipc-claim` as root's login shell).
// A second state file here would be a second truth that can disagree with the
// one the firmware and SSH already share, so there is none.
//
//   UNCLAIMED  --- POST /setup with a password that lands --->  CLAIMED
//
// While UNCLAIMED, upstream serves nothing but the claim flow: every other
// HTTP path answers 401 and browser navigation is redirected to /setup.html.
// Once CLAIMED, /setup.html 404s and POST /setup 403s - "an unauthenticated
// page that sets the root password must not outlive the state that justifies
// it". There is no un-claim here: the only supported way back is the firmware's
// own mechanism, which this package does not touch.
//
// Everything that needs the system - reading the shadow entry, setting the
// password, installing a key - is injected, so the decision logic is pure and
// host-tested and the Linux-only parts stay at the edge.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace machino { namespace http {

enum class ClaimState { Unclaimed, Claimed };

struct SetupOutcome {
    int         status = 200;
    // PLAIN TEXT, and shown to the operator verbatim: the page prints what the
    // camera said rather than inventing a message from the status code. On a
    // 200 a NON-EMPTY body means "claimed, but something was left undone" and
    // the page stays put and shows it instead of navigating on.
    std::string body;
    bool        mint_session = false;
};

class SetupGate {
public:
    // Reads the current claim state from the system (root's shadow entry).
    using StateFn  = std::function<ClaimState()>;
    // Sets root's password. Returns false and fills err on failure.
    using ClaimFn  = std::function<bool(const std::string& password, std::string& err)>;
    // Re-authenticates after the write: an exit status is not proof the hash
    // was written, so upstream checks the new password actually works.
    using VerifyFn = std::function<bool(const std::string& user, const std::string& pass)>;
    // Is a licence document present on this image? Upstream enforces the
    // acceptance parameter "whenever the document is on the image".
    using EulaFn   = std::function<bool()>;
    // Optional: install an authorized_keys entry. Absent = cannot install.
    using KeyFn    = std::function<bool(const std::string& key, std::string& err)>;

    SetupGate(StateFn state, ClaimFn claim, VerifyFn verify, EulaFn eula, KeyFn key = nullptr)
        : state_(std::move(state)), claim_(std::move(claim)),
          verify_(std::move(verify)), eula_(std::move(eula)), key_(std::move(key)) {}

    ClaimState state() const { return state_ ? state_() : ClaimState::Claimed; }
    bool unclaimed() const { return state() == ClaimState::Unclaimed; }

    // The only paths an UNCLAIMED camera answers. setup.html is self-contained
    // except for the licence documents it links and fetches, so those come too.
    static bool is_setup_path(const std::string& method, const std::string& path);

    SetupOutcome post(const std::string& form_body);

    // Upstream's own minimum, from setup.html's minlength="8".
    static constexpr size_t MIN_PASSWORD = 8;
    // crypt(3) only salts the first 72 bytes for bcrypt-class hashes and musl's
    // crypt refuses absurd input; 128 is far past any real passphrase and keeps
    // the request bounded.
    static constexpr size_t MAX_PASSWORD = 128;
    static constexpr size_t MAX_SSHKEY   = 4096;
    static constexpr size_t MAX_BODY     = 8192;

    // Exposed for tests: does this look like an SSH PUBLIC key, and is it
    // definitely not a private one?
    static bool valid_public_key(const std::string& key, std::string& err);

private:
    StateFn  state_;
    ClaimFn  claim_;
    VerifyFn verify_;
    EulaFn   eula_;
    KeyFn    key_;
};

}} // namespace machino::http
