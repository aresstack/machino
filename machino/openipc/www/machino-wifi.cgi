#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Wi-Fi" %>
<%in p/header.cgi %>
<!-- machino-owned page (installed by machino, removed by its uninstall;
     no OpenIPC file is modified). Same pipeline as every stock page.
     The machino-managed USB Wi-Fi module: status, client, access point. -->
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
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-wifi.cgi"; throw new Error("unauthorized"); }
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



// Der USB-Modus entscheidet, ob diese Seite ueberhaupt etwas tun kann. Er
// wohnt auf der USB-Seite; hier wird er nur GELESEN.
let modeSaved = null;
const MODE_LABEL = {off: "Disabled", wifi: "Wi-Fi", cellular: "Cellular (4G)"};
async function fetchMode() {
  const res = await api("GET", "/api/v1/usb");
  if (res.status !== 200 || !res.body) return;
  modeSaved = (res.body.config && res.body.config.mode) || res.body.mode || "off";
}

// Eine anderswo angestossene Aenderung (Bestaetigungsfrist) muss auch hier
// sichtbar sein -- sonst rollt sie zurueck, waehrend niemand den Knopf sieht.
async function loadPending() {
  const res = await api("GET", "/api/v1/network");
  if (res.status === 200 && res.body) showPending(res.body.change);
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
    row(tb, "Radio", "off (USB role is set to \""
        + (MODE_LABEL[modeSaved] || modeSaved) + "\")");
    $("c-station").hidden = true; $("c-ap").hidden = true;
    return;
  }
  row(tb, "Radio", c.present ? (c.driver || "present") : "not present");
  row(tb, "Interface", c.interface);
  row(tb, "Mode", w.mode);
  row(tb, "State", pill(w.state, stateKind(w.state)));
  if (w.connected) {
    row(tb, "Connected to", w.connected.ssid);
    row(tb, "Security", w.connected.security);
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
  $("count").textContent = s > 0 ? (s + " s") : "any moment now";
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


$("scan").onclick = async () => {
  const b = $("scan"); b.disabled = true; b.textContent = "scanning …";
  const res = await api("POST", "/api/v1/network/wifi/scan");
  b.disabled = false; b.textContent = "Scan for networks";
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
    b2.textContent = [n.security, "Channel " + n.channel, n.rssiDbm + " dBm"].join(" · ");
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


async function refresh() {
  try { await fetchMode(); await loadWifi(); await loadPending(); } catch (e) {}
}
refresh();
setInterval(() => { loadPending().catch(() => {}); }, 5000);

})();
</script>

</div>
<%in p/footer.cgi %>
