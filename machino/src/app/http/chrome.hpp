// Die Kopfleiste der Kamera-WebUI auf Machinos EIGENEN Seiten.
//
// Das Problem, das es loest: der Menueeintrag "Netzwerk & USB" steht im Menue
// der Stock-WebUI, aber er fuehrt auf /machino/net -- eine Seite, die Machino
// selbst ausliefert und die deren Kopfleiste nicht kennt. Wer daraufklickt,
// verliert das Menue und kommt nur ueber den Zurueck-Knopf wieder heraus. Als
// Integration ist das zu wenig.
//
// WIE: die Seite holt beim Laden EINE Seite der Stock-WebUI (gleiche Herkunft,
// dieselbe Sitzung) und liest daraus nur die LINKS -- Beschriftung und Ziel.
// Gezeichnet wird die Leiste dann in Machinos eigenem Stil.
//
// WARUM nicht deren Markup samt CSS uebernehmen: zwei vollstaendige
// Stylesheets auf einer Seite streiten sich um body, nav und die Farben, und
// deren Dropdowns brauchen zusaetzlich ihr Bootstrap-JavaScript. Das Ergebnis
// waere ein zerlegtes Layout, das von Fremdcode abhaengt. Nur die Links zu
// uebernehmen kostet nichts und bleibt trotzdem in Takt mit dem, was auf der
// Kamera wirklich im Menue steht -- eine nachgebaute Liste waere schon beim
// naechsten OpenIPC-Update falsch.
//
// WARUM eine eigene Datei statt Inline-Skript: beide Machino-Seiten brauchen
// dasselbe. Zwei Kopien in zwei Raw-Strings driften auseinander, und der
// Browser kann eine eigene Datei zwischenspeichern.
//
// Schlaegt irgendetwas davon fehl -- keine Sitzung, Seite nicht erreichbar,
// kein nav im Dokument -- passiert NICHTS: die Seite bleibt die eigenstaendige,
// die sie heute ist. Eine Kopfleiste ist Komfort und darf keine Seite kosten.
#pragma once

#include <cstddef>

namespace machino {
namespace http {

const char* machino_chrome_js();
std::size_t machino_chrome_js_len();

} // namespace http
} // namespace machino
