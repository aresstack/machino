// Crash-safe network transactions.
//
// The scenario: someone types a WiFi password into the web UI, the camera
// applies it, the camera drops off the network -- and then the power goes out
// before anyone could confirm. On the next boot it must come up on the
// configuration that was known to work.
//
// The awkward cases are the ones that earn the "crash-safe" label, so they are
// the bulk of this file: a torn pending record, a missing one, a corrupt
// confirmed one, and a crash between each pair of persistence steps.
#include "core/net/network_txn.hpp"
#include <cstdio>
#include <map>
#include <string>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// Survives across NetworkTxn instances, so "the process restarted" is modelled
// by building a new NetworkTxn on the same store.
struct MemStore : IStateStore {
    std::map<std::string, std::string> data;
    std::string fail_key;              // saves to this key fail
    int         fail_after = -1;       // or: fail from the Nth save on
    int         saves = 0;

    bool load(const std::string& k, std::string& out) const override {
        auto it = data.find(k);
        if (it == data.end()) return false;
        out = it->second;
        return true;
    }
    bool save(const std::string& k, const std::string& v) override {
        ++saves;
        if (!fail_key.empty() && k == fail_key) return false;
        if (fail_after >= 0 && saves > fail_after) return false;
        data[k] = v;
        return true;
    }
    void clear(const std::string& k) override { data.erase(k); }
};

struct Applier {
    std::string current = "BOOT-SCRIPTS";
    int  calls = 0;
    bool fail_next = false;
    Result operator()(const std::string& cfg) {
        ++calls;
        if (fail_next) { fail_next = false; return Result::error(); }
        current = cfg;
        return Result::ok();
    }
};

// A store that already holds a known-good configuration.
MemStore seeded(const char* good = "GOOD")
{
    MemStore st;
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    tx.seed_confirmed(good, err);
    return st;
}

void test_confirm_promotes_the_candidate()
{
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });

    uint64_t tok = 0; std::string err;
    TCHECK(tx.begin("NEW", 1000, 30000, tok, err));
    TCHECK(ap.current == "NEW");
    TCHECK(tx.pending());

    TCHECK(tx.confirm(tok, err));
    TCHECK(!tx.pending());
    std::string c;
    TCHECK(tx.confirmed_config(c) && c == "NEW");
    TCHECK(!tx.tick(99999));
}

void test_timeout_restores_the_confirmed_config()
{
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });

    uint64_t tok = 0; std::string err;
    TCHECK(tx.begin("NEW", 0, 10000, tok, err));
    TCHECK(!tx.tick(9000));
    TCHECK(tx.tick(10001));
    TCHECK(ap.current == "GOOD");
    std::string c;
    TCHECK(tx.confirmed_config(c) && c == "GOOD");
}

void test_restart_before_confirm_rolls_back()
{
    MemStore st = seeded();
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        TCHECK(tx.begin("NEW", 0, 60000, tok, err));
        TCHECK(ap.current == "NEW");
        // ... and here the process dies.
    }

    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    TCHECK(tx2.recover(err) == RecoverOutcome::RolledBack);
    TCHECK(ap2.current == "GOOD");
    TCHECK(!tx2.pending());
}

void test_restart_after_confirm_keeps_the_new_one()
{
    MemStore st = seeded();
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        tx.begin("NEW", 0, 60000, tok, err);
        TCHECK(tx.confirm(tok, err));
    }
    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    TCHECK(tx2.recover(err) == RecoverOutcome::Nothing);
    TCHECK(ap2.calls == 0);
    std::string c;
    TCHECK(tx2.confirmed_config(c) && c == "NEW");
}

// ------------------------------------------------- the damaged-record cases

void test_corrupt_pending_still_restores_confirmed()
{
    // The requirement in full: an unreadable pending record still means a
    // change was applied and never confirmed. An earlier version discarded it
    // and left the camera on whatever the boot scripts did.
    MemStore st = seeded();
    st.data["network-pending"] = "this is not a transaction";

    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(tx.recover(err) == RecoverOutcome::RolledBack);
    TCHECK(ap.current == "GOOD");
    TCHECK(err.find("unreadable") != std::string::npos);
    std::string left;
    TCHECK(!st.load("network-pending", left));      // and it is gone
}

void test_truncated_pending_still_restores_confirmed()
{
    MemStore st = seeded();
    PendingRecord pr; pr.token = 7; pr.candidate = "SOMETHING-LONG";
    const std::string full = NetworkTxn::encode_pending(pr);
    st.data["network-pending"] = full.substr(0, full.size() - 4);   // torn write

    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(tx.recover(err) == RecoverOutcome::RolledBack);
    TCHECK(ap.current == "GOOD");
}

void test_missing_pending_changes_nothing()
{
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(tx.recover(err) == RecoverOutcome::Nothing);
    TCHECK(ap.calls == 0);
}

void test_corrupt_confirmed_fails_closed()
{
    // Nothing is applied and the operator is told. Inventing a network
    // configuration for a camera is worse than leaving it as it booted.
    MemStore st;
    st.data["network-confirmed"] = "garbage";
    st.data["network-pending"] = NetworkTxn::encode_pending(PendingRecord{3, "NEW"});

    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(tx.recover(err) == RecoverOutcome::ConfirmedUnusable);
    TCHECK(ap.calls == 0);
    TCHECK(ap.current == "BOOT-SCRIPTS");
    TCHECK(err.find("missing or unreadable") != std::string::npos);
    // The pending record is deliberately NOT discarded: it is evidence.
    std::string left;
    TCHECK(st.load("network-pending", left));
}

