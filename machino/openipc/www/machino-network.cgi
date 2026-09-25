#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Network & USB" %>
<%in p/header.cgi %>
<!-- machino-owned page. Installed by machino, removed by its uninstall;
     no OpenIPC file is modified. Rendered by the SAME pipeline as every
     stock page (common/header/footer includes), so head, navbar, theme
     and main.js are in the first HTML and relative links resolve like
     everywhere else. Data comes only from machino's /api/v1. -->
<style>
/* Nur wofuer die Stock-UI keine Komponente hat -- alles andere kommt aus
   Bootstrap/bootstrap.override.css der Kamera. Farben ueber --bs-*-Variablen,
   damit Hell/Dunkel mitgeht. */
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

<!-- The confirmation banner is OUTSIDE the tabs and sticky: a change that will
     roll itself back must be visible no matter which tab the user wandered
     onto, and it is the one thing on this page that is time-critical. -->
<div id="pending" class="alert alert-warning" hidden>
  <strong>A change is waiting for confirmation.</strong>
  It is rolled back automatically in <span id="count">–</span> unless it is
  confirmed. Confirm only once the camera is still reachable with the new
  configuration &mdash; that is exactly what the window is for.
  <div class="row" style="margin-top:8px">
    <button class="btn btn-sm btn-primary" id="confirm">Confirm now</button>
  </div>
</div>

<div class="row g-4">

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Uplinks</h3><span class="mj-live-rule"></span></div>
    <!-- Stand frueher im eigenen Seitenkopf; der gehoert jetzt header.cgi. -->
    <p class="mj-card-note" id="active"></p>
    <table class="table table-sm" id="uplinks"><tbody></tbody></table>
    <p class="mj-card-note">An access point is not an uplink. A camera that provides its own Wi-Fi
    network is working as intended and deliberately has no internet
    connection.</p>
</div></div></div>


<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Ethernet</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="eth"><tbody></tbody></table>
    <p class="mj-card-note">Ethernet is not configured from here. Address and route belong to the
    boot scripts and udhcpc; this page only reports. There is deliberately
    no &ldquo;disconnect&rdquo;: eth0 is how this camera is reached.</p>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Wi-Fi status</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="wifistat"><tbody></tbody></table>
</div></div></div>
<div class="col-12 col-lg-6" id="c-station"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Connect as a client</h3><span class="mj-live-rule"></span></div>
    <button class="btn btn-sm btn-outline-secondary" id="scan">Scan for networks</button>
    <table class="table table-sm table-hover scan" id="scanres"><tbody></tbody></table>
    <label class="form-label" for="ssid">SSID</label><input class="form-control form-control-sm" id="ssid" maxlength="32" autocomplete="off">
    <label class="form-label" for="psk">Password</label>
    <input class="form-control form-control-sm" id="psk" type="password" maxlength="64" autocomplete="new-password">
    <p class="mj-card-note">8 to 63 characters, or exactly 64 hex characters for a raw PSK. Leave
    empty for an open network. The password is never returned &mdash; not
    even to this page.</p>
    <button class="btn btn-sm btn-primary" id="join">Connect (with confirmation window)</button>
    <div class="alert py-2" id="m-station" hidden></div>
</div></div></div>
<div class="col-12 col-lg-6" id="c-ap"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Provide an access point</h3><span class="mj-live-rule"></span></div>
    <div id="ap-unavail" class="unavail" hidden></div>
    <p class="mj-card-note" id="ap-unverified" hidden>Whether this radio supports access-point mode was not queried &mdash;
    this build makes no nl80211 request. Trying is allowed; if it fails,
    hostapd reports the reason instead of this page promising anything.</p>
    <div id="ap-form">
      <div class="grid">
        <div><label class="form-label" for="apssid">SSID</label><input class="form-control form-control-sm" id="apssid" maxlength="32"></div>
        <div><label class="form-label" for="apsec">Security</label>
          <select class="form-select form-select-sm" id="apsec">
            <option value="wpa2">WPA2</option>
            <option value="wpa2-wpa3">WPA2/WPA3</option>
            <option value="wpa3">WPA3</option>
            <option value="open">open</option>
          </select></div>
        <div><label class="form-label" for="apch">Channel</label><input class="form-control form-control-sm" id="apch" type="number" min="0" max="196" value="6"></div>
      </div>
      <label class="form-label" for="appsk">Password</label>
      <input class="form-control form-control-sm" id="appsk" type="password" maxlength="63" autocomplete="new-password">
      <div class="grid">
        <div><label class="form-label" for="apip">Camera address</label><input class="form-control form-control-sm" id="apip" value="192.168.4.1"></div>
        <div><label class="form-label" for="apfrom">DHCP from</label><input class="form-control form-control-sm" id="apfrom" value="192.168.4.20"></div>
        <div><label class="form-label" for="apto">DHCP to</label><input class="form-control form-control-sm" id="apto" value="192.168.4.100"></div>
      </div>
      <button class="btn btn-sm btn-primary" id="apstart">Start access point (with confirmation window)</button>
      <div class="alert py-2" id="m-ap" hidden></div>
    </div>
