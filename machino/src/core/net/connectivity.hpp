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
    // Most preferred first. Each entry is either an uplink ID ("wlan0",
    // "lte1") or a TYPE name ("ethernet", "wifi", "cellular"); an id wins over
    // a type when both could match.
    //
    // Types alone are not enough: a board can carry two modems or two radios,
    // and "cellular" could then not say which one. Types stay allowed because
    // they are what a single-radio board wants to write, and they keep working
    // when the interface is renamed.
    std::vector<std::string> order{"ethernet", "wifi", "cellular"};

    bool auto_failover = true;         // drop to the next one when the active dies
    bool return_to_preferred = true;   // go back up when a better one recovers

    // How long a candidate has to stay the answer before it is acted on.
    // Without this, one dropped ping flips the default route, every live RTSP
    // and WebRTC session is told the path changed, and the next tick flips it
    // back -- an uplink that is merely marginal then produces a permanent
    // stream of reconnects rather than a slightly worse picture.
    //
    // The two windows are different on purpose. Leaving a dead uplink is
    // urgent: nothing works until it happens. Going BACK to a preferred one
    // that has just recovered is not urgent at all, and a preferred uplink
    // that recovers and dies in a loop is the classic flap source -- so it
    // has to prove itself for considerably longer.
    uint32_t failover_debounce_ms = 3000;
    uint32_t return_debounce_ms = 15000;

    // When set, only this uplink is ever used and failover is off. This is the
    // "I know what I want" switch; it must be honoured even if it means no
    // connectivity, otherwise the UI lies.
    bool        pinned = false;
    std::string pinned_uplink;         // id or type name, same matching
};

struct UplinkStatus {
    std::string   id;
    UplinkType    type = UplinkType::Ethernet;
    LinkState     state = LinkState::Absent;
    bool          internet = false;
    bool          active = false;
    NetworkInfo   info;
    UplinkMetrics metrics;
};

// A change that undoes itself unless confirmed -- IN MEMORY ONLY.
//
// Use net::NetworkTxn (core/net/network_txn.hpp) for anything that can make
// the camera unreachable. This class survives a browser walking away; it does
// NOT survive a crash, a watchdog reboot or a power cut, because the undo
// lives in this object. For a WiFi or IP change that is not good enough, and
// picking the wrong one of the two is an easy mistake to make -- hence this
// note rather than a tidy-looking pair of siblings.
//
// What this one is still right for: a change that is merely annoying to lose,
// where a restart already puts things back by itself.
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
    // Monotonic milliseconds, the same source the rest of the runtime uses.
    //
    // Optional, and its absence is not a degraded mode -- it switches the
    // debounce off entirely. A host test that wants to see the selection
    // logic and nothing else installs no clock and gets an immediate answer;
    // a test that wants to see the damping installs one it controls. Guessing
    // a time here instead would make both kinds of test depend on wall clock.
    using ClockFn = std::function<uint32_t()>;
    void set_clock(ClockFn now) { now_ = std::move(now); }

    // Uplinks are borrowed; the caller owns them and outlives this.
    void add(INetworkUplink* u);
    void set_policy(const UplinkPolicy& p);
    UplinkPolicy policy() const;

    // Recompute the active uplink. Returns true when it changed.
    bool evaluate();

    // Called after the active uplink changed, OUTSIDE the lock.
    //
    // "Streaming over LTE is just a routing change" is true for the encoder
    // and false for anything holding a socket: the source address and the NAT
    // path change, so live TCP, RTSP and WebRTC sessions can break. Transports
    // subscribe here and decide for themselves -- ICE restart, reconnect, or
    // nothing. The media pipeline still never learns which uplink it is on.
    // Several transports subscribe independently -- RTSP, the MSE websocket
    // and WebRTC each decide for themselves. subscribe() returns a token;
    // unsubscribe() must be called before the subscriber is destroyed, and it
    // is safe to call from inside a notification (the manager copies the list
    // before dispatching, so removing yourself mid-callback cannot invalidate
    // the iteration).
    using PathChangeFn = std::function<void(const std::string& from_id, const std::string& to_id)>;
    uint64_t subscribe_path_change(PathChangeFn fn);
    void     unsubscribe_path_change(uint64_t token);

    INetworkUplink* active() const;
    bool            active_type(UplinkType& out) const;
    std::string     active_id() const;      // "" when nothing is active

    std::vector<UplinkStatus> status() const;

    // Which uplink the policy WOULD choose right now, ignoring what is active.
    // Split out so it can be tested without side effects.
    static INetworkUplink* select(const std::vector<INetworkUplink*>& uplinks,
                                  const UplinkPolicy& policy,
                                  INetworkUplink* current);

    // "Connected AND actually reaching something." The one definition, shared
    // by selection and by the route plan -- two definitions of usable would
    // mean the uplink the manager picks and the route the kernel gets could
    // be for different uplinks.
    static bool usable(const INetworkUplink* u);

private:
    mutable std::mutex m_;
    std::vector<INetworkUplink*> uplinks_;
    UplinkPolicy policy_;
    std::vector<std::pair<uint64_t, PathChangeFn>> subscribers_;
    uint64_t next_sub_ = 1;
    INetworkUplink* active_ = nullptr;
    ClockFn         now_;

    // The candidate that is currently serving its waiting period, and since
    // when. Reset whenever the answer changes, so a candidate that keeps
    // appearing and disappearing never accumulates time.
    INetworkUplink* pending_ = nullptr;
    uint32_t        pending_since_ms_ = 0;
};

}} // namespace machino::net
