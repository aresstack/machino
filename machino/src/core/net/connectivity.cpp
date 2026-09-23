#include "core/net/connectivity.hpp"

#include <algorithm>

namespace machino { namespace net {

const char* uplink_type_name(UplinkType t)
{
    switch (t) {
        case UplinkType::Ethernet: return "ethernet";
        case UplinkType::Wifi:     return "wifi";
        case UplinkType::Cellular: return "cellular";
    }
    return "ethernet";
}

const char* link_state_name(LinkState s)
{
    switch (s) {
        case LinkState::Absent:     return "absent";
        case LinkState::Down:       return "down";
        case LinkState::Connecting: return "connecting";
        case LinkState::Connected:  return "connected";
        case LinkState::Failed:     return "failed";
    }
    return "absent";
}

const char* wifi_mode_name(WifiMode m)
{
    return m == WifiMode::AccessPoint ? "access-point" : "station";
}

bool wifi_mode_parse(const std::string& s, WifiMode& out)
{
    if (s == "station")      { out = WifiMode::Station;     return true; }
    if (s == "access-point") { out = WifiMode::AccessPoint; return true; }
    return false;
}

const char* wifi_security_name(WifiSecurity s)
{
    switch (s) {
        case WifiSecurity::Open:     return "open";
        case WifiSecurity::Wpa:      return "wpa";
        case WifiSecurity::Wpa2:     return "wpa2";
        case WifiSecurity::Wpa3:     return "wpa3";
        case WifiSecurity::Wpa2Wpa3: return "wpa2-wpa3";
        case WifiSecurity::Wep:      return "wep";
    }
    return "open";
}

bool wifi_security_parse(const std::string& s, WifiSecurity& out)
{
    if (s == "open")       { out = WifiSecurity::Open;     return true; }
    if (s == "wpa")        { out = WifiSecurity::Wpa;      return true; }
    if (s == "wpa2")       { out = WifiSecurity::Wpa2;     return true; }
    if (s == "wpa3")       { out = WifiSecurity::Wpa3;     return true; }
    if (s == "wpa2-wpa3")  { out = WifiSecurity::Wpa2Wpa3; return true; }
    if (s == "wep")        { out = WifiSecurity::Wep;      return true; }
    return false;
}

// ------------------------------------------------------------- StagedChange

bool StagedChange::begin(Action forward, Action rollback, uint32_t now_ms,
                         uint32_t window_ms, uint64_t& token_out, std::string& err)
{
    if (!forward || !rollback) { err = "a staged change needs both an action and its undo"; return false; }
    if (window_ms == 0)        { err = "a confirmation window of zero would roll back instantly"; return false; }

    // Claim the slot BEFORE applying. Checking and then acting outside the
    // lock would let two callers both pass the check and both apply, and the
    // rollback order would be undefined -- exactly the situation this class
    // exists to prevent.
    uint64_t tok;
    {
        std::lock_guard<std::mutex> g(m_);
        if (pending_) { err = "another change is still waiting for confirmation"; return false; }
        pending_ = true;
        tok = token_ = next_token_++;
        deadline_ms_ = now_ms + window_ms;
        rollback_ = nullptr;          // nothing to undo until forward succeeded
    }

    // Applied outside the lock: the action talks to the platform and may block.
    Result rc = forward();
    if (!rc.is_ok()) {
        std::lock_guard<std::mutex> g(m_);
        pending_ = false;
        err = "the change could not be applied";
        return false;
    }

    bool expired = false;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!pending_ || token_ != tok) {
            expired = true;     // a tick() fired while forward() was running
        } else {
            rollback_ = std::move(rollback);
            token_out = tok;
        }
    }
    if (expired) {
        // The change went through but is no longer guarded, and nobody holds a
        // token for it. Undo it here so that "begin() returned false" always
        // means "nothing changed" -- otherwise a caller that retries would
        // apply it twice.
        rollback();
        err = "the confirmation window expired while the change was being applied";
        return false;
    }
    return true;
}

bool StagedChange::confirm(uint64_t token, std::string& err)
{
    std::lock_guard<std::mutex> g(m_);
    if (!pending_)        { err = "nothing is waiting for confirmation"; return false; }
    if (token != token_)  { err = "that confirmation belongs to a different change"; return false; }
    pending_ = false;
    rollback_ = nullptr;
    return true;
}

bool StagedChange::tick(uint32_t now_ms)
{
    Action undo;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!pending_) return false;
        // Unsigned compare that survives the 32-bit wrap the rest of the
        // runtime uses for monotonic milliseconds.
        if ((int32_t)(now_ms - deadline_ms_) < 0) return false;
        undo = std::move(rollback_);
        rollback_ = nullptr;
        pending_ = false;
    }
    if (undo) undo();
    return true;
}

bool StagedChange::pending() const
{
    std::lock_guard<std::mutex> g(m_);
    return pending_;
}

uint64_t StagedChange::token() const
{
    std::lock_guard<std::mutex> g(m_);
    return token_;
}

uint32_t StagedChange::remaining_ms(uint32_t now_ms) const
{
    std::lock_guard<std::mutex> g(m_);
    if (!pending_) return 0;
    int32_t d = (int32_t)(deadline_ms_ - now_ms);
    return d > 0 ? (uint32_t)d : 0u;
}

