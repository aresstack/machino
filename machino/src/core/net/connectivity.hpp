// Connectivity policy: which uplink carries traffic, and how a network change
// is allowed to be applied.
//
// Two responsibilities, both deliberately free of hardware:
//
//   1. Uplink selection. An ordered preference over Ethernet / WiFi / Cellular
//      with optional failover and return-to-preferred. Nothing above this asks
//      "are we on WiFi" -- the video path uses the IP stack and routing does
//      the rest, which is what makes "stream over LTE" a routing change and
//      not an encoder change.
//
//   2. Staged changes. A network setting that can lock the user out must not
//      be applied in one shot. OpenIPC has a standing softbrick report for
//      exactly this (firmware#1891): a wrong WiFi config and the camera is
//      gone. So a change applies, then has to be CONFIRMED within a window,
//      and rolls itself back if the confirmation never arrives -- because a
//      user who just lost the connection cannot confirm anything.
#pragma once
#include "core/result.hpp"
#include "ports/inetwork.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace net {

struct UplinkPolicy {
    // Most preferred first. Types not listed are never selected automatically.
    std::vector<UplinkType> order{UplinkType::Ethernet, UplinkType::Wifi, UplinkType::Cellular};
    bool auto_failover = true;         // drop to the next one when the active dies
    bool return_to_preferred = true;   // go back up when a better one recovers
    // When set, only this uplink is ever used and failover is off. This is the
    // "I know what I want" switch; it must be honoured even if it means no
    // connectivity, otherwise the UI lies.
    bool         pinned = false;
    UplinkType   pinned_type = UplinkType::Ethernet;
};

struct UplinkStatus {
    UplinkType    type = UplinkType::Ethernet;
    LinkState     state = LinkState::Absent;
    bool          internet = false;
    bool          active = false;
    NetworkInfo   info;
    UplinkMetrics metrics;
};

// A change that undoes itself unless confirmed. Generic on purpose: the same
// machinery protects a WiFi switch, an IP change and later an LTE APN.
class StagedChange {
public:
    using Action = std::function<Result()>;

    // Applies `forward` immediately. If confirm() does not arrive within
    // `window_ms`, the next tick() past the deadline runs `rollback`.
    // Refuses while another change is pending -- two staged changes at once
    // would make the rollback order undefined.
    bool begin(Action forward, Action rollback, uint32_t now_ms, uint32_t window_ms,
               uint64_t& token_out, std::string& err);

    bool confirm(uint64_t token, std::string& err);

    // Returns true when this call performed a rollback.
    bool tick(uint32_t now_ms);

    bool     pending() const;
    uint64_t token() const;
    uint32_t remaining_ms(uint32_t now_ms) const;

private:
    mutable std::mutex m_;
    bool     pending_ = false;
    uint64_t token_ = 0;
    uint64_t next_token_ = 1;
    uint32_t deadline_ms_ = 0;
    Action   rollback_;
};

class ConnectivityManager {
public:
    // Uplinks are borrowed; the caller owns them and outlives this.
    void add(INetworkUplink* u);
    void set_policy(const UplinkPolicy& p);
    UplinkPolicy policy() const;

    // Recompute the active uplink. Returns true when it changed.
    bool evaluate();

    INetworkUplink* active() const;
    bool            active_type(UplinkType& out) const;

    std::vector<UplinkStatus> status() const;

    // Which uplink the policy WOULD choose right now, ignoring what is active.
    // Split out so it can be tested without side effects.
    static INetworkUplink* select(const std::vector<INetworkUplink*>& uplinks,
                                  const UplinkPolicy& policy,
                                  INetworkUplink* current);

private:
    mutable std::mutex m_;
    std::vector<INetworkUplink*> uplinks_;
    UplinkPolicy policy_;
    INetworkUplink* active_ = nullptr;
};

}} // namespace machino::net
