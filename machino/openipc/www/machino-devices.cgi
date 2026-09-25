#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Device Manager" %>
<%in p/header.cgi %>
<!-- machino-owned page. Installed by machino, removed by its uninstall; no
     OpenIPC file is modified. Rendered by the SAME pipeline as every stock
     page (common/header/footer includes), so head, navbar, theme and main.js
     are in the first HTML and relative links resolve like everywhere else.
     Data comes only from machino's /api/v1. Components are the stock ones
     (card/card-body, mj-cap, mj-card-note, badge, btn) -- the page must not
     read as a second product. -->
<style>
/* Only what the stock UI has no component for: the two-button row. */
#mch .grid{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px}
</style>
<div id="mch">

<div class="row g-4">
<div class="col-12"><p id="msg" class="alert py-2" hidden></p></div>

<!-- Der Satz ist nicht Deko. Ohne ihn ist die Seite irrefuehrend: sie sieht
     aus, als koenne man hier WLAN konfigurieren, und genau das kann man
     nicht. -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
  <div class="mj-live-head"><h3 class="mj-cap">What this page does</h3><span class="mj-live-rule"></span></div>
  <p class="mj-card-note">It sets up the <em>hardware support</em> &mdash;
  drivers into <code>/lib/modules</code> and a profile into
  <code>/etc/wireless/usb</code>. The unmodified OpenIPC <em>Network</em> page
  then offers the adapter under &ldquo;Wireless Adapter&rdquo;, and that is
  where Wi-Fi gets connected. Installing does <em>not</em> switch the USB
  port &mdash; there is exactly one port, and who owns it is decided on
  <a href="machino-network.cgi">Network &amp; USB</a>.</p>
</div></div></div>

<div class="col-12"><div class="row g-4" id="list"></div></div>
</div>