// ------------------------------------------------------ ConnectivityManager

void ConnectivityManager::add(INetworkUplink* u)
{
    if (!u) return;
    std::lock_guard<std::mutex> g(m_);
    uplinks_.push_back(u);
}

void ConnectivityManager::set_policy(const UplinkPolicy& p)
{
    std::lock_guard<std::mutex> g(m_);
    policy_ = p;
}

UplinkPolicy ConnectivityManager::policy() const
{
    std::lock_guard<std::mutex> g(m_);
    return policy_;
}

INetworkUplink* ConnectivityManager::select(const std::vector<INetworkUplink*>& uplinks,
                                            const UplinkPolicy& policy,
                                            INetworkUplink* current)
{
    auto usable = [](INetworkUplink* u) {
        return u && u->state() == LinkState::Connected && u->has_internet();
    };

    // A selector is an uplink id or a type name. Ids win: they are the precise
    // answer, and a board with two modems needs them.
    auto match = [&](const std::string& sel, bool want_usable) -> INetworkUplink* {
        for (INetworkUplink* u : uplinks)
            if (u && u->id() == sel && (!want_usable || usable(u))) return u;
        for (INetworkUplink* u : uplinks)
            if (u && uplink_type_name(u->type()) == sel && (!want_usable || usable(u))) return u;
        return nullptr;
    };

    if (policy.pinned) {
        // Honour the pin even when it is down: the user asked for exactly this
        // one, and silently using another would make the status page a lie.
        return match(policy.pinned_uplink, false);
    }

    // The most preferred uplink that actually works right now.
    INetworkUplink* best = nullptr;
    for (const std::string& sel : policy.order) {
        INetworkUplink* u = match(sel, true);
        if (u) { best = u; break; }
    }

    // Nothing active yet: auto_failover governs whether we LEAVE a working
    // uplink, not whether we ever pick one. Without this a policy with
    // failover off would never connect at all.
    if (!current) return best;

    if (usable(current)) {
        if (!policy.return_to_preferred) return current;
        return best ? best : current;
    }

    // The active uplink died. Staying put is a legitimate choice.
    if (!policy.auto_failover) return current;
    return best;
}

bool ConnectivityManager::evaluate()
{
    // The uplink queries below read sysfs. Doing that while holding m_ would
    // put file I/O in a critical section shared with the API thread, so the
    // list is snapshotted first and select() runs unlocked.
    std::vector<INetworkUplink*> snapshot;
    UplinkPolicy policy;
    INetworkUplink* current = nullptr;
    {
        std::lock_guard<std::mutex> g(m_);
        snapshot = uplinks_;
        policy = policy_;
        current = active_;
    }

    INetworkUplink* chosen = select(snapshot, policy, current);

    std::vector<std::pair<uint64_t, PathChangeFn>> notify;
    std::string from, to;
    {
        std::lock_guard<std::mutex> g(m_);
        if (chosen == active_) return false;    // unchanged: nobody is told
        from = active_ ? active_->id() : std::string();
        to   = chosen  ? chosen->id()  : std::string();
        active_ = chosen;
        notify = subscribers_;                  // copied, so a subscriber may
    }                                           // unsubscribe from inside its own callback
    // Outside the lock: a transport reacting to this may tear down sessions,
    // and it must not do that while holding the connectivity mutex.
    for (const auto& s : notify) s.second(from, to);
    return true;
}

uint64_t ConnectivityManager::subscribe_path_change(PathChangeFn fn)
{
    if (!fn) return 0;
    std::lock_guard<std::mutex> g(m_);
    const uint64_t tok = next_sub_++;
    subscribers_.emplace_back(tok, std::move(fn));
    return tok;
}

void ConnectivityManager::unsubscribe_path_change(uint64_t token)
{
    std::lock_guard<std::mutex> g(m_);
    for (size_t i = 0; i < subscribers_.size(); ++i) {
        if (subscribers_[i].first == token) { subscribers_.erase(subscribers_.begin() + (long)i); return; }
    }
}

std::string ConnectivityManager::active_id() const
{
    std::lock_guard<std::mutex> g(m_);
    return active_ ? active_->id() : std::string();
}

INetworkUplink* ConnectivityManager::active() const
{
    std::lock_guard<std::mutex> g(m_);
    return active_;
}

bool ConnectivityManager::active_type(UplinkType& out) const
{
    std::lock_guard<std::mutex> g(m_);
    if (!active_) return false;
    out = active_->type();
    return true;
}

std::vector<UplinkStatus> ConnectivityManager::status() const
{
    // Same reason as evaluate(): info() and metrics() read sysfs, and the
    // status page is served from the HTTP thread. Snapshot, then query.
    std::vector<INetworkUplink*> snapshot;
    INetworkUplink* current = nullptr;
    {
        std::lock_guard<std::mutex> g(m_);
        snapshot = uplinks_;
        current = active_;
    }

    std::vector<UplinkStatus> out;
    out.reserve(snapshot.size());
    for (INetworkUplink* u : snapshot) {
        if (!u) continue;
        UplinkStatus s;
        s.id = u->id();
        s.type = u->type();
        s.state = u->state();
        s.internet = u->has_internet();
        s.active = (u == current);
        s.info = u->info();
        s.metrics = u->metrics();
        out.push_back(s);
    }
    return out;
}

}} // namespace machino::net
