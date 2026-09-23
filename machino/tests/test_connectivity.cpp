// AP36: uplink selection and the staged network change.
//
// The rollback tests are the reason this file exists. OpenIPC has a standing
// softbrick report (firmware#1891) where a wrong network config leaves the
// camera unreachable, and the user who caused it is by definition the one who
// can no longer confirm anything. So the machinery has to undo itself, and
// that behaviour has to be pinned by tests rather than hoped for.
#include "core/net/connectivity.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::net;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct FakeUplink : INetworkUplink {
    UplinkType  t;
    LinkState   st = LinkState::Connected;
    bool        inet = true;
    NetworkInfo ni;
    int         connects = 0, disconnects = 0;

    explicit FakeUplink(UplinkType type, const char* ifname) : t(type) { ni.ifname = ifname; }

    UplinkType    type() const override { return t; }
    LinkState     state() const override { return st; }
    NetworkInfo   info() const override { return ni; }
    UplinkMetrics metrics() const override { return UplinkMetrics{}; }
    Result connect() override { ++connects; return Result::ok(); }
    Result disconnect() override { ++disconnects; return Result::ok(); }
    bool has_internet() const override { return inet; }
};

// ------------------------------------------------------------ selection

void test_prefers_the_first_usable_in_order()
{
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink wifi(UplinkType::Wifi, "wlan0");
    ConnectivityManager m;
    m.add(&eth); m.add(&wifi);

    TCHECK(m.evaluate());
    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Ethernet);

    // Reorder: WiFi first.
    UplinkPolicy p;
    p.order = {UplinkType::Wifi, UplinkType::Ethernet, UplinkType::Cellular};
    m.set_policy(p);
    TCHECK(m.evaluate());
    TCHECK(m.active_type(t) && t == UplinkType::Wifi);
}

void test_failover_when_the_active_uplink_dies()
{
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink wifi(UplinkType::Wifi, "wlan0");
    ConnectivityManager m;
    m.add(&eth); m.add(&wifi);
    m.evaluate();

    eth.st = LinkState::Down;
    eth.inet = false;
    TCHECK(m.evaluate());
    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Wifi);
}

void test_connected_without_internet_is_not_usable()
{
    // A camera on a WLAN with no uplink is "connected" and useless; failover
    // has to see the difference.
    FakeUplink wifi(UplinkType::Wifi, "wlan0");
    FakeUplink cell(UplinkType::Cellular, "wwan0");
    wifi.inet = false;
    ConnectivityManager m;
    m.add(&wifi); m.add(&cell);

    UplinkPolicy p;
    p.order = {UplinkType::Wifi, UplinkType::Cellular};
    m.set_policy(p);

    m.evaluate();
    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Cellular);
}

void test_return_to_preferred()
{
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink cell(UplinkType::Cellular, "wwan0");
    eth.st = LinkState::Down; eth.inet = false;

    ConnectivityManager m;
    m.add(&eth); m.add(&cell);
    m.evaluate();
    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Cellular);

    eth.st = LinkState::Connected; eth.inet = true;
    TCHECK(m.evaluate());
    TCHECK(m.active_type(t) && t == UplinkType::Ethernet);
}

void test_no_return_to_preferred_keeps_the_working_one()
{
    // Avoids flapping back onto a marginal link, and avoids dropping a live
    // stream just because Ethernet blinked back.
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink cell(UplinkType::Cellular, "wwan0");
    eth.st = LinkState::Down; eth.inet = false;

    ConnectivityManager m;
    m.add(&eth); m.add(&cell);
    UplinkPolicy p; p.return_to_preferred = false;
    m.set_policy(p);
    m.evaluate();

    eth.st = LinkState::Connected; eth.inet = true;
    TCHECK(!m.evaluate());
    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Cellular);
}

void test_pinned_uplink_is_honoured_even_when_down()
{
    // Silently using another uplink would make the status page lie.
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink wifi(UplinkType::Wifi, "wlan0");
    wifi.st = LinkState::Down; wifi.inet = false;

    ConnectivityManager m;
    m.add(&eth); m.add(&wifi);
    UplinkPolicy p; p.pinned = true; p.pinned_type = UplinkType::Wifi;
    m.set_policy(p);
    m.evaluate();

    UplinkType t;
    TCHECK(m.active_type(t) && t == UplinkType::Wifi);
}

void test_nothing_usable_yields_no_active_uplink()
{
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    eth.st = LinkState::Down; eth.inet = false;
    ConnectivityManager m;
    m.add(&eth);
    m.evaluate();
    UplinkType t;
    TCHECK(!m.active_type(t));
}

void test_status_marks_exactly_one_active()
{
    FakeUplink eth(UplinkType::Ethernet, "eth0");
    FakeUplink wifi(UplinkType::Wifi, "wlan0");
    ConnectivityManager m;
    m.add(&eth); m.add(&wifi);
    m.evaluate();

    auto st = m.status();
    TCHECK(st.size() == 2);
    int active = 0;
    for (const auto& s : st) if (s.active) ++active;
    TCHECK(active == 1);
    TCHECK(st[0].info.ifname == "eth0");
}

// --------------------------------------------------------- staged change

