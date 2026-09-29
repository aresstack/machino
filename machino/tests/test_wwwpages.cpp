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
    // Die KI-Seite: seit sie machinos API spricht (Detectors-Karte, "Use"
    // neben dem Modell), gelten dieselben Vertraege wie fuer die anderen.
    check_page("openipc/www/machino-ai.cgi", "AI",
               "/cgi-bin/machino-ai.cgi");
    const std::string aip = slurp("openipc/www/machino-ai.cgi");
    TCHECK(has(aip, "/api/v1/ai/detectors"));
    TCHECK(has(aip, "\"/api/v1/config\""));
    TCHECK(has(aip, "model_path"));
    TCHECK(has(aip, "machino-ai-upload.cgi"));
    // Die SDK-Anleitung steht AUF der Seite, mit dem Toolkit-Pin der CI.
    TCHECK(has(aip, "magik-transform-tools"));
    TCHECK(has(aip, "e511d370dd7ff84664c9140e0590c354947c7eac"));

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

    // W5b: die Storage-/Aufraeum-Seite. Form-basiert wie machino-ai.cgi (kein
    // fetch/main.js), also NICHT in check_page -- aber die sicherheitskritische
    // Allowlist und die majestic-Haltung werden hier festgenagelt.
    const std::string cl = slurp("openipc/www/machino-cleanup.cgi");
    const std::string dl = slurp("openipc/www/machino-cleanup-dl.cgi");
    TCHECK(!cl.empty());
    TCHECK(!dl.empty());
    // Das haserl-Seitenmuster (wie ai.cgi): Includes + Titel, kein eigenes Doc.
    TCHECK(cl.compare(0, 18, "#!/usr/bin/haserl\n") == 0);
    TCHECK(has(cl, "<%in p/common.cgi %>"));
    TCHECK(has(cl, "<%in p/header.cgi %>"));
    TCHECK(has(cl, "<%in p/footer.cgi %>"));
    TCHECK(has(cl, "page_title=\"Storage\""));
    TCHECK(!has(cl, "<html"));
    TCHECK(!has(cl, "<!DOCTYPE"));
    TCHECK(has(cl, "mj-card-note"));
    TCHECK(has(cl, "row g-4"));
    // Sichtbarer Text englisch (Umlaute nur in Kommentaren).
    TCHECK(!has(cl, "&auml;"));
    TCHECK(!has(cl, "&uuml;"));

    // Sicherheitskern: BEIDE Skripte prueften jeden Pfad mit is_reclaimable,
    // lehnen ".." ab und lassen nur die bekannten Fundstellen zu. Das ist der
    // Riegel, der einen Pfad aus dem Netz von /etc/shadow o.ae. fernhaelt.
    for (const std::string* s : {&cl, &dl}) {
        TCHECK(has(*s, "is_reclaimable"));
        TCHECK(has(*s, "*..*"));                              // Traversal-Riegel
        TCHECK(has(*s, "[ -f \"$_p\" ] || return 1"));       // nur regulaere Dateien
        TCHECK(has(*s, "/etc/machino/backup/*"));
        TCHECK(has(*s, "/usr/bin/machino.old.*"));
        TCHECK(has(*s, "/etc/machino/models/*"));
    }
    // Der Download-Helfer schreibt seine EIGENE HTTP-Antwort (wie cgi-run) und
    // liefert als attachment aus -- sonst kann eine HTML-Seite keine Binaerdatei.
    TCHECK(dl.compare(0, 9, "#!/bin/sh") == 0);              // plain sh, kein haserl
    TCHECK(has(dl, "#!/bin/sh"));
    TCHECK(has(dl, "Content-Disposition: attachment"));
    TCHECK(has(dl, "HTTP/1.1 200 OK"));

    // majestic bekommt KEINEN Loeschknopf (read-only /rom, 0 KB) -- nur einen
    // Knopf, der die Deaktivierung (Whiteout) sicherstellt.
    TCHECK(has(cl, "ensure-majestic-off"));
    TCHECK(!has(cl, "del-majestic"));
    TCHECK(has(cl, "read-only"));
    // Die eigentlichen Aktionen der Seite.
    TCHECK(has(cl, "del-file"));
    TCHECK(has(cl, "machino-cleanup-dl.cgi?path="));

    // W5c: der Modell-Upload auf der KI-Seite. Die KI-Seite bekommt einen
    // Upload-Knopf (fetch), das Upload-CGL (plain sh) prueft das Bundle streng,
    // BEVOR etwas nach /etc/machino/models geht.
    const std::string ai   = slurp("openipc/www/machino-ai.cgi");
    const std::string aiup = slurp("openipc/www/machino-ai-upload.cgi");
    TCHECK(!ai.empty());
    TCHECK(!aiup.empty());
    // KI-Seite: Upload-Feld + fetch auf das Upload-CGI.
    TCHECK(has(ai, "machino-ai-upload.cgi"));
    TCHECK(has(ai, "type=\"file\""));
    TCHECK(has(ai, "fetch("));
    // Upload-CGI: plain sh, eigene HTTP-Antwort, und die Sicherheits-Gates.
    TCHECK(aiup.compare(0, 9, "#!/bin/sh") == 0);
    TCHECK(has(aiup, "HTTP/1.1"));
    TCHECK(has(aiup, "REQUEST_METHOD"));
    // Traversal-Riegel vor dem Extrahieren (tar-Ausbruch verhindern).
    TCHECK(has(aiup, "\\.\\."));
    TCHECK(has(aiup, "tar t"));
    // Symlink-Riegel: nur regulaere Dateien (kein cp eines Symlink-Ziels).
    TCHECK(has(aiup, "-type f -name"));
    // Manifest-Kompatibilitaet: nur venus-nna / nna1 wird installiert.
    TCHECK(has(aiup, "venus-nna"));
    TCHECK(has(aiup, "nna1"));
    // Zielort + Groessen-/Platz-Grenzen.
    TCHECK(has(aiup, "/etc/machino/models"));
    TCHECK(has(aiup, "CONTENT_LENGTH"));
    TCHECK(has(aiup, "Insufficient Storage"));
}