</div></div></div>

  <!-- Wenn USB nicht auf Mobilfunk steht, wird das GESAGT und nicht durch
       graue Felder angedeutet. Ein Formular, das sich speichern laesst und
       nichts bewirkt, ist schlimmer als eines, das erklaert warum. -->
  <div id="cell-offmode" class="unavail" hidden></div>

<div class="col-12 col-lg-6" id="cell-statuscard"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Status</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="cell"><tbody></tbody></table>
    <div id="cell-none" class="unavail" hidden>
      No cellular backend in this build. This page says so instead of
      offering controls that do nothing.
    </div>
</div></div></div>

<div class="col-12 col-lg-6" id="cell-linkcard"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Data link</h3><span class="mj-live-rule"></span></div>
    <label><input type="radio" name="celldl" value="ecm" style="width:auto"> ECM (default)</label>
    <label><input type="radio" name="celldl" value="ppp" style="width:auto"> PPP</label>
    <p class="mj-card-note" id="celldl-note">Takes effect after a reboot.</p>
    <button class="btn btn-sm btn-primary" id="celldlsave">Apply (with confirmation window)</button>
    <div class="alert py-2" id="m-celldl" hidden></div>
    <p class="mj-card-note">ECM is the normal path: the modem presents itself as a network card.
      PPP is the fallback for modems or networks where that does not work
      &mdash; the link then runs over the serial modem port. There is <b>no</b>
      automatic switching: if ECM fails, it stays on ECM and says why. A
      silent switch would mean the camera runs on a path nobody chose.</p>
</div></div></div>

<div class="col-12 col-lg-6" id="cell-apncard"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Credentials</h3><span class="mj-live-rule"></span></div>
    <div class="grid">
      <div><label class="form-label" for="cellpreset">Preset</label>
        <select class="form-select form-select-sm" id="cellpreset"><option value="">Custom</option></select></div>
      <div><label class="form-label" for="cellapn">APN</label><input class="form-control form-control-sm" id="cellapn" placeholder="internet.t-d1.de"></div>
      <div><label class="form-label" for="cellpdp">PDP type</label>
        <select class="form-select form-select-sm" id="cellpdp"><option value="IP">IPv4</option><option value="IPV4V6">IPv4/IPv6</option></select></div>
      <div><label class="form-label" for="cellauth">Authentication</label>
        <select class="form-select form-select-sm" id="cellauth"><option value="none">none</option><option value="pap">PAP</option><option value="chap">CHAP</option></select></div>
      <div><label class="form-label" for="celluser">Username</label><input class="form-control form-control-sm" id="celluser" autocomplete="off"></div>
      <div><label class="form-label" for="cellpw">Password</label><input class="form-control form-control-sm" id="cellpw" type="password" autocomplete="new-password" placeholder="unchanged"></div>
    </div>
    <label><input type="checkbox" id="cellauto" style="width:auto"> Modem auto-connect (persistent)</label>
    <label><input type="checkbox" id="cellnic" style="width:auto"> NIC mode: public address directly on the host</label>
    <p class="mj-card-note">A preset only fills the fields &mdash; they stay editable. The password
      field is empty because stored secrets are not returned; leaving it
      empty means &ldquo;unchanged&rdquo;.</p>
    <button class="btn btn-sm btn-primary" id="cellsave">Apply (with confirmation window)</button>
    <div class="alert py-2" id="m-cell" hidden></div>
</div></div></div>

<div class="col-12 col-lg-6" id="cell-simcard"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">SIM</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="cellsim"><tbody></tbody></table>
    <div class="grid">
      <div><label class="form-label" for="cellpin">SIM-PIN</label><input class="form-control form-control-sm" id="cellpin" type="password" autocomplete="new-password" placeholder="unchanged"></div>
    </div>
    <label><input type="checkbox" id="cellpinclear" style="width:auto"> Clear the stored PIN</label>
    <button class="btn btn-sm btn-primary" id="cellpinsave">Save PIN (with confirmation window)</button>
    <div class="alert py-2" id="m-cellpin" hidden></div>
    <p class="mj-card-note">A configured PIN is sent <b>at most once</b> per boot. Three wrong
      attempts lock the card and then the PUK is needed &mdash; so nothing
      here retries, not even a button press. A changed PIN may try once
      again.</p>
