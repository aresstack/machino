// A network change that survives a crash.
//
// StagedChange (connectivity.hpp) protects against "the browser went away":
// it holds the undo in RAM and runs it when the confirmation window passes.
// That is not enough for the failure this actually has to withstand. If the
// camera loses power, or the watchdog reboots it, or the daemon crashes
// between applying a WiFi change and confirming it, the undo is gone and the
// camera comes back on a configuration nobody ever confirmed -- which is
// exactly the softbrick this machinery exists to prevent.
//
// So the transaction is written down BEFORE it is applied:
//
//   begin()    persist {confirmed, candidate, token, pending}  then apply
//   confirm()  persist {confirmed = candidate}                 clear pending
//   tick()     window passed          -> re-apply confirmed, clear
//   recover()  found pending at start -> re-apply confirmed, clear
//
// Configurations are opaque strings. This class must not know what a WiFi or
// an IP configuration looks like; the caller serialises and applies. That also
// keeps secrets out of here: the caller decides what goes into the blob, and
// nothing in this file ever logs it.
#pragma once
#include "core/result.hpp"
#include "core/state_store.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace machino { namespace net {

enum class TxnState : int { Idle = 0, Pending };

struct TxnRecord {
    uint64_t    token = 0;
    std::string confirmed;    // the configuration known to work
    std::string candidate;    // what we are trying
    bool        pending = false;
};

class NetworkTxn {
public:
    // `apply` installs a serialized configuration. It is called for the
    // candidate on begin(), and for the confirmed one on rollback/recovery.
    using ApplyFn = std::function<Result(const std::string& config)>;

    NetworkTxn(IStateStore& store, ApplyFn apply, std::string key = "network-txn");

    // Called once at start-up, before anything else touches the network.
    // Returns true when it rolled something back.
    bool recover(std::string& err);

    // Persist first, then apply. On a failed apply the record is cleared and
    // the confirmed configuration is re-applied, so a caller that sees false
    // knows nothing changed.
    bool begin(const std::string& confirmed, const std::string& candidate,
               uint32_t now_ms, uint32_t window_ms,
               uint64_t& token_out, std::string& err);

    bool confirm(uint64_t token, std::string& err);

    // Returns true when this call rolled back.
    bool tick(uint32_t now_ms);

    bool        pending() const;
    uint64_t    token() const;
    uint32_t    remaining_ms(uint32_t now_ms) const;
    std::string confirmed_config() const;

    // Serialisation is exposed for tests: a truncated or garbage record must
    // be handled, not trusted.
    static std::string encode(const TxnRecord& r);
    static bool         decode(const std::string& text, TxnRecord& out);

private:
    bool rollback_locked(std::string& err);

    IStateStore& store_;
    ApplyFn      apply_;
    std::string  key_;

    mutable std::mutex m_;
    TxnRecord   rec_;
    uint32_t    deadline_ms_ = 0;
    bool        deadline_valid_ = false;   // false after a reboot: no timer survives
    uint64_t    next_token_ = 1;
};

}} // namespace machino::net