void test_begin_refuses_without_a_confirmed_baseline()
{
    // Applying a candidate with nothing to fall back to is a one-way door.
    MemStore st;
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;
    TCHECK(!tx.begin("NEW", 0, 10000, tok, err));
    TCHECK(ap.calls == 0);
    TCHECK(err.find("no confirmed configuration") != std::string::npos);
}

// ---------------------------------------- crashes between persistence steps

void test_crash_between_writing_pending_and_applying()
{
    MemStore st = seeded();
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        ap.fail_next = true;                 // stands in for "died while applying"
        TCHECK(!tx.begin("NEW", 0, 10000, tok, err));
    }
    // Even so, the store must be back to a clean, known-good state.
    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    TCHECK(tx2.recover(err) == RecoverOutcome::Nothing);
    std::string c;
    TCHECK(tx2.confirmed_config(c) && c == "GOOD");
}

void test_crash_between_writing_confirmed_and_clearing_pending()
{
    // Order matters: confirmed first. A crash in between leaves a pending
    // record next to an already-correct confirmed one, and recovery re-applies
    // what is already in place -- harmless. The other order would lose it.
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;
    tx.begin("NEW", 0, 60000, tok, err);
    TCHECK(tx.confirm(tok, err));

    // Simulate the crash by putting the pending record back by hand.
    st.data["network-pending"] = NetworkTxn::encode_pending(PendingRecord{tok, "NEW"});

    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    TCHECK(tx2.recover(err) == RecoverOutcome::RolledBack);
    TCHECK(ap2.current == "NEW");            // which is what we wanted anyway
}

void test_a_failed_pending_write_prevents_the_change()
{
    MemStore st = seeded();
    st.fail_key = "network-pending";
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;
    TCHECK(!tx.begin("NEW", 0, 10000, tok, err));
    TCHECK(ap.calls == 0);
    TCHECK(err.find("refusing") != std::string::npos);
}

void test_a_failed_confirmed_write_keeps_the_change_pending()
{
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;
    tx.begin("NEW", 0, 10000, tok, err);

    st.fail_key = "network-confirmed";
    TCHECK(!tx.confirm(tok, err));
    TCHECK(tx.pending());                    // still guarded
    TCHECK(tx.tick(10001));                  // and the window still protects it
    TCHECK(ap.current == "GOOD");
}

// ----------------------------------------------------------------- tokens

void test_token_from_before_a_restart_cannot_confirm()
{
    MemStore st = seeded();
    uint64_t old_token = 0;
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        std::string err;
        TCHECK(tx.begin("B", 0, 60000, old_token, err));
    }
    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    tx2.recover(err);

    uint64_t new_token = 0;
    TCHECK(tx2.begin("C", 0, 60000, new_token, err));
    TCHECK(new_token != old_token);
    TCHECK(!tx2.confirm(old_token, err));
    TCHECK(tx2.confirm(new_token, err));
}

void test_seed_does_not_overwrite()
{
    MemStore st = seeded("FIRST");
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(!tx.seed_confirmed("SECOND", err));
    std::string c;
    TCHECK(tx.confirmed_config(c) && c == "FIRST");
}

void test_pending_encoding_rejects_every_truncation()
{
    PendingRecord r;
    r.token = 42;
    r.candidate = "ssid=a\npsk=b\n\nmore";
    const std::string enc = NetworkTxn::encode_pending(r);

    PendingRecord back;
    TCHECK(NetworkTxn::decode_pending(enc, back));
    TCHECK(back.token == 42 && back.candidate == r.candidate);

    bool any_accepted = false;
    for (size_t cut = 1; cut < enc.size(); ++cut) {
        PendingRecord junk;
        if (NetworkTxn::decode_pending(enc.substr(0, cut), junk)) { any_accepted = true; break; }
    }
    TCHECK(!any_accepted);
}

void test_second_change_is_refused_while_pending()
{
    MemStore st = seeded();
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t a = 0, b = 0; std::string err;
    TCHECK(tx.begin("B", 0, 10000, a, err));
    TCHECK(!tx.begin("C", 0, 10000, b, err));
    TCHECK(ap.current == "B");
}

void test_no_timer_survives_a_restart()
{
    MemStore st = seeded();
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        tx.begin("NEW", 1000, 30000, tok, err);
        TCHECK(tx.remaining_ms(11000) == 20000);
    }
    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    TCHECK(tx2.remaining_ms(0) == 0);        // recovery is immediate, not scheduled
    std::string err;
    TCHECK(tx2.recover(err) == RecoverOutcome::RolledBack);
}

} // namespace

void run_network_txn_tests()
{
    test_confirm_promotes_the_candidate();
    test_timeout_restores_the_confirmed_config();
    test_restart_before_confirm_rolls_back();
    test_restart_after_confirm_keeps_the_new_one();
    test_corrupt_pending_still_restores_confirmed();
    test_truncated_pending_still_restores_confirmed();
    test_missing_pending_changes_nothing();
    test_corrupt_confirmed_fails_closed();
    test_begin_refuses_without_a_confirmed_baseline();
    test_crash_between_writing_pending_and_applying();
    test_crash_between_writing_confirmed_and_clearing_pending();
    test_a_failed_pending_write_prevents_the_change();
    test_a_failed_confirmed_write_keeps_the_change_pending();
    test_token_from_before_a_restart_cannot_confirm();
    test_seed_does_not_overwrite();
    test_pending_encoding_rejects_every_truncation();
    test_second_change_is_refused_while_pending();
    test_no_timer_survives_a_restart();
}