</div></div></div>

<div class="col-12 col-lg-6" id="cell-diagcard"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Diagnostics</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="celldiag"><tbody></tbody></table>
    <p class="mj-card-note">Read-only. There is deliberately no free AT console here: one wrong
      command permanently reconfigures the modem's USB composition.</p>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Order and failover</h3><span class="mj-live-rule"></span></div>
    <label class="form-label" for="order">Order (highest priority first, comma separated)</label>
    <input class="form-control form-control-sm" id="order" placeholder="ethernet,wifi,cellular">
    <p class="mj-card-note">Each entry is an uplink id (&ldquo;wlan0&rdquo;, &ldquo;lte1&rdquo;) or a
    type (&ldquo;ethernet&rdquo;, &ldquo;wifi&rdquo;, &ldquo;cellular&rdquo;).
    An id beats a type &mdash; with two modems, &ldquo;cellular&rdquo; could
    not say which one.</p>
    <label><input type="checkbox" id="failover" style="width:auto"> Automatic failover</label>
    <label><input type="checkbox" id="prefer" style="width:auto"> Return to the preferred uplink as soon as it is back</label>
    <label><input type="checkbox" id="pinned" style="width:auto"> Pin to exactly one uplink</label>
    <input class="form-control form-control-sm" id="pinnedid" placeholder="e.g. wlan0">
    <p class="mj-card-note">Pinned means the uplink is used even while it is down. That is intended:
    silently falling back to another one would make this page a lie.</p>
    <button class="btn btn-sm btn-primary" id="savepolicy">Save</button>
    <div class="alert py-2" id="m-policy" hidden></div>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">USB host</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="usbhost"><tbody></tbody></table>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">Port power</h3><span class="mj-live-rule"></span></div>
    <div id="usb-unavail" class="unavail" hidden></div>
    <div id="usb-form">
      <label><input type="checkbox" id="usben" style="width:auto"> Port mit Strom versorgen</label>
      <label><input type="checkbox" id="usbboot" style="width:auto"> Switch on automatically at boot</label>
      <div id="usbexpertbox">
        <label><input type="checkbox" id="usbexpert" style="width:auto"> Expert mode: allow any pin</label>
        <p class="mj-card-note">Without expert mode, only pins the board profile lists as wired for
        this purpose are selectable. The remaining
        GPIO-Bereich ist auf diesem Board der Sensor-Reset, der PHY-Reset und
        der Flash.</p>
        <div class="grid">
          <div><label class="form-label" for="usbpin">Pin</label><input class="form-control form-control-sm" id="usbpin" placeholder="Board default"></div>
          <div><label class="form-label" for="usblvl">Aktiver Pegel</label>
            <select class="form-select form-select-sm" id="usblvl"><option value="high">high</option><option value="low">low</option></select></div>
        </div>
      </div>
      <button class="btn btn-sm btn-primary" id="usbsave">Apply</button>
      <div class="alert py-2" id="m-usb" hidden></div>
    </div>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
    <div class="mj-live-head"><h3 class="mj-cap">USB role</h3><span class="mj-live-rule"></span></div>
    <p class="mj-card-note">The camera has <b>one</b> USB port. It carries nothing, a Wi-Fi module
      or a 4G modem &mdash; not two things.</p>
    <label><input type="radio" name="usbmode" value="off" style="width:auto"> Disabled</label>
    <label><input type="radio" name="usbmode" value="wifi" style="width:auto"> Wi-Fi</label>
    <label><input type="radio" name="usbmode" value="cellular" style="width:auto"> Cellular (4G)</label>
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
    <div class="mj-live-head"><h3 class="mj-cap">Connected devices</h3><span class="mj-live-rule"></span></div>
    <table class="table table-sm" id="usbdev"><tbody></tbody></table>
    <p class="mj-card-note">USB is not part of the media lifecycle. A device that appears,
    disappears or fails cannot touch the video path; the camera keeps
    streaming no matter what happens on the port.</p>
</div></div></div>

