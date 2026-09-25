#include "app/http/devui.hpp"

#include <cstring>

namespace machino { namespace http {

// Ein einziges rohes Stringliteral, wie bei netui.cpp. Der Begrenzer ist
// )MACHINO_HTML und nicht )", weil im JavaScript )" vorkommt.
static const char kPage[] = R"MACHINO_HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ger&auml;te &mdash; machino</title>
<style>
:root{--bg:var(--bs-body-bg,#14161a);--panel:var(--bs-tertiary-bg,#1c2026);
--line:var(--bs-border-color,#2c323b);--fg:var(--bs-body-color,#e6e8ea);
--dim:var(--bs-secondary-color,#9aa3ad);--ok:var(--bs-success,#4caf7d);
--warn:var(--bs-warning,#d9a13b);--bad:var(--bs-danger,#d4564f);
--acc:var(--bs-primary,#4a90d9)}
*{box-sizing:border-box}
body{margin:0}
/* Ohne die Stock-CSS (nicht angemeldet, Datei fehlt) traegt die Seite ihr
   eigenes Aussehen weiter -- die Fallbacks oben sind genau dafuer da. */
body:not(.lite){background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,sans-serif}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;
gap:16px;align-items:baseline;flex-wrap:wrap}
h1{font-size:16px;margin:0;font-weight:600}
header a{color:var(--acc);text-decoration:none;font-size:13px}
main{padding:16px;max-width:900px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;
padding:14px;margin-bottom:14px}
.card h2{font-size:14px;margin:0 0 4px;font-weight:600}
.sub{color:var(--dim);font-size:13px;margin:0 0 10px}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--dim);font-weight:500;width:42%}
tr:last-child th,tr:last-child td{border-bottom:0}
.pill{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;
border:1px solid var(--line)}
.pill.ok{color:var(--ok);border-color:var(--ok)}
.pill.warn{color:var(--warn);border-color:var(--warn)}
.pill.bad{color:var(--bad);border-color:var(--bad)}
.pill.dim{color:var(--dim)}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;align-items:center}
button{background:#262c34;color:var(--fg);border:1px solid var(--line);
border-radius:4px;padding:7px 14px;font:inherit;cursor:pointer}
button:hover:not(:disabled){border-color:var(--acc)}
button:disabled{opacity:.45;cursor:default}
.note{color:var(--dim);font-size:13px;margin-top:10px}
.note.act{color:var(--warn)}
#msg{margin:0 0 14px;padding:10px 12px;border-radius:4px;border:1px solid var(--line)}
#msg.err{color:var(--bad);border-color:var(--bad)}
#msg.good{color:var(--ok);border-color:var(--ok)}
#msg[hidden]{display:none}
</style>
</head>
<body>
<header>
  <h1>Ger&auml;te</h1>
  <a href="/machino/net">Netzwerk &amp; USB</a>
  <a href="/">Kamera</a>
</header>
<main>
<p id="msg" hidden></p>

<!-- Der Satz ist nicht Deko. Ohne ihn ist die Seite irrefuehrend: sie sieht
     aus, als koenne man hier WLAN konfigurieren, und genau das kann man
     nicht. -->
<div class="card">
  <h2>Was diese Seite tut</h2>
  <p class="sub">Sie richtet die <em>Hardwareunterst&uuml;tzung</em> ein &mdash;
  Treiber nach <code>/lib/modules</code> und ein Profil nach
  <code>/etc/wireless/usb</code>. Danach bietet die unver&auml;nderte
  OpenIPC-Seite <em>Network</em> den Adapter unter &bdquo;Wireless
  Adapter&ldquo; an; dort wird das WLAN auch verbunden. Installieren schaltet
  den USB-Port <em>nicht</em> um &mdash; es gibt genau einen Port, und wer ihn
  bekommt, steht unter <a href="/machino/net">Netzwerk &amp; USB</a>.</p>
</div>

<div id="list"></div>
</main>

<script>
"use strict";
const $ = (s, r) => (r || document).querySelector(s);

function msg(text, kind) {
  const m = $("#msg");
  if (!text) { m.hidden = true; return; }
  m.hidden = false; m.textContent = text;
  m.className = kind || "";
}

// Die Zustandsnamen kommen vom Server (install_state_name). Hier stehen nur
// die Saetze dazu -- und zwar die, die der Benutzer braucht, nicht die
// Aufzaehlungsnamen. "install-pending" auf eine Seite zu schreiben und den
// Benutzer raten zu lassen, was jetzt zu tun ist, waere keine Oberflaeche.
const STATE = {
  "unsupported": ["nicht unterst&uuml;tzt", "dim",
    "Diese Machino-Fassung kennt das Ger&auml;t nicht."],
  "unavailable": ["keine Nutzlast", "dim",
    "Dieses Release bringt die Kernelmodule nicht mit. Ohne sie w&auml;re " +
    "&bdquo;Installieren&ldquo; ein Knopf, der nur scheitern kann."],
  "not-installed": ["verf&uuml;gbar", "warn",
    "Die Treiber liegen bereit, sind auf der Kamera aber noch nicht eingerichtet."],
  "install-pending": ["wird beim Neustart eingerichtet", "warn",
    "Vorgemerkt. Wirksam nach dem n&auml;chsten Neustart &mdash; machino l&auml;dt " +
    "keine Kernelmodule im laufenden Betrieb."],
  "installed": ["installiert", "ok",
    "Eingerichtet. Der Adapter steht auf der OpenIPC-Seite <em>Network</em> " +
    "unter &bdquo;Wireless Adapter&ldquo; zur Auswahl."],
  "remove-pending": ["wird beim Neustart entfernt", "warn",
    "Vorgemerkt. Die mitgelieferten Module bleiben erhalten, nur die " +
    "Registrierung wird zur&uuml;ckgebaut."],
};

function flag(on, yes, no) {
  return '<span class="pill ' + (on ? "ok" : "dim") + '">' + (on ? yes : no) + "</span>";
}

function card(d) {
  const st = STATE[d.state] || [d.state, "dim", ""];
  const el = document.createElement("div");
  el.className = "card";

  // Installieren nur, wenn es etwas zu tun gibt UND etwas zu installieren da
  // ist. Deinstallieren nur, wenn wirklich etwas eingetragen ist. Ein Knopf,
  // der nichts bewirken kann, ist eine Falschaussage.
  // Solange das Geraet den USB-Port haelt, wird nicht deinstalliert -- der
  // Server lehnt es ohnehin ab (409). Den Knopf trotzdem anzubieten hiesse,
  // den Benutzer gegen eine Fehlermeldung laufen zu lassen, die er vorher
  // haette sehen koennen.
  const canInstall   = d.state === "not-installed";
  const canUninstall = d.state === "installed" && !d.active;
  const blocked      = d.state === "installed" && d.active;
  const pending      = d.state === "install-pending" || d.state === "remove-pending";

  el.innerHTML =
    "<h2>" + esc(d.title) + ' <span class="pill ' + st[1] + '">' + st[0] + "</span></h2>" +
    '<p class="sub">' + st[2] + "</p>" +
    "<table><tbody>" +
    "<tr><th>Kennung</th><td><code>" + esc(d.id) + "</code></td></tr>" +
    "<tr><th>Treiber</th><td><code>" + esc(d.driver || "&mdash;") + "</code></td></tr>" +
    "<tr><th>Bei OpenIPC registriert</th><td>" +
      flag(d.openipcRegistered, "ja", "nein") + "</td></tr>" +
    "<tr><th>Hardware erkannt</th><td>" +
      flag(d.hardwarePresent, "ja", "nicht gesehen") +
      ' <span class="pill dim">nur USB-Kennung</span></td></tr>' +
    "<tr><th>Modul geladen</th><td>" + flag(d.driverLoaded, "ja", "nein") + "</td></tr>" +
    "<tr><th>H&auml;lt den USB-Port</th><td>" + flag(d.active, "ja", "nein") + "</td></tr>" +
    "</tbody></table>" +
    '<div class="row">' +
      '<button data-a="install"   ' + (canInstall   ? "" : "disabled") + ">Installieren</button>" +
      '<button data-a="uninstall" ' + (canUninstall ? "" : "disabled") + ">Deinstallieren</button>" +
    "</div>" +
    (d.detail ? '<p class="note">' + esc(d.detail) + "</p>" : "") +
    (pending ? '<p class="note act">Ein Neustart macht es wirksam.</p>' : "") +
    (blocked
      ? '<p class="note act">Dieses Ger&auml;t h&auml;lt gerade den USB-Port. Zum ' +
        'Deinstallieren zuerst unter <a href="/machino/net">Netzwerk &amp; USB</a> ' +
        "den Port freigeben.</p>"
      : "") +
    (canUninstall
      ? '<p class="note">Deinstallieren entfernt nur die Registrierung. Die ' +
        "mitgelieferten Module bleiben auf der Kamera, damit ein sp&auml;teres " +
        "Installieren ohne neues Paket m&ouml;glich bleibt.</p>"
      : "");

  el.querySelectorAll("button[data-a]").forEach((b) => {
    b.addEventListener("click", () => act(d.id, b.dataset.a, el));
  });
  return el;
}

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

async function act(id, verb, el) {
  el.querySelectorAll("button").forEach((b) => (b.disabled = true));
  msg("");
  try {
    const r = await fetch("/api/v1/devices/" + encodeURIComponent(id) + "/" + verb,
                          { method: "POST" });
    const body = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error((body.error && body.error.message) || ("HTTP " + r.status));
    msg(verb === "install"
        ? "Vorgemerkt. Nach dem nächsten Neustart steht der Adapter auf der "
          + "OpenIPC-Seite Network zur Auswahl."
        : "Vorgemerkt. Die Registrierung wird beim nächsten Neustart entfernt; "
          + "die Module bleiben liegen.",
        "good");
  } catch (e) {
    msg(String(e.message || e), "err");
  }
  await load();
}

async function load() {
  const box = $("#list");
  try {
    const r = await fetch("/api/v1/devices");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const j = await r.json();
    const list = (j && j.devices) || [];
    box.textContent = "";
    if (!list.length) {
      const p = document.createElement("div");
      p.className = "card";
      p.textContent = "Diese Fassung kennt keine Zusatzgeräte.";
      box.appendChild(p);
      return;
    }
    list.forEach((d) => box.appendChild(card(d)));
  } catch (e) {
    msg("Der Gerätestatus ist nicht abrufbar: " + String(e.message || e), "err");
  }
}

load();
// Kein Dauerpolling. Auf dieser Seite aendert sich nichts von selbst: jede
// Aenderung wird erst durch einen Neustart wirksam, und ein Sekundentakt
// waere reine Last auf einem Board, das ohnehin knapp ist.
</script>
<script src="/machino/chrome.js" defer></script>
</body>
</html>
)MACHINO_HTML";

const char* machino_devices_page() { return kPage; }
size_t      machino_devices_page_len() { return sizeof(kPage) - 1; }

}} // namespace machino::http
