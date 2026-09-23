// AP35.20: generic USB host policy. No hardware, no platform names -- the
// backend is a fake, and every assertion is about the decision the service
// makes, not about what a T40 happens to do.
//
// The pin-safety cases are the ones that matter most: while finding the real
// power pin on the Vanhua board we came within one command of driving the
// Ethernet PHY reset, because it was the only GPIO output that looked
// interesting. A generic USB page with a free GPIO dropdown would hand that
// footgun to every user, so the service refuses pins the board does not
// declare unless the caller explicitly asks to be trusted.
#include "core/usb/usb_host_service.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::usb;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A stand-in for a board that switches port power through one wired pin.
struct FakeBackend : IUsbHostBackend {
    UsbCapabilities caps;
    bool            host = true;
    bool            power = false;
    bool            power_readable = true;
    bool            refuse_set = false;

    // what the service last asked for
    UsbPowerMode last_mode = UsbPowerMode::None;
    std::string  last_pin;
    bool         last_active_high = false;
    bool         last_on = false;
    int          set_calls = 0;

    std::vector<UsbDevice> device_list;

    UsbCapabilities capabilities() const override { return caps; }
    bool host_active() const override { return host; }

    Result set_power(UsbPowerMode m, const std::string& pin, bool active_high, bool on) override {
        ++set_calls;
        if (refuse_set) return Result::error();
        last_mode = m; last_pin = pin; last_active_high = active_high; last_on = on;
        power = on;
        return Result::ok();
    }
    bool power_state(bool& on_out) const override {
        if (!power_readable) return false;
        on_out = power; return true;
    }
    std::vector<UsbDevice> devices() const override { return device_list; }
};

UsbCapabilities switchable_board()
{
    UsbCapabilities c;
    c.host_supported = true;
    c.controller = "dwc2";
    c.max_speed = "high";
    c.power.switchable = true;
    c.power.default_pin = "PB18";
    c.power.default_active_high = true;
    c.power.voltage_mv = 3300;
    c.power.allowed_pins = {"PB18"};
    return c;
}

void test_board_default_resolves_to_the_wired_pin()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true;   // mode stays BoardDefault
    std::string err;
    TCHECK(s.apply(cfg, err).is_ok());
    TCHECK(err.empty());
    TCHECK(b.set_calls == 1);
    TCHECK(b.last_mode == UsbPowerMode::Gpio);
    TCHECK(b.last_pin == "PB18");
    TCHECK(b.last_active_high == true);
    TCHECK(b.last_on == true);

    // The resolved view must show the concrete decision, not the request.
    UsbStatus st = s.status();
    TCHECK(st.resolved.mode == UsbPowerMode::Gpio);
    TCHECK(st.resolved.pin == "PB18");
    TCHECK(st.resolved.drives_power);
    TCHECK(st.power_known && st.power_on);
}

void test_disable_turns_the_port_off()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);
    std::string err;

    UsbConfig on; on.enabled = true;
    TCHECK(s.apply(on, err).is_ok() && b.power);

    UsbConfig off; off.enabled = false;
    TCHECK(s.apply(off, err).is_ok());
    TCHECK(b.last_on == false && b.power == false);
}

void test_unlisted_pin_is_refused_without_expert()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true;
    cfg.mode = UsbPowerMode::Gpio;
    cfg.pin  = "PC7";              // the Ethernet PHY reset on the real board
    std::string err;

    TCHECK(!s.apply(cfg, err).is_ok());
    TCHECK(err.find("PC7") != std::string::npos);
    TCHECK(b.set_calls == 0);      // nothing was attempted
}

void test_expert_allows_an_unlisted_pin()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true;
    cfg.mode = UsbPowerMode::Gpio;
    cfg.pin  = "PD3";
    cfg.active_high = false;
    cfg.expert = true;
    std::string err;

    TCHECK(s.apply(cfg, err).is_ok());
    TCHECK(b.last_pin == "PD3");
    TCHECK(b.last_active_high == false);
}