void test_staged_change_rolls_back_without_confirmation()
{
    StagedChange sc;
    int applied = 0, undone = 0;
    uint64_t tok = 0;
    std::string err;

    TCHECK(sc.begin([&]{ ++applied; return Result::ok(); },
                    [&]{ ++undone;  return Result::ok(); },
                    1000, 30000, tok, err));
    TCHECK(applied == 1 && undone == 0 && sc.pending());

    TCHECK(!sc.tick(20000));            // still inside the window
    TCHECK(sc.remaining_ms(20000) == 11000);
    TCHECK(sc.tick(31001));             // deadline passed -> undo
    TCHECK(undone == 1 && !sc.pending());
    TCHECK(!sc.tick(40000));            // only once
}

void test_confirmed_change_is_not_rolled_back()
{
    StagedChange sc;
    int undone = 0;
    uint64_t tok = 0;
    std::string err;

    TCHECK(sc.begin([]{ return Result::ok(); },
                    [&]{ ++undone; return Result::ok(); },
                    0, 30000, tok, err));
    TCHECK(sc.confirm(tok, err));
    TCHECK(!sc.pending());
    TCHECK(!sc.tick(999999));
    TCHECK(undone == 0);
}

void test_wrong_token_does_not_confirm()
{
    StagedChange sc;
    int undone = 0;
    uint64_t tok = 0;
    std::string err;
    sc.begin([]{ return Result::ok(); }, [&]{ ++undone; return Result::ok(); }, 0, 10000, tok, err);

    TCHECK(!sc.confirm(tok + 1, err));
    TCHECK(sc.pending());
    TCHECK(sc.tick(10001) && undone == 1);
}

void test_failed_forward_leaves_nothing_pending()
{
    StagedChange sc;
    int undone = 0;
    uint64_t tok = 0;
    std::string err;
    TCHECK(!sc.begin([]{ return Result::error(); },
                     [&]{ ++undone; return Result::ok(); },
                     0, 10000, tok, err));
    TCHECK(!sc.pending());
    TCHECK(undone == 0);           // nothing was applied, nothing to undo
    TCHECK(!sc.tick(99999));
}

void test_second_change_is_refused_while_one_is_pending()
{
    StagedChange sc;
    uint64_t a = 0, b = 0;
    std::string err;
    TCHECK(sc.begin([]{ return Result::ok(); }, []{ return Result::ok(); }, 0, 10000, a, err));
    TCHECK(!sc.begin([]{ return Result::ok(); }, []{ return Result::ok(); }, 0, 10000, b, err));
    TCHECK(err.find("still waiting") != std::string::npos);
}

void test_zero_window_is_refused()
{
    StagedChange sc;
    uint64_t tok = 0;
    std::string err;
    TCHECK(!sc.begin([]{ return Result::ok(); }, []{ return Result::ok(); }, 0, 0, tok, err));
}

void test_rollback_survives_the_millisecond_wrap()
{
    // The runtime's monotonic clock is a wrapping uint32_t; a deadline that
    // straddles the wrap must still fire exactly once.
    StagedChange sc;
    int undone = 0;
    uint64_t tok = 0;
    std::string err;
    const uint32_t near_wrap = 0xFFFFF000u;
    TCHECK(sc.begin([]{ return Result::ok(); }, [&]{ ++undone; return Result::ok(); },
                    near_wrap, 8192, tok, err));
    TCHECK(!sc.tick(near_wrap + 1000));
    TCHECK(sc.tick(near_wrap + 9000));      // wrapped past the deadline
    TCHECK(undone == 1);
}

void test_name_round_trips()
{
    WifiMode m;
    TCHECK(wifi_mode_parse("station", m) && m == WifiMode::Station);
    TCHECK(wifi_mode_parse("access-point", m) && m == WifiMode::AccessPoint);
    TCHECK(!wifi_mode_parse("ap", m));

    for (WifiSecurity s : {WifiSecurity::Open, WifiSecurity::Wpa2, WifiSecurity::Wpa3,
                           WifiSecurity::Wpa2Wpa3, WifiSecurity::Wep}) {
        WifiSecurity back;
        TCHECK(wifi_security_parse(wifi_security_name(s), back) && back == s);
    }
    TCHECK(std::string(uplink_type_name(UplinkType::Cellular)) == "cellular");
    TCHECK(std::string(link_state_name(LinkState::Connecting)) == "connecting");
}

} // namespace

void run_connectivity_tests()
{
    test_prefers_the_first_usable_in_order();
    test_failover_when_the_active_uplink_dies();
    test_connected_without_internet_is_not_usable();
    test_return_to_preferred();
    test_no_return_to_preferred_keeps_the_working_one();
    test_pinned_uplink_is_honoured_even_when_down();
    test_nothing_usable_yields_no_active_uplink();
    test_status_marks_exactly_one_active();
    test_staged_change_rolls_back_without_confirmation();
    test_confirmed_change_is_not_rolled_back();
    test_wrong_token_does_not_confirm();
    test_failed_forward_leaves_nothing_pending();
    test_second_change_is_refused_while_one_is_pending();
    test_zero_window_is_refused();
    test_rollback_survives_the_millisecond_wrap();
    test_name_round_trips();
}
