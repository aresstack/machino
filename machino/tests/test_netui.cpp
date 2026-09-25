// The built-in network/USB page.
//
// A page cannot be rendered on the host, so what is checked here is the part
// that silently rots: whether the page and the API still agree. Every endpoint
// and every field name the page uses is asserted to be the one the API
// actually produces -- those strings are built next door in net_views.cpp, so
// a rename there breaks this test instead of breaking the page in a browser
// nobody opens until the camera is in a mast.
//
// The visual result is PENDING_BROWSER and says so in the release notes.
#include "app/api/net_views.hpp"
#include "app/http/netui.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::api;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

std::string page() { return std::string(http::machino_net_page(), http::machino_net_page_len()); }

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

size_t count(const std::string& hay, const std::string& needle)
{
    size_t n = 0, p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
    return n;
}

void test_the_page_is_self_contained()
{
    const std::string p = page();
    TCHECK(p.size() > 4000);
    TCHECK(has(p, "<!DOCTYPE html>"));

    // A camera serving its own access point has no internet. A network page
    // that needs a CDN to render is useless exactly when it is needed.
    TCHECK(!has(p, "http://"));
    TCHECK(!has(p, "https://"));
    // Genau EIN externes Skript, und zwar unser eigenes auf derselben
    // Herkunft: /machino/chrome.js holt die Kopfleiste der Kamera-WebUI und
    // tut nichts, wenn das nicht geht. Alles andere bleibt verboten -- der
    // Grund oben (kein CDN) gilt unveraendert.
    TCHECK(count(p, "<script src") == 1);
    TCHECK(has(p, "<script src=\"/machino/chrome.js\""));
    // Das Seitenskript ist eine IIFE. Nicht Stil: die Kopfleiste laedt
    // /a/main.js nach, das global `function $` deklariert -- ein globales
    // `const $` hier liess main.js beim Parsen sterben, und mit ihm jedes
    // Dropdown der Leiste (gemessen 2026-09-25).
    TCHECK(has(p, "(function () {"));
    TCHECK(!has(p, "<link rel=\"stylesheet\""));
}

void test_every_endpoint_the_page_calls_exists()
{
    const std::string p = page();
    TCHECK(has(p, "/machino/chrome"));      // die eine Einstellung der Kopfleiste
    for (const char* ep : {"/api/v1/network",
                           "/api/v1/network/policy",
                           "/api/v1/network/wifi",
                           "/api/v1/network/wifi/scan",
                           "/api/v1/network/wifi/station",
                           "/api/v1/network/wifi/ap",
                           "/api/v1/network/change/",
                           "/api/v1/usb",
                           "/api/v1/usb/devices"}) {
        TCHECK(has(p, ep));
    }
    // The confirm URL is assembled, not hardcoded, so the shape is checked.
    TCHECK(has(p, "\"/api/v1/network/change/\" + TOKEN + \"/confirm\""));
}

void test_the_field_names_match_what_the_api_produces()
{
    // Built from the real builders, not typed out again here: a rename in
    // net_views.cpp then fails this test rather than the page.
    net::UplinkStatus u;
    u.id = "wlan0"; u.type = net::UplinkType::Wifi; u.state = net::LinkState::Connected;
    u.internet = true; u.active = true;
    u.info.ifname = "wlan0"; u.info.ipv4 = "192.168.1.9";
    const std::string uplink = uplink_status_json(u).dump();

    net::UplinkPolicy pol;
    const std::string policy = policy_json(pol).dump();

    usb::UsbConfig uc;
    const std::string usbcfg = usb_config_json(uc).dump();

    const std::string p = page();

    // Each of these appears in the API document AND in the page.
    for (const char* key : {"\"internet\"", "\"active\"", "\"interface\"", "\"ipv4\"",
                            "\"netmask\"", "\"gateway\"", "\"dns\""}) {
        const std::string k(key);
        const std::string bare = k.substr(1, k.size() - 2);
        TCHECK(has(uplink, k));
        TCHECK(has(p, bare));
    }
    for (const char* key : {"\"order\"", "\"autoFailover\"", "\"returnToPreferred\"",
                            "\"pinned\"", "\"pinnedUplink\""}) {
        const std::string k(key);
        TCHECK(has(policy, k));
        TCHECK(has(p, k.substr(1, k.size() - 2)));
    }
    // The USB config is NESTED under "power" and camelCase. The first version
    // of this page used flat snake_case names; every PATCH from it would have
    // come back 422 unknown_field, and every read would have shown the
    // defaults. This test is why that never reached a camera.
    for (const char* key : {"\"enabled\"", "\"enableAtBoot\"", "\"expert\"",
                            "\"activeLevel\"", "\"pin\"", "\"power\""}) {
        const std::string k(key);
        TCHECK(has(usbcfg, k));
        TCHECK(has(p, k.substr(1, k.size() - 2)));
    }
    TCHECK(!has(p, "enable_at_boot"));
    TCHECK(!has(p, "active_level"));

    usb::UsbStatus us;
    const std::string usbst = usb_status_json(us).dump();
    for (const char* key : {"\"hostActive\"", "\"hostSupported\"", "\"maxSpeed\"",
                            "\"switchable\"", "\"voltageMv\"", "\"allowedPins\"",
                            "\"drivesPower\""}) {
        const std::string k(key);
        TCHECK(has(usbst, k));
    }
    for (const char* used : {"hostActive", "hostSupported", "maxSpeed",
                             "switchable", "voltageMv", "allowedPins"}) {
        TCHECK(has(p, used));
    }

    // Metrics are nested too.
    TCHECK(has(uplink, "\"metrics\""));
    for (const char* key : {"linkMbit", "rxBytes", "txBytes", "rssiDbm"}) {
        TCHECK(has(uplink, std::string("\"") + key + "\""));
        TCHECK(has(p, key));
    }
    TCHECK(has(uplink, "\"activeUplink\"") == false);   // that one is on the envelope
    TCHECK(has(network_json({}, pol, "eth0").dump(), "\"activeUplink\""));
    TCHECK(has(p, "activeUplink"));
}

