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

    {
        std::lock_guard<std::mutex> g(m_);
        if (pending_) { err = "another change is still waiting for confirmation"; return false; }
    }

    // Applied outside the lock: the action talks to the platform and may block.
    Result rc = forward();
    if (!rc.is_ok()) { err = "the change could not be applied"; return false; }

    std::lock_guard<std::mutex> g(m_);
    pending_ = true;
    token_ = next_token_++;
    deadline_ms_ = now_ms + window_ms;
    rollback_ = std::move(rollback);
    token_out = token_;
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
    auto find_type = [&](UplinkType t) -> INetworkUplink* {
        for (INetworkUplink* u : uplinks) if (u && u->type() == t) return u;
        return nullptr;
    };

    if (policy.pinned) {
        // Honour the pin even when it is down: the user asked for exactly this
        // one, and silently using another would make the status page a lie.
        return find_type(policy.pinned_type);
    }

    // Keep the current one unless it died or a preferred one came back and we
    // are allowed to return to it.
    if (usable(current) && !policy.return_to_preferred) return current;

    for (UplinkType t : policy.order) {
        INetworkUplink* u = find_type(t);
        if (usable(u)) {
            if (u == current) return current;
            // Only move off a working uplink for a strictly preferred one.
            if (!usable(current)) return u;
            if (!policy.return_to_preferred) return current;
            return u;
        }
        if (u == current && usable(current)) return current;
    }

    if (!policy.auto_failover) return current;
    return nullptr;
}

bool ConnectivityManager::evaluate()
{
    std::lock_guard<std::mutex> g(m_);
    INetworkUplink* chosen = select(uplinks_, policy_, active_);
    if (chosen == active_) return false;
    active_ = chosen;
    return true;
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
    std::lock_guard<std::mutex> g(m_);
    std::vector<UplinkStatus> out;
    out.reserve(uplinks_.size());
    for (INetworkUplink* u : uplinks_) {
        if (!u) continue;
        UplinkStatus s;
        s.type = u->type();
        s.state = u->state();
        s.internet = u->has_internet();
        s.active = (u == active_);
        s.info = u->info();
        s.metrics = u->metrics();
        out.push_back(s);
    }
    return out;
}

}} // namespace machino::net
