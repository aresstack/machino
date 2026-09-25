#include "app/http/chrome.hpp"

#include <cstring>
#include <string>

namespace machino {
namespace http {

// Das Skript in zwei Haelften: dazwischen wird die Quellseite eingesetzt.
// Dass ausgerechnet SIE aus der Konfiguration kommt und die Dateipfade nicht,
// ist kein Zufall -- siehe chrome.hpp.
static const char kHead[] = R"MACHINO_JS(/* machino: Kopfleiste der Kamera-WebUI, siehe chrome.hpp */
(function () {
  "use strict";
  var SOURCE = ")MACHINO_JS";

static const char kTail[] = R"MACHINO_JS(";
  if (!SOURCE) return;                    /* ausdruecklich abgeschaltet */

  var srcUrl;
  try { srcUrl = new URL(SOURCE, location.href); } catch (e) { return; }

  /* Der Platz fuer die Leiste wird SOFORT reserviert, nicht erst wenn die
     Antwort da ist. Sonst baut sich die Seite auf, die Leiste kommt nach und
     schiebt alles nach unten -- gemessen am 2026-09-25, und genau so sah es
     aus: erst keine Leiste, dann rutscht die Seite. 58px ist die Hoehe der
     Bootstrap-Navbar dieser UI; schlaegt die Uebernahme fehl, verschwindet
     der Platzhalter wieder (der kleine Sprung bleibt dann dem Fehlerfall
     vorbehalten statt jedem Seitenaufbau). */
  var slot = document.createElement("div");
  slot.id = "mch-nav-slot";
  slot.style.minHeight = "58px";

  /* Gleiche Herkunft in ein <head>-Element uebernehmen. Fremde Ziele bleiben
     draussen: die Kamera hat kein Internet, und ein Stylesheet von einem
     Server, den es nicht erreicht, laedt die Seite nur langsamer. */
  function adopt(el, tag, attr) {
    var v = el.getAttribute(attr);
    if (!v) return;
    var u;
    try { u = new URL(v, srcUrl); } catch (e) { return; }
    if (u.origin !== location.origin) return;
    if (document.querySelector(tag + "[" + attr + '="' + u.pathname + '"]')) return;
    var n = document.createElement(tag);
    for (var i = 0; i < el.attributes.length; i++) {
      var a = el.attributes[i];
      n.setAttribute(a.name, a.name === attr ? u.pathname + u.search : a.value);
    }
    if (tag === "link") { document.head.insertBefore(n, document.head.firstChild); return; }
    /* main.js registriert seine Arbeit auf window "load". Auf dieser Seite
       ist load laengst durch, wenn das Skript hier eingefuegt wird -- der
       Abmelden-Knopf der Leiste bliebe tot. Also: ist die Seite schon fertig
       geladen, wird initAll einmal von Hand angestossen. Ist sie es nicht,
       feuert load noch von selbst, und angestossen wird nichts (sonst liefe
       es doppelt und jeder Bestaetigungsdialog kaeme zweimal). */
    n.onload = function () {
      if (document.readyState === "complete" && !window.__mchInitDone &&
          typeof window.initAll === "function") {
        window.__mchInitDone = 1;
        try { window.initAll(); } catch (e) {}
      }
    };
    document.body.appendChild(n);
  }

  /* Die Links der uebernommenen Leiste sind RELATIV zur Seite, aus der sie
     stammt: "dashboard.cgi" heisst /cgi-bin/dashboard.cgi. Unveraendert
     eingefuegt loest der Browser sie gegen /machino/... auf, und jeder Klick
     endet im 404 -- gemessen am 2026-09-25. Also gegen die QUELLSEITE
     aufloesen und absolut hineinschreiben. */
  function rebase(root) {
    var els = root.querySelectorAll("[href], [src]");
    for (var i = 0; i < els.length; i++) {
      var el = els[i];
      var attr = el.hasAttribute("href") ? "href" : "src";
      var v = el.getAttribute(attr);
      if (!v || v.charAt(0) === "#") continue;
      /* javascript:, mailto:, fremde Hosts: deren URL hat eine andere (oder
         gar keine) Herkunft und faellt am origin-Vergleich darunter heraus. */
      var u;
      try { u = new URL(v, srcUrl); } catch (e) { continue; }
      if (u.origin !== location.origin) continue;
      el.setAttribute(attr, u.pathname + u.search + u.hash);
    }
  }

  function go() {
    document.body.insertBefore(slot, document.body.firstChild);
    fetch(SOURCE, { credentials: "same-origin", redirect: "follow" })
      .then(function (r) { return r.ok ? r.text() : null; })
      .then(function (html) {
        if (!html) { slot.remove(); return; }  /* nicht angemeldet o.ae. */
        var doc = new DOMParser().parseFromString(html, "text/html");
        var nav = doc.querySelector("nav.navbar") || doc.querySelector("nav");
        if (!nav) { slot.remove(); return; }

        /* Die Dateipfade werden NICHT geraten und stehen nirgends fest
           verdrahtet: sie stehen im <head> genau der Seite, die wir gerade
           geholt haben. Welche das auf dieser Kamera sind, steht in
           docs/openipc-webui-assets.md -- hier absichtlich nicht. */
        doc.querySelectorAll('head link[rel~="stylesheet"]').forEach(function (l) {
          adopt(l, "link", "href");
        });

        /* Das Theme haengt an <html data-bs-theme>. Ohne das steht die Seite
           im hellen Schema, waehrend der Rest der Kamera dunkel ist. */
        var th = doc.documentElement.getAttribute("data-bs-theme");
        if (th) document.documentElement.setAttribute("data-bs-theme", th);
        if (doc.body && doc.body.className) {
          document.body.className =
            (doc.body.className + " " + document.body.className).trim();
        }

        /* Ihr Markup unveraendert -- bis auf die Linkziele (siehe rebase):
           der INHALT des Menues haengt an der Firmware, eine Kopie im
           Quelltext waere beim naechsten Update falsch. */
        var live = document.importNode(nav, true);
        rebase(live);
        slot.appendChild(live);

        /* Nur die Skripte aus dem <head>. Die im <body> gehoeren der
           jeweiligen Seite (Vorschau, Diagramme) und haetten hier nichts zu
           tun ausser Last zu erzeugen. Ein Bootstrap-JS gibt es nicht; was
           die Dropdowns bedient, steht in ihrem main.js. */
        doc.querySelectorAll("head script[src]").forEach(function (s) {
          adopt(s, "script", "src");
        });
      })
      .catch(function () { slot.remove(); /* Seite bleibt eigenstaendig */ });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", go);
  } else {
    go();
  }
})();
)MACHINO_JS";

// Der konfigurierte Wert landet in einem JavaScript-String-Literal. Ein
// Anfuehrungszeichen oder ein Zeilenumbruch darin waere sonst ein Weg, eigenen
// Code in jede ausgelieferte Seite zu bekommen -- und die Konfiguration kann
// ueber die API geschrieben werden.
static std::string js_quote(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': case '\r': break;          // in einer URL hat das nichts verloren
        case '<':  o += "\\u003c"; break;     // kein </script> aus Versehen
        default:   o += c;
        }
    }
    return o;
}

std::string machino_chrome_js(const std::string& source)
{
    return std::string(kHead, sizeof(kHead) - 1) + js_quote(source)
         + std::string(kTail, sizeof(kTail) - 1);
}

} // namespace http
} // namespace machino