void test_explicit_pin_keeps_its_own_polarity()
{
    // When the caller names a pin they also own the polarity; only the default
    // pin inherits the board's.
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true;
    cfg.mode = UsbPowerMode::Gpio;
    cfg.pin = "PB18";
    cfg.active_high = false;
    std::string err;

    TCHECK(s.apply(cfg, err).is_ok());
    TCHECK(b.last_active_high == false);
}

void test_always_on_is_refused_on_a_switchable_board()
{
    // Otherwise the port stays dark and looks like a driver bug.
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true; cfg.mode = UsbPowerMode::AlwaysOn;
    std::string err;
    TCHECK(!s.apply(cfg, err).is_ok());
    TCHECK(b.set_calls == 0);
}

void test_always_on_board_needs_no_pin()
{
    FakeBackend b;
    b.caps.host_supported = true;
    b.caps.controller = "ehci";
    b.caps.power.switchable = false;      // hard-wired rail
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true;    // BoardDefault
    std::string err;
    TCHECK(s.apply(cfg, err).is_ok());
    // The backend is told even though there is nothing to switch: it is the
    // only way it learns to stop driving a pin it held before.
    TCHECK(b.set_calls == 1);
    TCHECK(b.last_mode == UsbPowerMode::AlwaysOn);
    TCHECK(s.status().resolved.mode == UsbPowerMode::AlwaysOn);
    TCHECK(!s.status().resolved.drives_power);
}

void test_switching_to_none_reaches_the_backend()
{
    // Regression: the service used to call set_power() only for Gpio, so a
    // switch from gpio to none never reached the backend -- it kept the pin
    // asserted and the port stayed powered while the mode said otherwise.
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);
    std::string err;

    UsbConfig on; on.enabled = true;
    TCHECK(s.apply(on, err).is_ok());
    TCHECK(b.last_mode == UsbPowerMode::Gpio && b.power);

    UsbConfig none; none.enabled = true; none.mode = UsbPowerMode::None;
    TCHECK(s.apply(none, err).is_ok());
    TCHECK(b.last_mode == UsbPowerMode::None);
    TCHECK(s.status().resolved.mode == UsbPowerMode::None);
}

void test_gpio_mode_on_a_board_that_cannot_switch()
{
    FakeBackend b;
    b.caps.host_supported = true;
    b.caps.power.switchable = false;
    UsbHostService s(b);

    UsbConfig cfg; cfg.enabled = true; cfg.mode = UsbPowerMode::Gpio; cfg.pin = "PB18";
    std::string err;
    TCHECK(!s.apply(cfg, err).is_ok());
    TCHECK(err.find("cannot switch") != std::string::npos);
}

void test_board_without_usb_host()
{
    FakeBackend b;                         // host_supported stays false
    b.host = false;
    UsbHostService s(b);
    std::string err;

    UsbConfig want; want.enabled = true;
    TCHECK(!s.apply(want, err).is_ok());   // asking for it is an error
    TCHECK(err.find("no USB host") != std::string::npos);

    UsbConfig off; off.enabled = false;
    err.clear();
    TCHECK(s.apply(off, err).is_ok());     // not wanting it is not
}

void test_backend_refusal_leaves_state_unchanged()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);
    std::string err;

    UsbConfig ok; ok.enabled = true;
    TCHECK(s.apply(ok, err).is_ok());

    b.refuse_set = true;
    UsbConfig other; other.enabled = true; other.mode = UsbPowerMode::Gpio;
    other.pin = "PB18"; other.active_high = false;
    TCHECK(!s.apply(other, err).is_ok());

    // The stored config must still be the one that actually took effect.
    TCHECK(s.config().active_high == true);
    TCHECK(s.status().resolved.active_high == true);
}