</div>
<script>
// Alles hier ist LOKAL (IIFE). Der Grund ist konkret: die Kopfleiste
// laedt /a/main.js der Kamera-WebUI nach, und das deklariert global
// `function $`. Stand hier ein globales `const $`, starb main.js beim
// Parsen ("Identifier '$' has already been declared") -- und mit ihm
// jedes Dropdown der Leiste. Gemessen am 2026-09-25.
(function () {

"use strict";
const $ = (id) => document.getElementById(id);

// Every request goes through here so a 401 does one thing everywhere: send the
// browser to the login page. Scattering that decision is how half a UI ends up
// silently showing stale data to a signed-out user.
async function api(method, path, body) {
  const r = await fetch(path, {
    method,
    headers: body === undefined ? {} : {"Content-Type": "application/json"},
    body: body === undefined ? undefined : JSON.stringify(body),
    credentials: "same-origin"
  });
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-network.cgi"; throw new Error("unauthorized"); }
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

// ---------------------------------------------------------------- tabs

// ------------------------------------------------------------- network
let LAST_NET = null;

async function loadNetwork() {
  const res = await api("GET", "/api/v1/network");
  if (res.status !== 200) {
    $("active").textContent = "Network management is not available in this build";
    return;
  }
  const n = res.body; LAST_NET = n;
  $("active").textContent = "Active: " + (n.activeUplink || "–");

  const tb = $("uplinks").querySelector("tbody"); tb.textContent = "";
  (n.uplinks || []).forEach((u) => {
    const m = u.metrics || {};
    const box = document.createElement("div");
    box.appendChild(pill(u.state, stateKind(u.state)));
    if (u.active) box.appendChild(pill("aktiv", "ok"));
    // "connected" and "has internet" are different answers on purpose: a
    // camera on a WLAN with no uplink is connected and useless.
    if (u.state === "connected" && !u.internet) box.appendChild(pill("no internet", "warn"));
    const extra = document.createElement("div");
    extra.className = "note";
    extra.textContent = [u.interface, u.ipv4, u.gateway ? "GW " + u.gateway : ""].filter(Boolean).join(" · ");
    box.appendChild(extra);
    row(tb, u.id + " (" + u.type + ")", box);
  });
  if (!(n.uplinks || []).length) row(tb, "–", "no uplinks registered");

  const eth = (n.uplinks || []).find((u) => u.type === "ethernet");
  const etb = $("eth").querySelector("tbody"); etb.textContent = "";
  if (eth) {
    const m = eth.metrics || {};
    row(etb, "Zustand", pill(eth.state, stateKind(eth.state)));
    row(etb, "Schnittstelle", eth.interface);
    row(etb, "IPv4", eth.ipv4);
    row(etb, "Netmask", eth.netmask);
    row(etb, "Gateway", eth.gateway);
    row(etb, "DNS", eth.dns);
    row(etb, "Link", m.linkMbit ? m.linkMbit + " Mbit/s" : "–");
    row(etb, "Received / sent", (m.rxBytes || 0) + " / " + (m.txBytes || 0) + " B");
  } else { row(etb, "–", "no ethernet uplink registered"); }

  const p = n.policy || {};
  $("order").value = (p.order || []).join(",");
  $("failover").checked = !!p.autoFailover;
  $("prefer").checked = !!p.returnToPreferred;
  $("pinned").checked = !!p.pinned;
  $("pinnedid").value = p.pinnedUplink || "";

  showPending(n.change);
}

async function loadWifi() {
  const res = await api("GET", "/api/v1/network/wifi");
  const tb = $("wifistat").querySelector("tbody"); tb.textContent = "";
  if (res.status !== 200) {
    row(tb, "–", "Wi-Fi is not available in this build");
    $("c-station").hidden = true; $("c-ap").hidden = true;
    return;
  }
  const w = res.body, c = w.capabilities || {};
  // Ein abgeschaltetes WLAN sieht von hier aus genau wie ein fehlendes
  // Funkmodul aus -- kein wlan0, keine Faehigkeiten. Der Unterschied ist fuer
  // den Bedienenden aber alles: das eine ist ein Haken, den er selbst gesetzt
  // hat, das andere ein Hardwareproblem. Also wird er benannt.
  if (!c.present && modeSaved !== null && modeSaved !== "wifi") {
    row(tb, "Radio", "aus (USB-Nutzung steht auf „"
        + (MODE_LABEL[modeSaved] || modeSaved) + "\")");
    $("c-station").hidden = true; $("c-ap").hidden = true;
    return;
  }
  row(tb, "Radio", c.present ? (c.driver || "present") : "not present");
  row(tb, "Schnittstelle", c.interface);
  row(tb, "Modus", w.mode);
  row(tb, "Zustand", pill(w.state, stateKind(w.state)));
  if (w.connected) {
    row(tb, "Verbunden mit", w.connected.ssid);
    row(tb, "Sicherheit", w.connected.security);
    row(tb, "Channel / signal", (w.connected.channel || "–") + " / " + (w.connected.rssiDbm || "–") + " dBm");
  }

  // Capabilities, not guesses: the reason a control is missing is shown next
  // to where it would have been.
  const sta = c.usable && c.usable.station;
  $("c-station").hidden = false;
  $("join").disabled = !sta;
  $("scan").disabled = !(c.usable && c.usable.scan);
  if (!sta) msg($("m-station"), "Client mode is not usable: " +
      (c.present ? "wpa_supplicant is not reachable" : "no radio present"), "");

  // Three states, not two. "accessPoint" means we may OFFER the attempt;
  // "accessPointVerified" means the driver is known to do it. An unverified
  // board still gets the form -- refusing every radio nobody has interrogated
  // would make the feature unreachable on all of them -- but it is labelled
  // as unverified rather than presented as a capability.
  const ap = c.usable && c.usable.accessPoint;
  const apSure = c.usable && c.usable.accessPointVerified;
  $("ap-form").hidden = !ap;
  $("ap-unavail").hidden = !!ap;
  $("ap-unavail").textContent = c.apUnavailableReason || "Access-point mode is not available.";
  $("ap-unverified").hidden = !(ap && !apSure);
}

// Was zuletzt GESPEICHERT war, nicht was im Kasten steht. Ohne den
// Unterschied kann die Seite nicht sagen, ob der Neustart noch aussteht --
// und "Reboot required" dauerhaft anzuzeigen waere genauso falsch wie
// es nie anzuzeigen.
let modeSaved = null;      // was in der Konfiguration steht
let modeBooted = null;     // was beim Start tatsaechlich geladen wurde

const MODE_LABEL = {off: "Disabled", wifi: "WLAN", cellular: "Cellular (4G)"};

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
  row(tb, "Host operation", c.hostSupported ? (u.hostActive ? "aktiv" : "supported, not active") : "not supported");
  row(tb, "Controller", c.controller);
  row(tb, "Top speed", c.maxSpeed);
  row(tb, "Port power switchable", cp.switchable ? "ja" : "nein");
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
    const n2 = document.createElement("div"); n2.className = "note";
    n2.textContent = [d.product, d.manufacturer, d.speed, d.maxPowerMa ? d.maxPowerMa + " mA" : ""]
        .filter(Boolean).join(" · ");
    box.appendChild(n2);
    (d.interfaces || []).forEach((i) => {
      const li = document.createElement("div"); li.className = "note";
      li.textContent = "Interface class " + i["class"] + (i.driver ? " · driver " + i.driver : " · no driver");
      box.appendChild(li);
    });
    row(dtb, d.path || d.product || "Device", box);
  });
}

