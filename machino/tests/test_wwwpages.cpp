// Machinos WebUI-Seiten sind seit dem Umbau vom 2026-09-25 ECHTE
// OpenIPC-Seiten: haserl-CGIs in openipc/www/, installiert in
// /var/www/cgi-bin/, gerendert von derselben Pipeline wie network.cgi.
//
// Vorher gab es zwei Anlaeufe, die beide im Browser gescheitert sind: eine
// nachgebaute Kopfleiste (Fremdkoerper) und eine clientseitig aus einer
// geholten Seite uebernommene (Seite springt, relative Links laufen unter
// /machino/ ins Leere, main.js stirbt am globalen $-Konflikt). Diese Tests
// halten fest, was die Seiten zu OpenIPC-Seiten macht -- damit der dritte
// Anlauf der letzte war.
//
// Die Seiten liegen als DATEIEN im Repo (das Bundle kopiert sie), nicht als
// Strings im Binary: das Binary liefert sie nicht mehr aus, es leitet die
// alten /machino/-Pfade nur noch dorthin um.
extern int g_fail_ext, g_pass_ext;
#define TCHECK(c) do { if (c) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#include <cstdio>
#include <string>

namespace {

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

std::string slurp(const char* rel)
{
    // Vom Repo-Wurzelverzeichnis oder aus machino/ heraus gestartet -- beide
    // Arbeitsverzeichnisse kommen in CI und lokal vor.
    for (const std::string base : {std::string(""), std::string("machino/")}) {
        FILE* f = std::fopen((base + rel).c_str(), "rb");
        if (!f) continue;
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    }
    return std::string();
}

void check_page(const char* rel, const char* title, const char* login_next)
{
    const std::string p = slurp(rel);
    TCHECK(!p.empty());

    // Das OpenIPC-Seitenmuster, wortwoertlich das von wireguard.cgi: haserl,
    // common/header/footer-Includes. DARAUS kommen Head, Navbar, Theme und
    // main.js im ersten HTML -- nichts wird nachgeladen oder nachgebaut.
    TCHECK(p.compare(0, 18, "#!/usr/bin/haserl\n") == 0);
    TCHECK(has(p, "<%in p/common.cgi %>"));
    TCHECK(has(p, "<%in p/header.cgi %>"));
    TCHECK(has(p, "<%in p/footer.cgi %>"));
    TCHECK(has(p, std::string("page_title=\"") + title + "\""));

    // KEIN eigenes Dokument: html/head/body liefert header.cgi. Eine Seite,
    // die ihr eigenes <html> mitbringt, ist wieder eine zweite WebUI.
    TCHECK(!has(p, "<html"));
    TCHECK(!has(p, "<head>"));
    TCHECK(!has(p, "<body"));
    TCHECK(!has(p, "<!DOCTYPE"));

    // Keine Reste der zwei verworfenen Anlaeufe.
    TCHECK(!has(p, "chrome.js"));
    TCHECK(!has(p, "id=\"tabs\""));
    TCHECK(!has(p, "buildTabs"));

    // Eigenes CSS nur GESCOPED (#mch ...): .row, .card usw. gehoeren auf
    // dieser Seite Bootstrap. Ein ungescoptes .row{display:flex;gap:10px}
    // hat frueher das Grid der umgebenden Seite zerlegt.
    TCHECK(!has(p, "\nbody{"));
    TCHECK(!has(p, "\n.row{"));
    TCHECK(!has(p, "\n.card{"));

    // Das Seitenskript ist eine IIFE: header.cgi laedt /a/main.js, und das
    // deklariert global `function $`. Ein globales `const $` hier liess
    // main.js beim Parsen sterben (gemessen 2026-09-25).
    TCHECK(has(p, "(function () {"));

    // Nach 401 zur Anmeldung und DANACH ZURUECK AUF DIESE SEITE.
    TCHECK(has(p, std::string("/login.html?next=") + login_next));

    // Nur absolute API-Pfade: die Seite liegt unter /cgi-bin/, ihre Daten
    // kommen von Machino auf Port 80 -- ein relativer Pfad ginge an busybox.
    TCHECK(has(p, "\"/api/v1/"));
    TCHECK(!has(p, "\"api/v1/"));
}

} // namespace

void run_wwwpages_tests()
{
    std::printf("== WebUI: Machinos OpenIPC-Seiten ==\n");
    check_page("openipc/www/machino-network.cgi", "Network & USB",
               "/cgi-bin/machino-network.cgi");
    check_page("openipc/www/machino-devices.cgi", "Device Manager",
               "/cgi-bin/machino-devices.cgi");

    // Die Netzwerkseite behaelt ihre Funktionsflaeche: jede dieser Routen
    // existiert in net_api, und die Seite muss sie weiter ansprechen --
    // der Umbau war UI-Architektur, kein Funktionsabbau.
    const std::string net = slurp("openipc/www/machino-network.cgi");
    for (const char* ep : {"/api/v1/network", "/api/v1/network/wifi",
                           "/api/v1/network/wifi/scan", "/api/v1/network/wifi/station",
                           "/api/v1/network/wifi/ap", "/api/v1/network/cellular",
                           "/api/v1/network/policy", "/api/v1/usb", "/api/v1/usb/devices"})
        TCHECK(has(net, ep));
    const std::string dev = slurp("openipc/www/machino-devices.cgi");
    TCHECK(has(dev, "/api/v1/devices"));
}
