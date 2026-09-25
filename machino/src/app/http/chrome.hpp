// Die Kopfleiste der Kamera-WebUI auf Machinos EIGENEN Seiten.
//
// Das Problem: der Menueeintrag "Netzwerk & USB" steht im Menue der Stock-WebUI,
// fuehrt aber auf /machino/net -- eine Seite, die Machino selbst ausliefert und
// die deren Kopfleiste nicht kennt. Wer daraufklickt, verliert das Menue und
// kommt nur ueber den Zurueck-Knopf wieder heraus. Als Integration zu wenig.
//
// WIE: die UI der Kamera wird BENUTZT, nicht nachgebaut. Alles liegt auf
// derselben Herkunft, also kann die Seite es einfach laden:
//
//     /a/bootstrap.min.css        ihr Stylesheet
//     /a/bootstrap.override.css   ihre Anpassungen (Farben, Dark/Light)
//     /a/main.js                  ihr Verhalten
//     <nav class="navbar">        ihr Markup, unveraendert uebernommen
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

#include <cstddef>

namespace machino {
namespace http {

const char* machino_chrome_js();
std::size_t machino_chrome_js_len();

} // namespace http
} // namespace machino