// --------------------------------------------- the confirmation window
let TOKEN = null, DEADLINE = 0, TIMER = null;

function showPending(change) {
  if (!change || !change.pending) {
    TOKEN = null; $("pending").hidden = true;
    if (TIMER) { clearInterval(TIMER); TIMER = null; }
    return;
  }
  TOKEN = change.token;
  DEADLINE = Date.now() + (change.remaining_ms || 0);
  $("pending").hidden = false;
  if (!TIMER) TIMER = setInterval(tick, 500);
  tick();
}

function tick() {
  const left = Math.max(0, DEADLINE - Date.now());
  const s = Math.ceil(left / 1000);
  $("count").textContent = s > 0 ? (s + " s") : "jedem Moment";
  if (left <= 0) {
    // The countdown reaching zero does NOT mean it was rolled back -- only the
    // camera knows that, and it may be unreachable. Ask; do not assume.
    clearInterval(TIMER); TIMER = null;
    refresh();
  }
}

$("confirm").onclick = async () => {
  if (!TOKEN) return;
  const res = await api("POST", "/api/v1/network/change/" + TOKEN + "/confirm");
  if (res.status !== 200) { alert("Confirmation failed: " + reason(res)); }
  await refresh();
};

// ------------------------------------------------------------- actions
$("scan").onclick = async () => {
  const b = $("scan"); b.disabled = true; b.textContent = "suche …";
  const res = await api("POST", "/api/v1/network/wifi/scan");
  b.disabled = false; b.textContent = "Netzwerke suchen";
  const tb = $("scanres").querySelector("tbody"); tb.textContent = "";
  if (res.status !== 200) { msg($("m-station"), reason(res, "Scan failed"), "bad"); return; }
  msg($("m-station"), "", null);
  // The endpoint answers with the array itself.
  const nets = Array.isArray(res.body) ? res.body : [];
  if (!nets.length) {
    // The API answers 503 for a failed scan and 200 with an empty list for a
    // quiet band, so this really does mean "nothing on the air".
    row(tb, "–", "no networks found");
    return;
  }
  nets.forEach((n) => {
    const tr = document.createElement("tr");
    const a = document.createElement("td"); a.textContent = n.ssid || "(hidden)";
    const b2 = document.createElement("td");
    b2.textContent = [n.security, "Kanal " + n.channel, n.rssiDbm + " dBm"].join(" · ");
    tr.append(a, b2);
    tr.onclick = () => { $("ssid").value = n.ssid || ""; $("psk").focus(); };
    tb.appendChild(tr);
  });
};