void test_enable_at_boot_is_honoured()
{
    FakeBackend b; b.caps = switchable_board();
    UsbHostService s(b);
    std::string err;

    UsbConfig cfg; cfg.enabled = true; cfg.enable_at_boot = false;
    TCHECK(s.apply(cfg, err).is_ok());
    int before = b.set_calls;

    TCHECK(s.apply_at_boot(err).is_ok());
    TCHECK(b.set_calls == before);          // boot path did not touch the port

    UsbConfig boot = cfg; boot.enable_at_boot = true;
    TCHECK(s.apply(boot, err).is_ok());
    before = b.set_calls;
    TCHECK(s.apply_at_boot(err).is_ok());
    TCHECK(b.set_calls == before + 1);
}

void test_devices_are_passed_through_untouched()
{
    FakeBackend b; b.caps = switchable_board();
    UsbDevice d; d.path = "1-1"; d.vid = "a69c"; d.pid = "88dc";
    d.manufacturer = "AICSemi"; d.product = "AIC8800DC"; d.speed = "480"; d.max_power_ma = 500;
    UsbInterface i0; i0.cls = "e0"; i0.subclass = "01"; i0.protocol = "01";
    UsbInterface i2; i2.cls = "ff"; i2.subclass = "ff"; i2.protocol = "ff";
    d.interfaces = {i0, i2};
    b.device_list = {d};

    UsbHostService s(b);
    UsbStatus st = s.status();
    TCHECK(st.devices.size() == 1);
    TCHECK(st.devices[0].vid == "a69c" && st.devices[0].pid == "88dc");
    TCHECK(st.devices[0].interfaces.size() == 2);
    // The service must not interpret a VID:PID -- no device-class logic here.
    TCHECK(st.devices[0].product == "AIC8800DC");
}

void test_null_backend_accepts_being_switched_off()
{
    // Regression, and a lesson about fakes: the FakeBackend above answers Ok to
    // every set_power, so it hid this. The real NullUsbHostBackend answers
    // Unsupported, and once the service started telling the backend about
    // EVERY mode, "disable USB on a board that has none" turned into a
    // failure. Tested against the real class, not the fake.
    NullUsbHostBackend nb;
    UsbHostService s(nb);
    std::string err;

    UsbConfig off;
    TCHECK(s.apply(off, err).is_ok());
    TCHECK(err.empty());

    UsbConfig on; on.enabled = true;
    TCHECK(!s.apply(on, err).is_ok());      // asking for it is still an error
    TCHECK(s.capabilities().host_supported == false);
    TCHECK(s.status().devices.empty());
}

void test_mode_names_round_trip()
{
    for (UsbPowerMode m : {UsbPowerMode::BoardDefault, UsbPowerMode::Gpio,
                           UsbPowerMode::AlwaysOn, UsbPowerMode::None}) {
        UsbPowerMode back;
        TCHECK(usb_power_mode_parse(usb_power_mode_name(m), back));
        TCHECK(back == m);
    }
    UsbPowerMode junk;
    TCHECK(!usb_power_mode_parse("", junk));
    TCHECK(!usb_power_mode_parse("on", junk));
}

void test_unknown_power_state_is_reported_as_unknown()
{
    FakeBackend b; b.caps = switchable_board(); b.power_readable = false;
    UsbHostService s(b);
    UsbStatus st = s.status();
    TCHECK(!st.power_known);
}

} // namespace

void run_usb_tests()
{
    test_board_default_resolves_to_the_wired_pin();
    test_disable_turns_the_port_off();
    test_unlisted_pin_is_refused_without_expert();
    test_expert_allows_an_unlisted_pin();
    test_explicit_pin_keeps_its_own_polarity();
    test_always_on_is_refused_on_a_switchable_board();
    test_always_on_board_needs_no_pin();
    test_switching_to_none_reaches_the_backend();
    test_gpio_mode_on_a_board_that_cannot_switch();
    test_board_without_usb_host();
    test_backend_refusal_leaves_state_unchanged();
    test_enable_at_boot_is_honoured();
    test_devices_are_passed_through_untouched();
    test_null_backend_accepts_being_switched_off();
    test_mode_names_round_trip();
    test_unknown_power_state_is_reported_as_unknown();
}
