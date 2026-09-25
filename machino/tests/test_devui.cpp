// Die eingebaute Geraeteseite.
//
// Gleiche Begruendung wie test_netui.cpp: rendern laesst sich die Seite auf dem
// Host nicht, aber das, was still verrottet, laesst sich pruefen -- ob Seite
// und API noch dieselben Namen benutzen. Die Zustandsnamen sind hier der
// heikle Teil: `install_state_name()` liefert "not-installed", die Aufzaehlung
// heisst `NotInstalled`, und eine Seite, die nach dem Enum-Namen sucht, faellt
// stumm in ihren Default-Zweig. Genau das ist beim ersten Wurf passiert.
//
// Das Aussehen bleibt PENDING_BROWSER (H19 in docs/pending-physical.md).
#include "app/http/devui.hpp"
#include "core/devices/device_package.hpp"

#include <cstdio>
#include <string>

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

std::string page() { return std::string(http::machino_devices_page(), http::machino_devices_page_len()); }

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

void test_the_page_is_self_contained()
{
    const std::string p = page();
    TCHECK(p.size() > 2000);
    TCHECK(has(p, "<!DOCTYPE html>"));

    // Eine Kamera am eigenen Access Point hat kein Internet. Die Geraeteseite
    // wird genau dann gebraucht, wenn noch gar keine Verbindung steht.
    TCHECK(!has(p, "http://"));
    TCHECK(!has(p, "https://"));
    TCHECK(!has(p, "<script src"));
    TCHECK(!has(p, "<link rel=\"stylesheet\""));
}

void test_every_endpoint_the_page_calls_exists()
{
    const std::string p = page();
    TCHECK(has(p, "/api/v1/devices"));
    // install/uninstall werden zusammengesetzt, also wird die Form geprueft.
    TCHECK(has(p, "\"/api/v1/devices/\" + encodeURIComponent(id) + \"/\" + verb"));
    TCHECK(has(p, "\"install\""));
    TCHECK(has(p, "\"uninstall\""));
    TCHECK(has(p, "method: \"POST\""));
}

void test_the_field_names_match_what_the_api_produces()
{
    const std::string p = page();
    // Diese Namen baut net_api.cpp in devices_get(). Wird dort einer
    // umbenannt, zeigt die Seite stumm "undefined".
    for (const char* f : {"devices", "id", "title", "driver", "state",
                          "openipcRegistered", "hardwarePresent",
                          "driverLoaded", "active", "detail"}) {
        TCHECK(has(p, f));
    }
}

void test_the_state_names_match_install_state_name()
{
    const std::string p = page();
    // Der eigentliche Punkt dieses Tests. Jeder Zustand, den die API
    // herausgeben kann, muss auf der Seite einen Eintrag haben -- und zwar
    // unter GENAU der Zeichenkette, die install_state_name() liefert.
    const devices::InstallState all[] = {
        devices::InstallState::Unsupported,
        devices::InstallState::Unavailable,
        devices::InstallState::NotInstalled,
        devices::InstallState::InstallPending,
        devices::InstallState::Installed,
        devices::InstallState::RemovePending,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const std::string key = std::string("\"") + devices::install_state_name(all[i]) + "\":";
        TCHECK(has(p, key));
    }

    // Und die Knoepfe haengen an denselben Zeichenketten, nicht an den
    // Enum-Namen.
    TCHECK(has(p, "d.state === \"not-installed\""));
    TCHECK(has(p, "d.state === \"installed\""));
    TCHECK(has(p, "d.state === \"install-pending\""));
    TCHECK(has(p, "d.state === \"remove-pending\""));
    TCHECK(!has(p, "NotInstalled"));
    TCHECK(!has(p, "InstallPending"));
}

void test_the_page_says_what_it_does_not_do()
{
    const std::string p = page();
    // Ohne diesen Hinweis sieht die Seite aus, als konfiguriere man hier WLAN.
    // Das kann sie nicht, und genau diese Verwechslung hat den Befund vom
    // 2026-09-25 so lange am Leben gehalten.
    TCHECK(has(p, "/etc/wireless/usb"));
    TCHECK(has(p, "/lib/modules"));
    TCHECK(has(p, "Wireless"));
    TCHECK(has(p, "/machino/net"));      // wer den Port haelt, steht dort
    // Und: dass Deinstallieren die Nutzlast liegen laesst.
    TCHECK(has(p, "ohne neues Paket"));
}

void test_nothing_here_touches_the_media_path()
{
    const std::string p = page();
    TCHECK(!has(p, "/api/v1/config"));
    TCHECK(!has(p, "/api/v1/snapshot"));
    TCHECK(!has(p, "/ws/video"));
}

} // namespace

void run_devui_tests()
{
    test_the_page_is_self_contained();
    test_every_endpoint_the_page_calls_exists();
    test_the_field_names_match_what_the_api_produces();
    test_the_state_names_match_install_state_name();
    test_the_page_says_what_it_does_not_do();
    test_nothing_here_touches_the_media_path();
}