$("join").onclick = async () => {
  const body = {ssid: $("ssid").value, passphrase: $("psk").value};
  const res = await api("POST", "/api/v1/network/wifi/station", body);
  if (res.status === 202) {
    // The field is cleared immediately: the page has no reason to keep a
    // passphrase once it has been handed over, and the API never gives one
    // back.
    $("psk").value = "";
    msg($("m-station"), "Applied. Please confirm above once the camera is reachable.", "ok");
    showPending({pending: true, token: res.body.token, remaining_ms: res.body.confirm_within_ms});
  } else {
    msg($("m-station"), reason(res, "Connect failed"), "bad");
  }
};

$("apstart").onclick = async () => {
  const body = {
    ssid: $("apssid").value, passphrase: $("appsk").value,
    security: $("apsec").value, channel: parseInt($("apch").value, 10) || 0,
    ip: $("apip").value, dhcpStart: $("apfrom").value, dhcpEnd: $("apto").value,
    dhcpServer: true
  };
  const res = await api("POST", "/api/v1/network/wifi/ap", body);
  if (res.status === 202) {
    $("appsk").value = "";
    msg($("m-ap"), "Access point started. Please confirm above.", "ok");
    showPending({pending: true, token: res.body.token, remaining_ms: res.body.confirm_within_ms});
  } else {
    msg($("m-ap"), reason(res, "The access point could not be started"), "bad");
  }
};

$("savepolicy").onclick = async () => {
  const order = $("order").value.split(",").map((s) => s.trim()).filter(Boolean);
  const body = {
    autoFailover: $("failover").checked,
    returnToPreferred: $("prefer").checked,
    pinned: $("pinned").checked,
    pinnedUplink: $("pinnedid").value
  };
  if (order.length) body.order = order;
  const res = await api("PATCH", "/api/v1/network/policy", body);
  if (res.status === 200) { msg($("m-policy"), "Saved.", "ok"); await loadNetwork(); }
  else msg($("m-policy"), reason(res, "Save failed"), "bad");
};

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
    await loadCellular();
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

// ------------------------------------------------------------ Mobilfunk
//
// EIN Wert wird hier nie erfunden. Was das Modem nicht geliefert hat, kommt
// als null aus der API und wird zu "–". Eine 0 stuende an derselben Stelle
// wie eine Messung, und "-0 dBm" sieht aus wie eine.
function num(v, unit) { return (v === null || v === undefined) ? "–" : (v + (unit || "")); }
function txt(v) { return (v === null || v === undefined || v === "") ? "–" : v; }

const CELL_STATE_TEXT = {
  disabled: "switched off",
  wait_device: "no modem found",
  wait_at: "The modem does not answer on the AT port",
  wait_sim: "SIM not ready",
  wait_registration: "not on the network",
  ensure_ecm_mode: "Checking the mode",
  wait_reenumeration: "The modem is restarting",
  configure_pdp: "Setting the APN",
  start_data: "Data link is being established",
  wait_netif: "waiting for the network interface",
  addressing: "waiting for an address",
  up: "connected",
  failed: "failed"
};

let cellPresets = [];
let cellPresetsLoaded = false;

async function loadCellularPresets() {
  if (cellPresetsLoaded) return;
  cellPresetsLoaded = true;                 // auch bei Fehlschlag: nicht in jedem Takt erneut
  const res = await api("GET", "/api/v1/network/cellular/presets");
  if (res.status !== 200 || !res.body || !Array.isArray(res.body.presets)) return;
  cellPresets = res.body.presets;
  const sel = $("cellpreset");
  cellPresets.forEach((p) => {
    const o = document.createElement("option");
    o.value = p.id; o.textContent = p.label + (p.note ? " — " + p.note : "");
    sel.appendChild(o);
  });
}

