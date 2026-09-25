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

  /* Gleiche Herkunft in ein <head>-Element uebernehmen. Fremde Ziele bleiben
     draussen: die Kamera hat kein Internet, und ein Stylesheet von einem
     Server, den es nicht erreicht, laedt die Seite nur langsamer. */
  function adopt(el, tag, attr) {
    var v = el.getAttribute(attr);
    if (!v) return;
    var u;
    try { u = new URL(v, location.href); } catch (e) { return; }
    if (u.origin !== location.origin) return;
    if (document.querySelector(tag + "[" + attr + '="' + u.pathname + '"]')) return;
    var n = document.createElement(tag);
    for (var i = 0; i < el.attributes.length; i++) {
      var a = el.attributes[i];
      n.setAttribute(a.name, a.name === attr ? u.pathname + u.search : a.value);
    }
    if (tag === "link") document.head.insertBefore(n, document.head.firstChild);
    else document.body.appendChild(n);
  }

  function go() {
    fetch(SOURCE, { credentials: "same-origin", redirect: "follow" })
      .then(function (r) { return r.ok ? r.text() : null; })
      .then(function (html) {
        if (!html) return;                     /* nicht angemeldet o.ae. */
        var doc = new DOMParser().parseFromString(html, "text/html");
        var nav = doc.querySelector("nav.navbar") || doc.querySelector("nav");
        if (!nav) return;

        /* Die Dateipfade werden NICHT geraten und stehen nirgends fest
           verdrahtet: sie stehen im <head> genau der Seite, die wir gerade
           geholt haben. Welche Dateien das auf dieser Kamera sind, steht in
           docs/openipc-webui-assets.md -- hier steht es absichtlich NICHT,
           damit kein Pfad aus Versehen zur Konstante wird. */
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

        /* Ihr Markup UNVERAENDERT. Der Inhalt des Menues haengt an der
           Firmware; eine Kopie im Quelltext waere beim naechsten Update
           falsch. */
        document.body.insertBefore(document.importNode(nav, true),
                                   document.body.firstChild);

        /* Nur die Skripte aus dem <head>. Die im <body> gehoeren der
           jeweiligen Seite (Vorschau, Diagramme) und haetten hier nichts zu
           tun ausser Last zu erzeugen. Ein Bootstrap-JS gibt es nicht; was
           die Dropdowns bedient, steht in ihrem main.js. */
        doc.querySelectorAll("head script[src]").forEach(function (s) {
          adopt(s, "script", "src");
        });
      })
      .catch(function () { /* Seite bleibt eigenstaendig */ });
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
