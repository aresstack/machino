#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Cellular" %>
<%in p/header.cgi %>
<!-- machino-owned page (installed by machino, removed by its uninstall;
     no OpenIPC file is modified). Same pipeline as every stock page.
     The machino-managed USB 4G modem: status, credentials, SIM, diagnostics. -->
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

<div class="col-12"><div id="cell-offmode" class="unavail" hidden></div></div>

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
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-cellular.cgi"; throw new Error("unauthorized"); }
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
  row(tb, "Modem present", c.available ? "yes" : "no");
  row(tb, "Vendor", txt(md.manufacturer));
  row(tb, "Model", txt(md.model));
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
  row(tb, "Internet", c.internet ? "reachable" : "not confirmed");

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
  row(stb, "Note", txt(sim.detail));
  row(stb, "ICCID", txt(sim.iccid));
  row(stb, "IMSI", txt(sim.imsi));
  // Nur ob eine hinterlegt ist. Die PIN selbst verlaesst die Kamera nicht.
  row(stb, "PIN stored", cfg.simPinSet ? "yes" : "no");
  $("cellpin").placeholder = cfg.simPinSet ? "saved — leave empty for unchanged" : "";

  // ---- Diagnose
  const dtb = $("celldiag").querySelector("tbody"); dtb.textContent = "";
  row(dtb, "ATI", [md.manufacturer, md.model, md.firmware].filter(Boolean).join(" / ") || "–");
  row(dtb, "IMEI", txt(md.imei));
  row(dtb, "CEREG", txt(nw.registration));
  row(dtb, "CSQ", num(rf.csq));
  row(dtb, "COPS", txt(nw.operatorName) + (nw.operatorCode ? " (" + nw.operatorCode + ")" : ""));
  row(dtb, "QNWINFO", txt(nw.rat));
  row(dtb, "QENG cell / TAC", txt(rf.cellId) + " / " + txt(rf.tac));
  row(dtb, "QENG EARFCN / PCI", num(rf.earfcn) + " / " + num(rf.pci));
  row(dtb, "CGPADDR", txt((c.pdp || {}).ipv4));
  row(dtb, "Interface", txt(c.interface));
  row(dtb, "Address / gateway", txt(ad.ipv4) + " / " + txt(ad.gateway));
  row(dtb, "DNS", (ad.dns && ad.dns.length) ? ad.dns.join(", ") : "–");
  row(dtb, "Address assignment", dl.nicMode ? "static from CGCONTRDP (NIC mode)" : "DHCP (routing mode)");
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
    await loadPending();
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
    await loadPending();
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
    await loadPending();
  } else {
    msg($("m-cellpin"), reason(res, "Save failed"), "bad");
  }
};


async function refresh() {
  try { await fetchMode(); await loadCellular(); await loadPending(); } catch (e) {}
}
refresh();
setInterval(() => { loadCellular().catch(() => {}); }, 5000);

})();
</script>

</div>
<%in p/footer.cgi %>
