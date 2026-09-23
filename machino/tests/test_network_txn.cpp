// Crash-safe network transactions.
//
// The scenario these exist for: someone types a WiFi password over the web UI,
// the camera applies it, the camera drops off the network -- and then the
// power goes out before anyone could confirm. On the next boot it must come up
// on the configuration that was known to work, not on the one that lost it.
#include "core/net/network_txn.hpp"
#include <cstdio>
#include <map>
#include <string>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// Survives across NetworkTxn instances, so "the process restarted" can be
// modelled by building a new NetworkTxn on the same store.
struct MemStore : IStateStore {
    std::map<std::string, std::string> data;
    bool fail_save = false;
    int  saves = 0;

    bool load(const std::string& k, std::string& out) const override {
        auto it = data.find(k);
        if (it == data.end()) return false;
        out = it->second;
        return true;
    }
    bool save(const std::string& k, const std::string& v) override {
        ++saves;
        if (fail_save) return false;
        data[k] = v;
        return true;
    }
    void clear(const std::string& k) override { data.erase(k); }
};

struct Applier {
    std::string current = "CONFIRMED";
    int  calls = 0;
    bool fail_next = false;
    Result operator()(const std::string& cfg) {
        ++calls;
        if (fail_next) { fail_next = false; return Result::error(); }
        current = cfg;
        return Result::ok();
    }
};

void test_confirm_keeps_the_candidate()
{
    MemStore st; Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });

    uint64_t tok = 0; std::string err;
    TCHECK(tx.begin("OLD", "NEW", 1000, 30000, tok, err));
    TCHECK(ap.current == "NEW");
    TCHECK(tx.pending());

    TCHECK(tx.confirm(tok, err));
    TCHECK(!tx.pending());
    TCHECK(tx.confirmed_config() == "NEW");
    TCHECK(!tx.tick(99999));            // nothing left to roll back
    TCHECK(ap.current == "NEW");
}

void test_timeout_restores_the_confirmed_config()
{
    MemStore st; Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });

    uint64_t tok = 0; std::string err;
    TCHECK(tx.begin("OLD", "NEW", 0, 10000, tok, err));
    TCHECK(ap.current == "NEW");

    TCHECK(!tx.tick(9000));
    TCHECK(tx.tick(10001));
    TCHECK(ap.current == "OLD");
    TCHECK(!tx.pending());
    TCHECK(tx.confirmed_config() == "OLD");
}

void test_process_restart_before_confirm_rolls_back()
{
    // The point of the whole file: a new NetworkTxn over the same store is a
    // restarted daemon.
    MemStore st;
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        TCHECK(tx.begin("OLD", "NEW", 0, 60000, tok, err));
        TCHECK(ap.current == "NEW");
        // ... and here the process dies. No tick, no confirm.
    }

    Applier ap2;
    ap2.current = "WHATEVER-THE-BOOT-SCRIPTS-DID";
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    TCHECK(tx2.recover(err));
    TCHECK(ap2.current == "OLD");
    TCHECK(!tx2.pending());
    TCHECK(tx2.confirmed_config() == "OLD");
    TCHECK(err.find("rolled back") != std::string::npos);
}

void test_restart_after_confirm_keeps_the_new_config()
{
    MemStore st;
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        tx.begin("OLD", "NEW", 0, 60000, tok, err);
        TCHECK(tx.confirm(tok, err));
    }

    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    TCHECK(!tx2.recover(err));          // nothing to undo
    TCHECK(ap2.calls == 0);             // and nothing re-applied
    TCHECK(tx2.confirmed_config() == "NEW");
}

void test_corrupt_record_is_discarded_not_trusted()
{
    MemStore st;
    st.data["network-txn"] = "v1\n7\n1\n999\nshort";   // length lies about the rest

    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(!tx.recover(err));
    TCHECK(ap.calls == 0);                              // nothing guessed
    TCHECK(err.find("unreadable") != std::string::npos);
    std::string left;
    TCHECK(!st.load("network-txn", left));              // and it is gone
}

void test_garbage_record_is_discarded()
{
    MemStore st;
    st.data["network-txn"] = "this is not a transaction";
    Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    std::string err;
    TCHECK(!tx.recover(err));
    TCHECK(ap.calls == 0);
}

void test_stale_token_cannot_confirm()
{
    MemStore st; Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;
    tx.begin("OLD", "NEW", 0, 10000, tok, err);

    TCHECK(!tx.confirm(tok + 1, err));
    TCHECK(tx.pending());
    TCHECK(tx.tick(10001));
    TCHECK(ap.current == "OLD");

    // And a confirmation arriving after the rollback must not resurrect it.
    TCHECK(!tx.confirm(tok, err));
    TCHECK(ap.current == "OLD");
}

