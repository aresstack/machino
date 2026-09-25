// Die Kopfleiste der Kamera-WebUI auf Machinos EIGENEN Seiten.
//
// Das Problem: der Menueeintrag "Netzwerk & USB" steht im Menue der Stock-WebUI,
// fuehrt aber auf /machino/net -- eine Seite, die Machino selbst ausliefert und
// die deren Kopfleiste nicht kennt. Wer daraufklickt, verliert das Menue und
// kommt nur ueber den Zurueck-Knopf wieder heraus. Als Integration zu wenig.
//
// Die gemessenen Fakten zu dieser WebUI -- Bootstrap-Version, welche Dateien
// es gibt, was main.js kann und was nicht -- stehen in
// docs/openipc-webui-assets.md. Wer hier etwas aendert, liest das zuerst.
//
// WIE: die UI der Kamera wird BENUTZT, nicht nachgebaut. Alles liegt auf
// derselben Herkunft, also kann die Seite es einfach laden -- Stylesheets und
// Verhalten aus dem <head> der geholten Seite, das <nav> unveraendert.
//
// WAS KONFIGURIERBAR IST, UND WARUM NUR DAS: api.chrome_source, also WELCHE
// Seite geholt wird (Vorgabe /cgi-bin/live.cgi, leer = Uebernahme aus). Die
// Dateipfade dagegen werden NICHT konfiguriert und auch nicht geraten: sie
// stehen im <head> genau dieser Seite. Auf dieser Kamera ist das
// /a/bootstrap.min.css, /a/bootstrap.override.css und /a/main.js -- auf einer
// anderen Variante etwas anderes, und dann stimmt es trotzdem, ohne dass
// jemand etwas einstellen muss. Eine Einstellung, die man sich ablesen kann,
// ist eine Einstellung zu viel.
//
// Ein Bootstrap-JavaScript gibt es auf dieser Kamera NICHT (geprueft:
// /a/bootstrap.bundle.min.js, bootstrap.min.js, bootstrap.js -> alle 404).
// Dropdowns, Collapse, das Markieren des aktiven Eintrags und der
// Abmelden-Knopf stecken in main.js. Wer die Leiste ohne main.js will, muesste
// dieses Verhalten nachschreiben -- ein erster Anlauf hier hat genau das getan
// und war zu Recht als Fremdkoerper erkennbar.
//
// Das <nav> wird VERBATIM uebernommen statt nachgebaut, weil sein Inhalt von
// der Firmware abhaengt (die Stock-Seite rendert Eintraege bedingt). Eine Kopie
// im Machino-Quelltext waere beim naechsten OpenIPC-Update falsch.
//
// WARUM eine eigene Datei statt Inline-Skript: beide Machino-Seiten brauchen
// dasselbe, zwei Kopien in zwei Raw-Strings driften auseinander, und der
// Browser kann eine eigene Datei zwischenspeichern.
//
// Schlaegt etwas davon fehl -- keine Sitzung, Seite nicht erreichbar, kein nav
// im Dokument -- passiert NICHTS: die Seite bleibt die eigenstaendige, die sie
// ohnehin ist. Eine Kopfleiste ist Komfort und darf keine Seite kosten.
#pragma once

#include <string>

namespace machino {
namespace http {

// `source` ist api.chrome_source. Leer -> das Skript tut nichts.
std::string machino_chrome_js(const std::string& source);

} // namespace http
} // namespace machino
