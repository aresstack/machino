#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="USB" %>
<%in p/header.cgi %>
<!-- machino-owned page (installed by machino, removed by its uninstall;
     no OpenIPC file is modified). Same pipeline as every stock page.
     The one USB port: who owns it, whether it is powered, what is plugged in. -->
<style>
/* Nur wofuer die Stock-UI keine Komponente hat -- alles andere kommt aus
   Bootstrap/bootstrap.override.css der Kamera. */
#mch .grid{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}
#mch .grid>*{flex:1 1 180px}
#mch .scan td{cursor:pointer}
#mch #count{font-variant-numeric:tabular-nums;font-weight:600}
#mch .unavail{color:var(--bs-secondary-color,#9aa3ad);font-style:italic;margin:6px 0}
#mch label{display:block;margin:8px 0 2px}
#mch .btn{margin-top:8px}
#mch th{width:40%}
</style>
<div id="mch">

<div class="row g-4">
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">USB role</h3><span class="mj-live-rule"></span></div>
    <p class="mj-card-note">The camera has <b>one</b> USB port. It carries nothing, a Wi-Fi module
      or a 4G modem &mdash; not two things.</p>
    <label><input type="radio" name="usbmode" value="off" style="width:auto"> Disabled</label>
    <label><input type="radio" name="usbmode" value="wifi" style="width:auto"> Wi-Fi</label>
    <label><input type="radio" name="usbmode" value="cellular" style="width:auto"> Cellular (4G)</label>
    <p class="mj-card-note">This only assigns the port. SSID, password and IP address of a
      Wi-Fi client connection live on OpenIPC's own
      <a href="network.cgi">Network</a> page &mdash; once the adapter is
      registered on <a href="machino-devices.cgi">Device Manager</a>, it
      appears there under <b>Wireless adapter</b>.</p>
    <p class="mj-card-note" id="usbmode-note">Takes effect after a reboot.</p>
    <button class="btn btn-sm btn-primary" id="usbmodesave">Apply</button>
    <div class="alert py-2" id="m-usbmode" hidden></div>
    <p class="mj-card-note">With <b>Disabled</b>, no driver is loaded at boot, the port power on
      PB18 stays down and no service runs for it. The USB port is then fully
      available to any other device. That is also the reason for the reboot:
      swapping kernel modules under a running media pipeline would be the
      unsafe path.</p>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Port power</h3><span class="mj-live-rule"></span></div>
    <div id="usb-unavail" class="unavail" hidden></div>
    <div id="usb-form">
      <p class="mj-card-note">On this board a GPIO drives a transistor that
      switches <b>3.3&nbsp;V</b> onto the USB VBUS pin. With power off, no
      device enumerates &mdash; an add-on board looks absent even though it is
      plugged in.</p>
      <label><input type="checkbox" id="usben" style="width:auto"> Power the port</label>
      <label><input type="checkbox" id="usbboot" style="width:auto"> Keep it powered from every boot on</label>
      <div class="grid">
        <div><label class="form-label" for="usbpin">GPIO pin that drives the transistor</label><input class="form-control form-control-sm" id="usbpin" placeholder="Board default"></div>
        <div><label class="form-label" for="usblvl">Active level</label>
          <select class="form-select form-select-sm" id="usblvl"><option value="high">high</option><option value="low">low</option></select></div>
      </div>
      <div id="usbexpertbox">
        <label><input type="checkbox" id="usbexpert" style="width:auto"> Expert mode: allow any pin</label>
        <p class="mj-card-note">&ldquo;Board default&rdquo; uses the pin from the
        board profile. Without expert mode, only pins the profile lists as
        wired for this purpose are selectable &mdash; the remaining GPIO range
        on this board is the sensor reset, the PHY reset and the flash.</p>
      </div>
      <button class="btn btn-sm btn-primary" id="usbsave">Apply</button>
      <div class="alert py-2" id="m-usb" hidden></div>
    </div>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">USB host</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="usbhost"><tbody></tbody></table>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Connected devices</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="usbdev"><tbody></tbody></table>
    <p class="mj-card-note">USB is not part of the media lifecycle. A device that appears,
    disappears or fails cannot touch the video path; the camera keeps
    streaming no matter what happens on the port.</p>
</div></div></div>

</div>

