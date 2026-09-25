#include "app/http/chrome.hpp"

#include <cstring>

namespace machino {
namespace http {

static const char kJs[] = R"MACHINO_JS(/* machino: die Kopfleiste der Kamera-WebUI, siehe chrome.hpp */
(function () {
  "use strict";

  /* Die Stock-WebUI liegt auf DERSELBEN Herkunft: ihr Stylesheet unter /a/,
     ihr Verhalten in /a/main.js, und die Weiterleitung von / fuehrt auf
     live.cgi. Wir bauen davon nichts nach, wir benutzen es. */
  var CSS    = ["/a/bootstrap.min.css", "/a/bootstrap.override.css"];
  var JS     = "/a/main.js";
  var SOURCE = "/cgi-bin/live.cgi";

  /* Ihre Stylesheets kommen VOR unseren inline-Stil, damit unsere paar
     eigenen Regeln gewinnen, wo wir wirklich etwas eigenes brauchen. */
  function styles() {
    for (var i = CSS.length - 1; i >= 0; i--) {
      if (document.querySelector('link[href="' + CSS[i] + '"]')) continue;
      var l = document.createElement("link");
      l.rel = "stylesheet";
      l.href = CSS[i];
      document.head.insertBefore(l, document.head.firstChild);
    }
  }

  /* main.js ist ihr ganzes UI-Verhalten: Dropdowns, Collapse, das Markieren
     des aktiven Eintrags, der Abmelden-Knopf. Ein Bootstrap-JS gibt es auf
     dieser Kamera NICHT (geprueft: /a/bootstrap*.js -> 404) -- wer die
     Dropdowns ohne main.js will, muesste sie nachbauen, und genau das soll
     hier nicht passieren. */
  function behaviour() {
    if (document.querySelector('script[src="' + JS + '"]')) return;
    var s = document.createElement("script");
    s.src = JS;
    document.body.appendChild(s);
  }

  function go() {
    fetch(SOURCE, { credentials: "same-origin", redirect: "follow" })
      .then(function (r) { return r.ok ? r.text() : null; })
      .then(function (html) {
        if (!html) return;                         /* nicht angemeldet o.ae. */
        var doc = new DOMParser().parseFromString(html, "text/html");
        var nav = doc.querySelector("nav.navbar") || doc.querySelector("nav");
        if (!nav) return;
        styles();
        /* Ihr Markup UNVERAENDERT. Mit ihrem CSS und ihrem main.js verhaelt es
           sich hier genauso wie auf jeder anderen Seite der Kamera -- und es
           bleibt automatisch in Takt mit dem, was diese Firmware wirklich im
           Menue hat. Eine nachgebaute Liste waere beim naechsten Update falsch. */
        document.body.insertBefore(document.importNode(nav, true),
                                   document.body.firstChild);
        if (doc.body && doc.body.className) {
          document.body.className =
            (doc.body.className + " " + document.body.className).trim();
        }
        behaviour();
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

const char* machino_chrome_js() { return kJs; }
std::size_t machino_chrome_js_len() { return sizeof(kJs) - 1; }

} // namespace http
} // namespace machino
