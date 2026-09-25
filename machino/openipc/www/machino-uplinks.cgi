#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Uplinks" %>
<%in p/header.cgi %>
<!-- machino-owned page (installed by machino, removed by its uninstall;
     no OpenIPC file is modified). Same pipeline as every stock page.
     Which links carry the camera, and in which order. -->
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
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-uplinks.cgi"; throw new Error("unauthorized"); }
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
    if (u.active) box.appendChild(pill("active", "ok"));
    // "connected" and "has internet" are different answers on purpose: a
    // camera on a WLAN with no uplink is connected and useless.
    if (u.state === "connected" && !u.internet) box.appendChild(pill("no internet", "warn"));
    const extra = document.createElement("div");
    extra.className = "small text-secondary";
    extra.textContent = [u.interface, u.ipv4, u.gateway ? "GW " + u.gateway : ""].filter(Boolean).join(" · ");
    box.appendChild(extra);
    row(tb, u.id + " (" + u.type + ")", box);
  });
  if (!(n.uplinks || []).length) row(tb, "–", "no uplinks registered");

  const eth = (n.uplinks || []).find((u) => u.type === "ethernet");
  const etb = $("eth").querySelector("tbody"); etb.textContent = "";
  if (eth) {
    const m = eth.metrics || {};
    row(etb, "State", pill(eth.state, stateKind(eth.state)));
    row(etb, "Interface", eth.interface);
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


async function refresh() {
  try { await loadNetwork(); } catch (e) { /* 401 hat schon navigiert */ }
}
refresh();
setInterval(() => { loadNetwork().catch(() => {}); }, 5000);

})();
</script>

</div>
<%in p/footer.cgi %>
