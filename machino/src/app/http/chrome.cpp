#include "app/http/chrome.hpp"

#include <cstring>

namespace machino {
namespace http {

static const char kJs[] = R"MACHINO_JS(/* machino: Kopfleiste der Kamera-WebUI, siehe chrome.hpp */
(function () {
  "use strict";

  /* Woher die Links kommen. "/" ist die Startseite der Stock-WebUI und traegt
     dieselbe Kopfleiste wie jede andere; sie zu holen ist ein Request auf
     derselben Herkunft, also geht die Sitzung automatisch mit. */
  var SOURCE = "/";

  function css() {
    var s = document.createElement("style");
    s.textContent = [
      "#mch-chrome{display:flex;gap:4px;align-items:center;flex-wrap:wrap;",
      "padding:6px 16px;border-bottom:1px solid var(--line,#2c323b);",
      "background:var(--panel,#1c2026);font:13px/1.4 system-ui,sans-serif}",
      "#mch-chrome .mch-brand{font-weight:600;color:var(--fg,#e6e8ea);margin-right:10px}",
      "#mch-chrome a{color:var(--dim,#9aa3ad);text-decoration:none;padding:5px 9px;",
      "border-radius:4px;white-space:nowrap}",
      "#mch-chrome a:hover{background:#22272e;color:var(--fg,#e6e8ea)}",
      "#mch-chrome a.mch-here{color:var(--fg,#e6e8ea);background:#22272e}",
      "#mch-chrome details{position:relative}",
      "#mch-chrome summary{list-style:none;cursor:pointer;color:var(--dim,#9aa3ad);",
      "padding:5px 9px;border-radius:4px;white-space:nowrap}",
      "#mch-chrome summary::-webkit-details-marker{display:none}",
      "#mch-chrome summary:hover{background:#22272e;color:var(--fg,#e6e8ea)}",
      "#mch-chrome details[open]>summary{background:#22272e;color:var(--fg,#e6e8ea)}",
      "#mch-chrome details .mch-menu{position:absolute;z-index:50;top:100%;left:0;",
      "min-width:190px;background:var(--panel,#1c2026);border:1px solid var(--line,#2c323b);",
      "border-radius:6px;padding:4px;display:flex;flex-direction:column;",
      "box-shadow:0 8px 24px rgba(0,0,0,.45)}"
    ].join("");
    document.head.appendChild(s);
  }

  /* Ein Eintrag der Stock-Navigation: entweder ein echter Link oder ein
     Aufklapper mit Kindern. Alles ohne Ziel (href="#", javascript:) faellt
     weg -- das sind deren Dropdown-Schalter, die ohne ihr JavaScript ohnehin
     nichts tun. */
  function harvest(doc) {
    var out = [];
    var nav = doc.querySelector("nav");
    if (!nav) return out;
    var real = function (a) {
      var h = a.getAttribute("href");
      if (!h) return null;
      h = h.trim();
      if (!h || h.charAt(0) === "#") return null;
      if (/^javascript:/i.test(h)) return null;
      var t = (a.textContent || "").replace(/\s+/g, " ").trim();
      if (!t) return null;
      return { href: h, text: t };
    };
    nav.querySelectorAll("li").forEach(function (li) {
      if (li.closest(".dropdown-menu")) return;      /* Kinder kommen unten */
      var menu = li.querySelector(".dropdown-menu");
      if (menu) {
        var label = (li.querySelector("a,button") || {}).textContent || "";
        var kids = [];
        menu.querySelectorAll("a").forEach(function (a) {
          var e = real(a);
          if (e) kids.push(e);
        });
        if (kids.length) {
          out.push({ text: label.replace(/\s+/g, " ").trim() || "Mehr", kids: kids });
        }
        return;
      }
      var a = li.querySelector("a");
      if (!a) return;
      var e = real(a);
      if (e) out.push(e);
    });
    return out;
  }

  function here(href) {
    try {
      return new URL(href, location.href).pathname.replace(/\/$/, "") ===
             location.pathname.replace(/\/$/, "");
    } catch (e) { return false; }
  }

  function link(e) {
    var a = document.createElement("a");
    a.href = e.href;
    a.textContent = e.text;
    if (here(e.href)) a.className = "mch-here";
    return a;
  }

  function render(items) {
    var bar = document.createElement("div");
    bar.id = "mch-chrome";
    var b = document.createElement("span");
    b.className = "mch-brand";
    b.textContent = "OpenIPC";
    bar.appendChild(b);
    items.forEach(function (e) {
      if (!e.kids) { bar.appendChild(link(e)); return; }
      var d = document.createElement("details");
      var s = document.createElement("summary");
      s.textContent = e.text;
      d.appendChild(s);
      var m = document.createElement("div");
      m.className = "mch-menu";
      var openHere = false;
      e.kids.forEach(function (k) {
        if (here(k.href)) openHere = true;
        m.appendChild(link(k));
      });
      d.appendChild(m);
      if (openHere) s.className = "mch-here";
      bar.appendChild(d);
    });
    /* Ein Klick daneben schliesst die Aufklapper wieder. */
    document.addEventListener("click", function (ev) {
      bar.querySelectorAll("details[open]").forEach(function (d) {
        if (!d.contains(ev.target)) d.removeAttribute("open");
      });
    });
    document.body.insertBefore(bar, document.body.firstChild);
  }

  function go() {
    fetch(SOURCE, { credentials: "same-origin", redirect: "follow" })
      .then(function (r) { return r.ok ? r.text() : null; })
      .then(function (html) {
        if (!html) return;
        var doc = new DOMParser().parseFromString(html, "text/html");
        var items = harvest(doc);
        if (!items.length) return;                 /* nicht angemeldet o.ae. */
        css();
        render(items);
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