<script>
// Alles hier ist LOKAL (IIFE): header.cgi laedt /a/main.js der Kamera-WebUI,
// und das deklariert global `function $`. Ein globales `const $` hier liess
// main.js beim Parsen sterben -- und mit ihm jedes Dropdown der Leiste.
// Gemessen am 2026-09-25.
(function () {

"use strict";
const $ = (id) => document.getElementById(id);

async function api(method, path, body) {
  const r = await fetch(path, {
    method,
    headers: body === undefined ? {} : {"Content-Type": "application/json"},
    body: body === undefined ? undefined : JSON.stringify(body),
    credentials: "same-origin"
  });
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-usb.cgi"; throw new Error("unauthorized"); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  return {status: r.status, body: j};
}

function msg(el, text, kind) {
  el.textContent = text;
  el.className = "alert py-2 " + (kind === "ok" ? "alert-success" : kind === "bad" ? "alert-danger" : "alert-secondary");
  el.hidden = !text;
}
// A failure reply is shown verbatim. The API answers with the concrete reason
// ("hostapd is not in this image"), and replacing it with a generic
// "Fehler" here would throw away the only useful part.
function reason(res, fallback) {
  const e = res.body && (res.body.error || res.body);
  return (e && (e.message || e.error)) || fallback || ("HTTP " + res.status);
}

function row(tb, k, v) {
  const tr = document.createElement("tr");
  const th = document.createElement("th"); th.textContent = k;
  const td = document.createElement("td");
  if (v instanceof Node) td.appendChild(v); else td.textContent = (v === "" || v == null) ? "–" : v;
  tr.append(th, td); tb.appendChild(tr);
}
function pill(text, kind) {
  const s = document.createElement("span");
  s.className = "badge " + (kind === "ok" ? "text-bg-success" : kind === "warn" ? "text-bg-warning" : kind === "bad" ? "text-bg-danger" : "text-bg-secondary");
  s.textContent = text; return s;
}
function stateKind(s) {
  if (s === "connected") return "ok";
  if (s === "connecting") return "warn";
  if (s === "absent") return "";
  return "bad";
}


let modeSaved = null;      // was in der Konfiguration steht
let modeBooted = null;     // was beim Start tatsaechlich geladen wurde

const MODE_LABEL = {off: "Disabled", wifi: "Wi-Fi", cellular: "Cellular (4G)"};

function selectedMode() {
  const r = document.querySelector('input[name=usbmode]:checked');
  return r ? r.value : null;
}

function markModePending() {
  const n = $("usbmode-note");
  if (modeSaved === null) { n.textContent = "Takes effect after a reboot."; return; }
  const want = selectedMode();
  if (want !== modeSaved) {
    n.textContent = "Not saved. Apply, then reboot.";
    return;
  }
  // Drei Zustaende, nicht zwei. "Saved" und "laeuft" sind hier
  // verschiedene Dinge, und sie fallen genau zwischen Speichern und Neustart
  // auseinander -- deshalb vergleicht die Seite mit dem, was der Boot-Helfer
  // wirklich gestartet hat, statt sich ein Kennzeichen zu merken, das den
  // Neustart ueberleben wuerde.
  if (modeBooted !== null && modeSaved !== modeBooted) {
    n.textContent = "Saved: " + (MODE_LABEL[modeSaved] || modeSaved)
      + ". Still active: " + (MODE_LABEL[modeBooted] || modeBooted)
      + " — a reboot is required.";
    return;
  }
  n.textContent = modeSaved === "off"
    ? "The USB port is not in use: no driver, no port power, no service."
    : (MODE_LABEL[modeSaved] || modeSaved) + " is active.";
}


async function loadUsb() {
  const res = await api("GET", "/api/v1/usb");
  const tb = $("usbhost").querySelector("tbody"); tb.textContent = "";
  if (res.status !== 200) {
    row(tb, "–", "USB host is not available in this build");
    $("usb-form").hidden = true;
    $("usb-unavail").hidden = false;
    $("usb-unavail").textContent = "USB host is not available in this build.";
    return;
  }
  const u = res.body;
  const c = u.capabilities || {}, cp = c.power || {};
  const cfg = u.config || {}, cfgp = cfg.power || {}, st = u.power || {};
  row(tb, "Host operation", c.hostSupported ? (u.hostActive ? "active" : "supported, not active") : "not supported");
  row(tb, "Controller", c.controller);
  row(tb, "Top speed", c.maxSpeed);
  row(tb, "Port power switchable", cp.switchable ? "yes" : "no");
  if (cp.voltageMv) row(tb, "Port voltage", (cp.voltageMv / 1000) + " V");
  // "unknown" is a third answer and is kept as one: a hard-wired rail cannot
  // be read back, and showing "off" there would look like a fault.
  row(tb, "Port power", st.state === "on" ? "on" : st.state === "off" ? "off" : "not readable");
  row(tb, "Resolved mode", (st.mode || "–") + (st.pin ? " on " + st.pin : ""));
  if (cp.allowedPins && cp.allowedPins.length)
    row(tb, "Released pins", cp.allowedPins.join(", "));

  const sw = !!cp.switchable;
  $("usb-form").hidden = !sw;
  $("usb-unavail").hidden = sw;
  $("usb-unavail").textContent = c.hostSupported
      ? "This board has no switchable port power; there is nothing to set."
      : "This board has no USB host.";
  // Die Modusauswahl haengt NICHT an cp.switchable: ein Board ohne
  // schaltbaren Portstrom kann trotzdem ein Modul am Port tragen.
  modeSaved  = cfg.mode || (res.body.mode || "off");
  modeBooted = (res.body.bootMode !== undefined) ? res.body.bootMode : null;
  const radio = document.querySelector('input[name=usbmode][value="' + modeSaved + '"]');
  if (radio) radio.checked = true;
  markModePending();

  $("usben").checked = !!cfg.enabled;
  $("usbboot").checked = !!cfgp.enableAtBoot;
  $("usbexpert").checked = !!cfgp.expert;
  $("usbpin").value = cfgp.pin || "";
  $("usblvl").value = cfgp.activeLevel || "high";

  const dtb = $("usbdev").querySelector("tbody"); dtb.textContent = "";
  const dres = await api("GET", "/api/v1/usb/devices");
  // The endpoint answers with the array itself, not an object wrapping one.
  const list = Array.isArray(dres.body) ? dres.body : [];
  if (!list.length) row(dtb, "–", "no device connected");
  list.forEach((d) => {
    const box = document.createElement("div");
    box.textContent = [d.vid, d.pid].filter(Boolean).join(":");
    const n2 = document.createElement("div"); n2.className = "small text-secondary";
    n2.textContent = [d.product, d.manufacturer, d.speed, d.maxPowerMa ? d.maxPowerMa + " mA" : ""]
        .filter(Boolean).join(" · ");
    box.appendChild(n2);
    (d.interfaces || []).forEach((i) => {
      const li = document.createElement("div"); li.className = "small text-secondary";
      li.textContent = "Interface class " + i["class"] + (i.driver ? " · driver " + i.driver : " · no driver");
      box.appendChild(li);
    });
    row(dtb, d.path || d.product || "Device", box);
  });
}


document.querySelectorAll('input[name=usbmode]')
        .forEach(r => r.addEventListener("change", markModePending));

$("usbmodesave").onclick = async () => {
  const want = selectedMode();
  if (!want) return;
  const res = await api("PATCH", "/api/v1/usb", { mode: want });
  if (res.status === 200) {
    modeSaved = (res.body && res.body.config && res.body.config.mode) || want;
    modeBooted = (res.body && res.body.bootMode !== undefined) ? res.body.bootMode : modeBooted;
    markModePending();
    // Kein "Applied." allein: uebernommen ist die EINSTELLUNG, nicht der
    // Zustand des Ports. Wer hier nur Erfolg meldet, laesst jemanden auf ein
    // Modem warten, das erst nach einem Neustart existiert.
    const pending = (modeBooted !== null && modeBooted !== modeSaved);
    msg($("m-usbmode"), pending
        ? "Saved. Takes effect after a reboot — still running: "
          + (MODE_LABEL[modeBooted] || modeBooted) + "."
        : "Saved.", "ok");
  } else {
    msg($("m-usbmode"), reason(res, "Save failed"), "bad");
  }
};

$("usbsave").onclick = async () => {
  const body = {
    enabled: $("usben").checked,
    power: {
      enableAtBoot: $("usbboot").checked,
      expert: $("usbexpert").checked,
      activeLevel: $("usblvl").value
    }
  };
  // An empty pin means "board default". The API rejects unknown keys, so
  // sending the field empty and omitting it are both valid but different
  // requests; omitting it is the one that means "leave it alone".
  if ($("usbpin").value.trim()) body.power.pin = $("usbpin").value.trim();
  const res = await api("PATCH", "/api/v1/usb", body);
  if (res.status === 200) { msg($("m-usb"), "Applied.", "ok"); await loadUsb(); }
  else msg($("m-usb"), reason(res, "Apply failed"), "bad");
};


async function refresh() {
  try { await loadUsb(); } catch (e) {}
}
refresh();

})();
</script>

</div>
<%in p/footer.cgi %>