void test_a_token_from_before_a_restart_cannot_confirm_a_later_change()
{
    // Regression: the token counter restarted at 1 with the process, so a
    // browser still holding a token from before the crash could confirm a
    // DIFFERENT change that happened to get the same number.
    MemStore st;
    uint64_t old_token = 0;
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        std::string err;
        TCHECK(tx.begin("A", "B", 0, 60000, old_token, err));
    }

    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    std::string err;
    tx2.recover(err);

    uint64_t new_token = 0;
    TCHECK(tx2.begin("A", "C", 0, 60000, new_token, err));
    TCHECK(new_token != old_token);
    TCHECK(!tx2.confirm(old_token, err));       // the stale one must not work
    TCHECK(tx2.pending());
    TCHECK(tx2.confirm(new_token, err));        // the right one still does
}

void test_failed_write_refuses_to_apply()
{
    // If we cannot write down how to undo it, we must not do it.
    MemStore st; Applier ap;
    st.fail_save = true;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t tok = 0; std::string err;

    TCHECK(!tx.begin("OLD", "NEW", 0, 10000, tok, err));
    TCHECK(ap.calls == 0);
    TCHECK(ap.current == "CONFIRMED");
    TCHECK(err.find("refusing") != std::string::npos);
}

void test_failed_apply_restores_and_reports()
{
    MemStore st; Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    ap.fail_next = true;
    uint64_t tok = 0; std::string err;

    TCHECK(!tx.begin("OLD", "NEW", 0, 10000, tok, err));
    TCHECK(!tx.pending());
    TCHECK(ap.current == "OLD");        // the known-good one was put back
    TCHECK(tx.confirmed_config() == "OLD");
}

void test_second_change_is_refused_while_pending()
{
    MemStore st; Applier ap;
    NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
    uint64_t a = 0, b = 0; std::string err;
    TCHECK(tx.begin("OLD", "NEW", 0, 10000, a, err));
    TCHECK(!tx.begin("OLD", "OTHER", 0, 10000, b, err));
    TCHECK(ap.current == "NEW");
}

void test_encoding_round_trips_awkward_payloads()
{
    // Configurations contain newlines and '='; a naive key=value format would
    // lose them and a truncated file would look valid.
    TxnRecord r;
    r.token = 42;
    r.pending = true;
    r.confirmed = "ssid=a\npsk=b\n\nmore";
    r.candidate = "";
    std::string enc = NetworkTxn::encode(r);

    TxnRecord back;
    TCHECK(NetworkTxn::decode(enc, back));
    TCHECK(back.token == 42 && back.pending);
    TCHECK(back.confirmed == r.confirmed);
    TCHECK(back.candidate.empty());

    for (size_t cut = 1; cut < enc.size(); ++cut) {
        TxnRecord junk;
        // Every truncation must be rejected, never half-read.
        if (NetworkTxn::decode(enc.substr(0, cut), junk)) {
            ++g_fail_ext;
            fprintf(stderr, "FAIL truncation at %zu accepted\n", cut);
            break;
        }
    }
    ++g_pass_ext;
}

void test_remaining_ms_and_no_timer_after_restart()
{
    MemStore st;
    {
        Applier ap;
        NetworkTxn tx(st, [&](const std::string& c) { return ap(c); });
        uint64_t tok = 0; std::string err;
        tx.begin("OLD", "NEW", 1000, 30000, tok, err);
        TCHECK(tx.remaining_ms(11000) == 20000);
    }
    // After a restart there is no deadline to count down; recovery is
    // immediate, not scheduled.
    Applier ap2;
    NetworkTxn tx2(st, [&](const std::string& c) { return ap2(c); });
    TCHECK(tx2.remaining_ms(0) == 0);
    std::string err;
    TCHECK(tx2.recover(err));
    TCHECK(ap2.current == "OLD");
}

} // namespace

void run_network_txn_tests()
{
    test_confirm_keeps_the_candidate();
    test_timeout_restores_the_confirmed_config();
    test_process_restart_before_confirm_rolls_back();
    test_restart_after_confirm_keeps_the_new_config();
    test_corrupt_record_is_discarded_not_trusted();
    test_garbage_record_is_discarded();
    test_stale_token_cannot_confirm();
    test_a_token_from_before_a_restart_cannot_confirm_a_later_change();
    test_failed_write_refuses_to_apply();
    test_failed_apply_restores_and_reports();
    test_second_change_is_refused_while_pending();
    test_encoding_round_trips_awkward_payloads();
    test_remaining_ms_and_no_timer_after_restart();
}
