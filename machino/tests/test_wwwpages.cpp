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
    for (const std::string& base : {std::string(""), std::string("machino/")}) {
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

    // Die vier Reviewbefunde vom 2026-09-25, je ein Stolperdraht:
    // 1. header.cgi oeffnet <main>/<div class=container> bereits -- ein
    //    eigenes <main> verschachtelt die Seite falsch.
    TCHECK(!has(p, "<main"));
    // 2. Keine eigene Designsprache: die Komponenten sind die der Kamera-UI.
    TCHECK(!has(p, "class=\"act\""));
    TCHECK(!has(p, "class=\"pill"));
    TCHECK(!has(p, "class=\"note\""));
    TCHECK(has(p, "mj-cap"));
    TCHECK(has(p, "mj-card-note"));
    TCHECK(has(p, "row g-4"));
    // 3. Sichtbarer Text ENGLISCH wie der Rest der WebUI. Umlaute duerfen nur
    //    in Kommentaren stehen; die Pruefung ist grob, aber jede der vier
    //    Ketten kam im deutschen Bestand vor.
    TCHECK(!has(p, "&auml;"));
    TCHECK(!has(p, "&uuml;"));
    TCHECK(!has(p, "Bestätigung"));
}

} // namespace

void run_wwwpages_tests()
{
    std::printf("== WebUI: Machinos OpenIPC-Seiten ==\n");
    // Vier Seiten seit der Zerlegung vom 2026-09-26: die eine
    // "Network & USB"-Monsterseite (15 Karten, und ein zweites "Network"
    // neben OpenIPCs eigenem Menuepunkt) ist in Seiten je Thema zerlegt.
    // KEINE Wi-Fi-Seite: Station-WLAN konfiguriert OpenIPCs eigene
    // Netzwerkseite (Architekturkorrektur 2026-09-25, Doppelbesitzer von
    // wlan0); der AP-Modus bekommt spaeter eine eigene schmale Flaeche.
    check_page("openipc/www/machino-usb.cgi", "USB",
               "/cgi-bin/machino-usb.cgi");
    check_page("openipc/www/machino-cellular.cgi", "Cellular",
               "/cgi-bin/machino-cellular.cgi");
    check_page("openipc/www/machino-uplinks.cgi", "Uplinks",
               "/cgi-bin/machino-uplinks.cgi");
    check_page("openipc/www/machino-devices.cgi", "Device Manager",
               "/cgi-bin/machino-devices.cgi");
    // AP8: die native IPsec-Seite -- gleiche OpenIPC-Vertraege wie die anderen.
    check_page("openipc/www/machino-ipsec.cgi", "IPsec",
               "/cgi-bin/machino-ipsec.cgi");

    // Die Funktionsflaeche ist vollstaendig auf die Seiten verteilt -- die
    // Zerlegung war UI-Architektur, kein Funktionsabbau.
    const std::string usb  = slurp("openipc/www/machino-usb.cgi");
    const std::string cell = slurp("openipc/www/machino-cellular.cgi");
    const std::string up   = slurp("openipc/www/machino-uplinks.cgi");
    TCHECK(has(usb,  "/api/v1/usb"));
    TCHECK(has(usb,  "/api/v1/usb/devices"));
    TCHECK(has(cell, "/api/v1/network/cellular"));
    TCHECK(has(cell, "/api/v1/network/cellular/presets"));
    TCHECK(has(up,   "/api/v1/network"));
    TCHECK(has(up,   "/api/v1/network/policy"));
    const std::string dev = slurp("openipc/www/machino-devices.cgi");
    TCHECK(has(dev, "/api/v1/devices"));

    // AP8: die IPsec-Seite spricht genau die vorhandenen /api/v1/ipsec*-Routen,
    // der PSK ist write-only (Passwortfeld, "stored"-Anzeige, nie Rueckgabe),
    // und Configured/Negotiated/Installed werden getrennt gezeigt.
    const std::string ips = slurp("openipc/www/machino-ipsec.cgi");
    TCHECK(has(ips, "/api/v1/ipsec"));
    TCHECK(has(ips, "/api/v1/ipsec/config"));
    TCHECK(has(ips, "/api/v1/ipsec/connect"));
    TCHECK(has(ips, "/api/v1/ipsec/disconnect"));
    TCHECK(has(ips, "/api/v1/ipsec/status"));
    TCHECK(has(ips, "type=\"password\""));          // PSK-Feld maskiert
    TCHECK(has(ips, "Negotiated TSr"));             // negotiated != configured
    TCHECK(has(ips, "Installed route"));            // installed getrennt
    TCHECK(has(ips, "machino-cellular.cgi"));       // Cellular-Link, keine 2. Modemconfig
    // AP9: EAP-MSCHAPv2 + Trust-Modell in der UI.
    TCHECK(has(ips, "eap-mschapv2"));
    TCHECK(has(ips, "eapPassword"));                // write-only wie der PSK
    TCHECK(has(ips, "trustMode"));
    TCHECK(has(ips, "caPem"));
    TCHECK(has(ips, "hostStoreAvailable"));         // HOST_STORE-Ausgrauen
    TCHECK(has(ips, "No CA validation"));           // NONE nur explizit

    // Cellular haengt am usb.mode; seit der Zerlegung LIEST die Seite ihn
    // selbst, statt eine Variable einer anderen Seite zu erwarten.
    TCHECK(has(cell, "fetchMode"));
    // Eine anderswo angestossene Bestaetigungsfrist muss ueberall sichtbar
    // sein, wo sie zurueckrollen kann.
    TCHECK(has(cell, "loadPending"));

    // Station-WLAN gehoert OpenIPCs Netzwerkseite; die USB-Seite muss den
    // Weg dorthin NENNEN, sonst sucht jeder die SSID wieder bei machino.
    TCHECK(has(usb, "Wireless adapter"));
    TCHECK(has(usb, "network.cgi"));

    // Der Portstrom ist auf diesem Board 3,3 V ueber einen Transistor an
    // einem GPIO -- ohne ihn enumeriert nichts. Die Karte muss das SAGEN,
    // sonst wirkt eine gesteckte Zusatzplatine wie fehlende Hardware
    // (gemessen am 2026-09-26).
    TCHECK(has(usb, "3.3"));
    TCHECK(has(usb, "transistor"));
    TCHECK(has(usb, "Power the port"));
}