async function loadCellular() {
  const res = await api("GET", "/api/v1/network/cellular");
  const cards = ["cell-statuscard", "cell-linkcard", "cell-apncard", "cell-simcard", "cell-diagcard"];
  if (res.status !== 200) {
    // Kein Mobilfunk-Backend in diesem Build. Gesagt, nicht angedeutet.
    cards.forEach((id) => { $(id).hidden = true; });
    $("cell-offmode").hidden = false;
    $("cell-offmode").textContent =
      "Cellular is not available in this build.";
    return;
  }
  const c = res.body || {};

  // Der USB-Modus entscheidet, ob hier ueberhaupt etwas passieren kann.
  const usable = (modeSaved === null) || (modeSaved === "cellular");
  $("cell-offmode").hidden = usable;
  if (!usable) {
    $("cell-offmode").textContent =
      "Cellular is not enabled for USB. Select it under \"USB role\" "
      + "and reboot; the credentials stay saved.";
  }
  // Die Karten bleiben SICHTBAR, auch wenn der Modus ein anderer ist: die
  // gespeicherten Zugangsdaten sollen sich vorbereiten lassen, bevor jemand
  // umschaltet. Nur der Hinweis oben sagt, dass noch nichts davon laeuft.
  cards.forEach((id) => { $(id).hidden = false; });
  $("cell-none").hidden = true;

  await loadCellularPresets();

  // ---- Status
  const tb = $("cell").querySelector("tbody"); tb.textContent = "";
  const dl = c.dataLink || {}, ad = c.address || {}, nw = c.network || {}, rf = c.radio || {}, md = c.modem || {};
  row(tb, "Link", pill(txt(c.state), stateKind(c.state)));
  row(tb, "Explanation", CELL_STATE_TEXT[dl.state] || txt(dl.detail));
  row(tb, "Modem present", c.available ? "ja" : "nein");
  row(tb, "Vendor", txt(md.manufacturer));
  row(tb, "Modell", txt(md.model));
  row(tb, "Firmware", txt(md.firmware));
  row(tb, "Operator", txt(nw.operatorName));
  row(tb, "Registration", txt(nw.registration) + (nw.roaming ? " (Roaming)" : ""));
  row(tb, "Radio technology", txt(nw.rat));
  row(tb, "Band", num(rf.band) + (rf.bandMhz ? " (" + rf.bandMhz + " MHz)" : ""));
  row(tb, "RSRP", num(rf.rsrpDbm, " dBm"));
  row(tb, "RSRQ", num(rf.rsrqDb, " dB"));
  row(tb, "SINR", num(rf.sinrDb, " dB"));
  row(tb, "Data link", txt(dl.kind) + " / " + txt(dl.state)
      + (dl.rebootRequired ? " — selected is " + txt(dl.selected) + ", reboot required" : ""));
  row(tb, "Interface", txt(c.interface));
  row(tb, "IPv4", txt(ad.ipv4));
  row(tb, "Internet", c.internet ? "erreichbar" : "not confirmed");

  // ---- Zugangsdaten. Nur fuellen, wenn niemand gerade tippt: ein
  // 5-Sekunden-Takt, der ein Formular ueberschreibt, ist unbenutzbar.
  const cfg = c.config || {};
  if (document.activeElement && document.activeElement.closest &&
      document.activeElement.closest("#cell-apncard")) {
    /* der Benutzer ist im Formular -- nichts anfassen */
  } else {
    $("cellapn").value  = cfg.apn || "";
    $("cellpdp").value  = cfg.pdpType || "IP";
    $("cellauth").value = cfg.authMode || "none";
    $("celluser").value = cfg.username || "";
    $("cellauto").checked = !!cfg.autoConnect;
    const dlr = document.querySelector('input[name=celldl][value="' + (cfg.dataLink || "ecm") + '"]');
    if (dlr) dlr.checked = true;
    $("celldl-note").textContent = dl.rebootRequired
        ? "Saved: " + txt(dl.selected) + ". Still active: " + txt(dl.kind)
          + " — a reboot is required."
        : "Takes effect after a reboot.";
    $("cellnic").checked  = !!cfg.nicMode;
    $("cellpw").placeholder = cfg.passwordSet ? "saved — leave empty for unchanged" : "";
  }

  // ---- SIM
  const stb = $("cellsim").querySelector("tbody"); stb.textContent = "";
  const sim = c.sim || {};
  row(stb, "Status", txt(sim.state));
  row(stb, "Hinweis", txt(sim.detail));
  row(stb, "ICCID", txt(sim.iccid));
  row(stb, "IMSI", txt(sim.imsi));
  // Nur ob eine hinterlegt ist. Die PIN selbst verlaesst die Kamera nicht.
  row(stb, "PIN hinterlegt", cfg.simPinSet ? "ja" : "nein");
  $("cellpin").placeholder = cfg.simPinSet ? "saved — leave empty for unchanged" : "";

  // ---- Diagnose
  const dtb = $("celldiag").querySelector("tbody"); dtb.textContent = "";
  row(dtb, "ATI", [md.manufacturer, md.model, md.firmware].filter(Boolean).join(" / ") || "–");
  row(dtb, "IMEI", txt(md.imei));
  row(dtb, "CEREG", txt(nw.registration));
  row(dtb, "CSQ", num(rf.csq));
  row(dtb, "COPS", txt(nw.operatorName) + (nw.operatorCode ? " (" + nw.operatorCode + ")" : ""));
  row(dtb, "QNWINFO", txt(nw.rat));
  row(dtb, "QENG Zelle / TAC", txt(rf.cellId) + " / " + txt(rf.tac));
  row(dtb, "QENG EARFCN / PCI", num(rf.earfcn) + " / " + num(rf.pci));
  row(dtb, "CGPADDR", txt((c.pdp || {}).ipv4));
  row(dtb, "Interface", txt(c.interface));
  row(dtb, "Address / gateway", txt(ad.ipv4) + " / " + txt(ad.gateway));
  row(dtb, "DNS", (ad.dns && ad.dns.length) ? ad.dns.join(", ") : "–");
  row(dtb, "Address assignment", dl.nicMode ? "statisch aus CGCONTRDP (NIC-Modus)" : "DHCP (Routing-Modus)");
  row(dtb, "Failed attempts", num(dl.attempts));
  row(dtb, "Last error", txt(c.lastError));
}

