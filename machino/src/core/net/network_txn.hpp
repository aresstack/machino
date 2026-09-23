// A network change that survives a crash.
//
// StagedChange (connectivity.hpp) protects against "the browser went away":
// it holds the undo in RAM. That is not enough for the failure this has to
// withstand. If the camera loses power, or the watchdog reboots it, or the
// daemon crashes between applying a WiFi change and confirming it, the undo is
// gone and the camera comes back on a configuration nobody ever confirmed --
// exactly the softbrick this machinery exists to prevent.
//
// TWO records, deliberately separate
// ---------------------------------
// An earlier version kept the confirmed configuration inside the same blob as
// the pending one. That looked tidy and was wrong: a torn write took the
// known-good configuration down with the attempt, and recovery then had
// nothing to restore -- it discarded the record and left whatever the boot
// scripts happened to do. So:
//
//   network-confirmed   the last configuration known to work. Written only on
//                       confirm. Never touched by a failing attempt.
//   network-pending     token + candidate. Written before the candidate is
//                       applied, removed on confirm, rollback or recovery.
//
//   begin()    write pending, then apply candidate
//   confirm()  write confirmed = candidate, then remove pending
//   tick()     window passed          -> apply confirmed, remove pending
//   recover()  pending found at start -> apply confirmed, remove pending
//
// A corrupt or truncated PENDING record is still a rollback: we know something
// was in flight even if we cannot read what. A corrupt CONFIRMED record is
// fail-closed -- it is reported and nothing is applied, because inventing a
// network configuration for a camera is worse than leaving it as it booted.
//
// Configurations are opaque strings. This class must not know what a WiFi or
// an IP configuration looks like; the caller serialises and applies. That also
// keeps secrets out of here: nothing in this file logs the content.
#pragma once
#include "core/result.hpp"
#include "core/state_store.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace machino { namespace net {

enum class SeedOutcome : int {
    Seeded = 0,         // there was none, one was written
    AlreadyPresent,     // a baseline exists; nothing to do
    Failed,             // it could not be written -- rollback is unavailable
};

enum class RecoverOutcome : int {
    Nothing = 0,        // no transaction was in flight
    RolledBack,         // an unconfirmed change was undone
    ConfirmedUnusable,  // the known-good record is gone or unreadable
};

struct PendingRecord {
    uint64_t    token = 0;
    std::string candidate;
};

class NetworkTxn {
public:
    using ApplyFn = std::function<Result(const std::string& config)>;

    NetworkTxn(IStateStore& store, ApplyFn apply,
               std::string confirmed_key = "network-confirmed",
               std::string pending_key = "network-pending");

    // Called once at start-up, before anything else touches the network.
    RecoverOutcome recover(std::string& err);

    // Seeds the known-good configuration when there is none yet (first boot).
    // Refuses to overwrite an existing one -- that is what confirm() is for.
    //
    // THREE outcomes, deliberately not a bool. The previous signature returned
    // false both for "a baseline is already there" (normal) and for "writing
    // one failed" (the rollback safety net does not exist), and a caller
    // collapsed them into the harmless reading. On the camera the other one
    // was true, and every staged change was refused for a whole release.
    SeedOutcome seed_confirmed(const std::string& config, std::string& err);

    bool begin(const std::string& candidate, uint32_t now_ms, uint32_t window_ms,
               uint64_t& token_out, std::string& err);

    bool confirm(uint64_t token, std::string& err);

    // Returns true when this call rolled back.
    bool tick(uint32_t now_ms);

    bool        pending() const;
    uint64_t    token() const;
    uint32_t    remaining_ms(uint32_t now_ms) const;
    bool        confirmed_config(std::string& out) const;

    // Exposed for tests: a truncated or garbage record must be handled.
    static std::string encode_pending(const PendingRecord& r);
    static bool        decode_pending(const std::string& text, PendingRecord& out);

private:
    bool load_confirmed(std::string& out) const;
    bool rollback_locked(std::string& err);

    IStateStore& store_;
    ApplyFn      apply_;
    std::string  ck_, pk_;

    mutable std::mutex m_;
    bool        pending_ = false;
    uint64_t    token_ = 0;
    uint64_t    next_token_ = 1;
    uint32_t    deadline_ms_ = 0;
    bool        deadline_valid_ = false;   // no timer survives a reboot
};

}} // namespace machino::net