void test_the_two_endpoints_that_answer_a_bare_array()
{
    // usb_devices_json and wifi_scan_json return the ARRAY, not an object
    // wrapping one. The page read .devices and .networks off them at first,
    // which silently showed "nothing connected" and "no networks found"
    // forever -- the most convincing possible way to look broken.
    TCHECK(usb_devices_json({}).is_array());
    TCHECK(wifi_scan_json({}).is_array());

    const std::string p = page();
    TCHECK(has(p, "Array.isArray(dres.body)"));
    TCHECK(has(p, "Array.isArray(res.body)"));
    TCHECK(!has(p, "dres.body.devices"));
    TCHECK(!has(p, "res.body.networks"));
}

void test_the_capability_names_match()
{
    net::WifiCapabilities c;
    c.present = true;
    c.ap_unavailable_reason = "hostapd is not in this image";
    const std::string caps = wifi_capabilities_json(c).dump();
    const std::string p = page();

    // The page greys controls out by capability and shows the reason. If
    // either name drifts it silently shows every control as available.
    TCHECK(has(caps, "\"station\""));
    TCHECK(has(caps, "\"accessPoint\""));
    TCHECK(has(caps, "\"scan\""));
    TCHECK(has(caps, "\"apUnavailableReason\""));
    TCHECK(has(p, "usable.station"));
    TCHECK(has(p, "usable.accessPoint"));
    TCHECK(has(p, "usable.scan"));
    TCHECK(has(p, "apUnavailableReason"));

    // AP support is three-valued, and the page must not flatten it back: an
    // attemptable-but-unverified radio gets the form AND a note saying nobody
    // asked the driver.
    TCHECK(has(caps, "\"accessPointVerified\""));
    TCHECK(has(caps, "\"accessPointKnown\""));
    TCHECK(has(p, "usable.accessPointVerified"));
    TCHECK(has(p, "ap-unverified"));
}

void test_the_staged_change_fields_match()
{
    const std::string p = page();
    // 202 plus token plus window is the contract net_api answers a staged
    // change with; the countdown is driven off exactly these.
    TCHECK(has(p, "status === 202"));
    TCHECK(has(p, "confirm_within_ms"));
    TCHECK(has(p, "remaining_ms"));
    TCHECK(has(p, "res.body.token"));
    // Reaching zero must NOT be treated as "it was rolled back": only the
    // camera knows, and it may be unreachable.
    TCHECK(has(p, "refresh()"));
}

void test_no_passphrase_is_ever_put_back_into_a_field()
{
    // The API never returns one; the page must not keep one either.
    const std::string p = page();
    TCHECK(has(p, "$(\"psk\").value = \"\""));
    TCHECK(has(p, "$(\"appsk\").value = \"\""));
    // And nothing reads a passphrase back out of a response.
    TCHECK(!has(p, ".passphrase;"));
    TCHECK(!has(p, "value = w.passphrase"));
    TCHECK(!has(p, "body.passphrase"));
}

void test_a_401_always_goes_to_the_login_page()
{
    // One place, not scattered: a UI that handles this in some paths and not
    // others shows stale data to a signed-out user.
    const std::string p = page();
    TCHECK(has(p, "r.status === 401"));
    TCHECK(has(p, "/login.html?next="));
}

void test_all_the_sections_the_navigation_promises_exist()
{
    const std::string p = page();
    for (const char* id : {"t-overview", "t-ethernet", "t-wifi", "t-cellular",
                           "t-routing", "t-usbhost", "t-usbpower", "t-usbmode", "t-usbdev"}) {
        // Once in the tab table, once as the section.
        TCHECK(has(p, std::string("\"") + id + "\""));
        TCHECK(has(p, std::string("id=\"") + id + "\""));
    }
}

void test_nothing_here_touches_the_media_path()
{
    // USB and connectivity must not be able to reach the pipeline, and the
    // page is the easiest place for that to creep back in as a convenience.
    const std::string p = page();
    TCHECK(!has(p, "/api/v1/config"));
    TCHECK(!has(p, "/api/v1/state"));
    TCHECK(!has(p, "/api/v1/snapshot"));
    TCHECK(!has(p, "/ws/video"));
}

} // namespace

void run_netui_tests()
{
    test_the_page_is_self_contained();
    test_every_endpoint_the_page_calls_exists();
    test_the_field_names_match_what_the_api_produces();
    test_the_two_endpoints_that_answer_a_bare_array();
    test_the_capability_names_match();
    test_the_staged_change_fields_match();
    test_no_passphrase_is_ever_put_back_into_a_field();
    test_a_401_always_goes_to_the_login_page();
    test_all_the_sections_the_navigation_promises_exist();
    test_nothing_here_touches_the_media_path();
}