$("cellpreset").onchange = () => {
  const p = cellPresets.find((x) => x.id === $("cellpreset").value);
  if (!p) return;
  // Nur ausfuellen. Die Felder bleiben aenderbar -- eine Vorlage ist ein
  // Vorschlag, keine erzwungene Anbieterkonfiguration.
  $("cellapn").value = p.apn || "";
  if (p.pdpType)  $("cellpdp").value = p.pdpType;
  if (p.authMode) $("cellauth").value = p.authMode;
};

$("cellsave").onclick = async () => {
  const body = {
    apn: $("cellapn").value.trim(),
    pdpType: $("cellpdp").value,
    authMode: $("cellauth").value,
    username: $("celluser").value,
    autoConnect: $("cellauto").checked,
    nicMode: $("cellnic").checked
  };
  // Ein leeres Passwortfeld heisst "unchanged", nicht "clear". Wer es
  // löschen will, hat dafür keinen Weg über dieses Feld -- das ist Absicht:
  // ein versehentlich geleertes Feld darf nicht das gespeicherte Geheimnis
  // mitnehmen.
  if ($("cellpw").value) body.password = $("cellpw").value;
  const res = await api("PATCH", "/api/v1/network/cellular", body);
  if (res.status === 202) {
    $("cellpw").value = "";
    msg($("m-cell"), "Applied — please confirm above or it will be rolled back.", "ok");
    await loadNetwork();
  } else {
    msg($("m-cell"), reason(res, "Save failed"), "bad");
  }
};

$("celldlsave").onclick = async () => {
  const r = document.querySelector('input[name=celldl]:checked');
  if (!r) return;
  const res = await api("PATCH", "/api/v1/network/cellular", { dataLink: r.value });
  if (res.status === 202) {
    msg($("m-celldl"), "Applied — please confirm above. Takes effect after a reboot.", "ok");
    await loadNetwork();
  } else {
    msg($("m-celldl"), reason(res, "Save failed"), "bad");
  }
};

$("cellpinsave").onclick = async () => {
  const clear = $("cellpinclear").checked;
  const pin = $("cellpin").value;
  if (!clear && !pin) { msg($("m-cellpin"), "No PIN entered.", "warn"); return; }
  const res = await api("PATCH", "/api/v1/network/cellular", { simPin: clear ? "" : pin });
  if (res.status === 202) {
    $("cellpin").value = ""; $("cellpinclear").checked = false;
    msg($("m-cellpin"), clear
        ? "PIN cleared — please confirm above."
        : "PIN saved — please confirm above. It is sent ONCE on the next attempt.", "ok");
    await loadNetwork();
  } else {
    msg($("m-cellpin"), reason(res, "Save failed"), "bad");
  }
};

// ------------------------------------------------------------- refresh
async function refresh() {
  try {
    await loadNetwork();
    // loadUsb ZUERST: es setzt modeSaved, und sowohl loadWifi als auch
    // loadCellular brauchen das, um ein abgeschaltetes Geraet von einem
    // fehlenden zu unterscheiden.
    await loadUsb();
    await loadWifi();
    await loadCellular();
  } catch (e) { /* a 401 already navigated away */ }
}


refresh();
// Polled, not pushed. The status here changes on the scale of seconds and a
// dedicated SSE stream for one page would be a second thing to keep alive
// through exactly the network changes this page makes.
setInterval(() => {
  loadNetwork().catch(() => {});
  // Der Mobilfunkstatus gehoert dazu: Registrierung, Signal und Datenlink
  // aendern sich im Sekundentakt, und eine Seite, die dafuer ein Neuladen
  // verlangt, ist waehrend eines Verbindungsaufbaus nutzlos. Das Formular
  // wird dabei nicht angefasst, solange jemand darin steht -- siehe
  // loadCellular().
  loadCellular().catch(() => {});
}, 5000);

})();
</script>

</div>
<%in p/footer.cgi %>