<script>
// Alles hier ist LOKAL (IIFE). Der Grund ist konkret: header.cgi laedt
// /a/main.js der Kamera-WebUI, und das deklariert global `function $`.
// Stand hier ein globales `const $`, starb main.js beim Parsen
// ("Identifier '$' has already been declared") -- und mit ihm jedes
// Dropdown der Leiste. Gemessen am 2026-09-25.
(function () {

"use strict";
const $ = (s, r) => (r || document).querySelector(s);

async function mchFetch(path, init) {
  const opts = Object.assign({ credentials: "same-origin" }, init || {});
  const r = await fetch(path, opts);
  if (r.status === 401) {
    location.href = "/login.html?next=/cgi-bin/machino-devices.cgi";
    throw new Error("unauthorized");
  }
  return r;
}

function msg(text, kind) {
  const m = $("#msg");
  if (!text) { m.hidden = true; return; }
  m.hidden = false; m.textContent = text;
  m.className = "alert py-2 " +
    (kind === "good" ? "alert-success" : kind === "err" ? "alert-danger" : "alert-secondary");
}

// Die Zustandsnamen kommen vom Server (install_state_name). Hier stehen nur
// die Saetze dazu -- die, die der Benutzer braucht, nicht die
// Aufzaehlungsnamen. Sichtbarer Text ist ENGLISCH wie der Rest der WebUI.
const STATE = {
  "unsupported": ["not supported", "text-bg-secondary",
    "This machino build does not know this device."],
  "unavailable": ["no payload", "text-bg-secondary",
    "This release does not ship the kernel modules. Without them, " +
    "&ldquo;Install&rdquo; would be a button that can only fail."],
  "not-installed": ["available", "text-bg-warning",
    "The drivers are on the camera but not set up yet."],
  "install-pending": ["set up at next reboot", "text-bg-warning",
    "Scheduled. Takes effect after the next reboot &mdash; machino loads no " +
    "kernel modules while the camera is running."],
  "installed": ["installed", "text-bg-success",
    "Set up. The adapter is selectable on the OpenIPC <em>Network</em> page " +
    "under &ldquo;Wireless Adapter&rdquo;."],
  "remove-pending": ["removed at next reboot", "text-bg-warning",
    "Scheduled. The shipped modules stay on the camera; only the " +
    "registration is rolled back."],
};

function flag(on, yes, no) {
  return '<span class="badge ' + (on ? "text-bg-success" : "text-bg-secondary") + '">' + (on ? yes : no) + "</span>";
}

function card(d) {
  const st = STATE[d.state] || [d.state, "text-bg-secondary", ""];
  const col = document.createElement("div");
  col.className = "col-12 col-lg-6";

  // Installieren nur, wenn es etwas zu tun gibt UND etwas zu installieren da
  // ist. Deinstallieren nur, wenn wirklich etwas eingetragen ist. Ein Knopf,
  // der nichts bewirken kann, ist eine Falschaussage.
  // Solange das Geraet den USB-Port haelt, wird nicht deinstalliert -- der
  // Server lehnt es ohnehin ab (409).
  const canInstall   = d.state === "not-installed";
  const canUninstall = d.state === "installed" && !d.active;
  const blocked      = d.state === "installed" && d.active;
  const pending      = d.state === "install-pending" || d.state === "remove-pending";

  col.innerHTML =
    '<div class="card h-100"><div class="card-body">' +
    '<div class="mj-live-head"><h3 class="mj-cap">' + esc(d.title) +
      ' <span class="badge ' + st[1] + '">' + st[0] + '</span></h3>' +
      '<span class="mj-live-rule"></span></div>' +
    '<p class="mj-card-note">' + st[2] + "</p>" +
    '<table class="table table-sm"><tbody>' +
    "<tr><th>Id</th><td><code>" + esc(d.id) + "</code></td></tr>" +
    "<tr><th>Driver</th><td><code>" + esc(d.driver || "&mdash;") + "</code></td></tr>" +
    "<tr><th>Registered with OpenIPC</th><td>" +
      flag(d.openipcRegistered, "yes", "no") + "</td></tr>" +
    "<tr><th>Hardware detected</th><td>" +
      flag(d.hardwarePresent, "yes", "not seen") +
      ' <span class="badge text-bg-secondary">USB id only</span></td></tr>' +
    "<tr><th>Module loaded</th><td>" + flag(d.driverLoaded, "yes", "no") + "</td></tr>" +
    "<tr><th>Holds the USB port</th><td>" + flag(d.active, "yes", "no") + "</td></tr>" +
    "</tbody></table>" +
    '<div class="grid">' +
      '<button class="btn btn-sm btn-primary" data-a="install" ' +
        (canInstall ? "" : "disabled") + ">Install</button>" +
      '<button class="btn btn-sm btn-outline-secondary" data-a="uninstall" ' +
        (canUninstall ? "" : "disabled") + ">Uninstall</button>" +
    "</div>" +
    (d.detail ? '<p class="mj-card-note">' + esc(d.detail) + "</p>" : "") +
    (pending ? '<p class="mj-card-note text-warning">A reboot makes it effective.</p>' : "") +
    (blocked
      ? '<p class="mj-card-note text-warning">This device currently holds the USB port. To ' +
        'uninstall, release the port on <a href="machino-network.cgi">Network &amp; USB</a> ' +
        "first.</p>"
      : "") +
    (canUninstall
      ? '<p class="mj-card-note">Uninstalling only removes the registration. The ' +
        "shipped modules stay on the camera, so a later install needs no new " +
        "package.</p>"
      : "") +
    "</div></div>";

  col.querySelectorAll("button[data-a]").forEach((b) => {
    b.addEventListener("click", () => act(d.id, b.dataset.a, col));
  });
  return col;
}

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

async function act(id, verb, el) {
  el.querySelectorAll("button").forEach((b) => (b.disabled = true));
  msg("");
  try {
    const r = await mchFetch("/api/v1/devices/" + encodeURIComponent(id) + "/" + verb,
                             { method: "POST" });
    const body = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error((body.error && body.error.message) || ("HTTP " + r.status));
    msg(verb === "install"
        ? "Scheduled. After the next reboot the adapter is selectable on the "
          + "OpenIPC Network page."
        : "Scheduled. The registration is removed at the next reboot; the "
          + "modules stay on the camera.",
        "good");
  } catch (e) {
    msg(String(e.message || e), "err");
  }
  await load();
}

async function load() {
  const box = $("#list");
  try {
    const r = await mchFetch("/api/v1/devices");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const j = await r.json();
    const list = (j && j.devices) || [];
    box.textContent = "";
    if (!list.length) {
      const p = document.createElement("div");
      p.className = "col-12";
      p.innerHTML = '<div class="card"><div class="card-body">' +
                    "This build knows no add-on devices.</div></div>";
      box.appendChild(p);
      return;
    }
    list.forEach((d) => box.appendChild(card(d)));
  } catch (e) {
    msg("The device status is not readable: " + String(e.message || e), "err");
  }
}

load();
// Kein Dauerpolling. Auf dieser Seite aendert sich nichts von selbst: jede
// Aenderung wird erst durch einen Neustart wirksam, und ein Sekundentakt
// waere reine Last auf einem Board, das ohnehin knapp ist.

})();
</script>

</div>
<%in p/footer.cgi %>
