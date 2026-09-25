#include "app/http/netui.hpp"

#include <cstring>

namespace machino { namespace http {

// One raw string literal. Kept in a single place so the page and the API it
// calls cannot drift into different versions of the same contract.
//
// The delimiter is )MACHINO_HTML rather than )" because the page contains )"
// in its JavaScript.
static const char kPage[] = R"MACHINO_HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Netzwerk &amp; USB &mdash; machino</title>
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
nav#tabs{display:flex;gap:2px;flex-wrap:wrap;padding:0 16px;border-bottom:1px solid var(--line)}
nav#tabs button{background:none;border:0;border-bottom:2px solid transparent;color:var(--dim);
padding:10px 12px;cursor:pointer;font:inherit}
nav#tabs button[aria-selected=true]{color:var(--fg);border-bottom-color:var(--acc)}
main{padding:16px;max-width:900px}
section[hidden]{display:none}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;
padding:14px;margin-bottom:14px}
.card h2{font-size:14px;margin:0 0 10px;font-weight:600}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--dim);font-weight:500;width:40%}
tr:last-child th,tr:last-child td{border-bottom:0}
.pill{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;
border:1px solid var(--line)}
.pill.ok{color:var(--ok);border-color:var(--ok)}
.pill.warn{color:var(--warn);border-color:var(--warn)}
.pill.bad{color:var(--bad);border-color:var(--bad)}
label{display:block;margin:8px 0 2px;color:var(--dim);font-size:13px}
input,select{background:#0f1114;color:var(--fg);border:1px solid var(--line);
border-radius:4px;padding:7px 8px;width:100%;font:inherit}
button.act{background:var(--acc);color:#fff;border:0;border-radius:4px;
padding:8px 14px;cursor:pointer;font:inherit;margin-top:10px}
button.act[disabled]{opacity:.45;cursor:not-allowed}
button.ghost{background:none;color:var(--fg);border:1px solid var(--line)}
.row{display:flex;gap:10px;flex-wrap:wrap}
.row>*{flex:1 1 180px}
.msg{margin-top:10px;padding:8px 10px;border-radius:4px;border:1px solid var(--line);
white-space:pre-wrap}
.msg.bad{border-color:var(--bad);color:var(--bad)}
.msg.ok{border-color:var(--ok);color:var(--ok)}
.note{color:var(--dim);font-size:13px;margin:6px 0 0}
.scan{width:100%;margin-top:8px}
.scan td{cursor:pointer}
.scan tr:hover td{background:#22272e}
#pending{position:sticky;top:0;z-index:5;background:#3a2c14;border:1px solid var(--warn);
border-radius:6px;padding:12px;margin:0 16px 14px}
#pending[hidden]{display:none}
#count{font-variant-numeric:tabular-nums;font-weight:600}
.unavail{color:var(--dim);font-style:italic}
</style>
</head>
<body>
<header>
  <h1>Netzwerk &amp; USB</h1>
  <span class="note" id="active"></span>
</header>

<!-- The confirmation banner is OUTSIDE the tabs and sticky: a change that will
     roll itself back must be visible no matter which tab the user wandered
     onto, and it is the one thing on this page that is time-critical. -->
<div id="pending" hidden>
  <strong>Änderung wartet auf Bestätigung.</strong>
  Wird in <span id="count">–</span> automatisch zurückgerollt, wenn sie nicht
  bestätigt wird. Bestätige erst, wenn die Kamera über die neue Konfiguration
  noch erreichbar ist &mdash; genau dafür ist die Frist da.
  <div class="row" style="margin-top:8px">
    <button class="act" id="confirm">Jetzt bestätigen</button>
  </div>
</div>

<nav id="tabs"></nav>
<main>

<section id="t-overview">
  <div class="card">
    <h2>Uplinks</h2>
    <table id="uplinks"><tbody></tbody></table>
    <p class="note">Ein Access Point ist kein Uplink. Eine Kamera, die ihr
    eigenes WLAN bereitstellt, arbeitet wie vorgesehen und hat
    absichtsgemäß keine Internetverbindung.</p>
  </div>
</section>

<section id="t-ethernet" hidden>
  <div class="card">
    <h2>Ethernet</h2>
    <table id="eth"><tbody></tbody></table>
    <p class="note">Ethernet wird nicht von hier aus konfiguriert. Adresse und
    Route gehören den Boot-Skripten bzw. udhcpc; diese Seite berichtet nur.
    Ein &bdquo;Trennen&ldquo; gibt es absichtlich nicht: über eth0 ist die
    Kamera erreichbar.</p>
  </div>
</section>

<section id="t-wifi" hidden>
  <div class="card">
    <h2>WLAN-Status</h2>
    <table id="wifistat"><tbody></tbody></table>
  </div>
  <div class="card" id="c-station">
    <h2>Als Client verbinden</h2>
    <button class="act ghost" id="scan">Netzwerke suchen</button>
    <table class="scan" id="scanres"><tbody></tbody></table>
    <label for="ssid">SSID</label><input id="ssid" maxlength="32" autocomplete="off">
    <label for="psk">Passwort</label>
    <input id="psk" type="password" maxlength="64" autocomplete="new-password">
    <p class="note">8 bis 63 Zeichen, oder genau 64 Hex-Zeichen für einen
    rohen PSK. Leer lassen für ein offenes Netz. Das Passwort wird nie
    zurückgeliefert &mdash; auch nicht an diese Seite.</p>
    <button class="act" id="join">Verbinden (mit Bestätigungsfrist)</button>
    <div class="msg" id="m-station" hidden></div>
  </div>
  <div class="card" id="c-ap">
    <h2>Eigenes WLAN bereitstellen</h2>
    <div id="ap-unavail" class="unavail" hidden></div>
    <p class="note" id="ap-unverified" hidden>Ob dieses Funkmodul den
    Access-Point-Modus beherrscht, wurde nicht abgefragt — dieser Build stellt
    keine nl80211-Anfrage. Der Versuch ist erlaubt; scheitert er, meldet
    hostapd den Grund, statt dass hier etwas versprochen wird.</p>
    <div id="ap-form">
      <div class="row">
        <div><label for="apssid">SSID</label><input id="apssid" maxlength="32"></div>
        <div><label for="apsec">Sicherheit</label>
          <select id="apsec">
            <option value="wpa2">WPA2</option>
            <option value="wpa2-wpa3">WPA2/WPA3</option>
            <option value="wpa3">WPA3</option>
            <option value="open">offen</option>
          </select></div>
        <div><label for="apch">Kanal</label><input id="apch" type="number" min="0" max="196" value="6"></div>
      </div>
      <label for="appsk">Passwort</label>
      <input id="appsk" type="password" maxlength="63" autocomplete="new-password">
      <div class="row">
        <div><label for="apip">Adresse der Kamera</label><input id="apip" value="192.168.4.1"></div>
        <div><label for="apfrom">DHCP von</label><input id="apfrom" value="192.168.4.20"></div>
        <div><label for="apto">DHCP bis</label><input id="apto" value="192.168.4.100"></div>
      </div>
      <button class="act" id="apstart">Access Point starten (mit Bestätigungsfrist)</button>
      <div class="msg" id="m-ap" hidden></div>
    </div>
  </div>
</section>

<section id="t-cellular" hidden>
  <!-- Wenn USB nicht auf Mobilfunk steht, wird das GESAGT und nicht durch
       graue Felder angedeutet. Ein Formular, das sich speichern laesst und
       nichts bewirkt, ist schlimmer als eines, das erklaert warum. -->
  <div id="cell-offmode" class="unavail" hidden></div>

  <div class="card" id="cell-statuscard">
    <h2>Status</h2>
    <table id="cell"><tbody></tbody></table>
    <div id="cell-none" class="unavail" hidden>
      Kein Mobilfunk-Backend in diesem Build. Das wird hier gesagt, statt
      Bedienelemente anzubieten, die nichts tun.
    </div>
  </div>

  <div class="card" id="cell-linkcard">
    <h2>Datenverbindung</h2>
    <label><input type="radio" name="celldl" value="ecm" style="width:auto"> ECM (Standard)</label>
    <label><input type="radio" name="celldl" value="ppp" style="width:auto"> PPP</label>
    <p class="note" id="celldl-note">Änderung wird nach einem Neustart wirksam.</p>
    <button class="act" id="celldlsave">Übernehmen (mit Bestätigungsfrist)</button>
    <div class="msg" id="m-celldl" hidden></div>
    <p class="note">ECM ist der normale Weg: das Modem meldet sich als
      Netzwerkkarte. PPP ist die Ausweichmöglichkeit für Modems oder Netze, in
      denen das nicht geht &mdash; die Strecke läuft dann über den seriellen
      Modem-Port. Es wird <b>nicht</b> automatisch gewechselt: scheitert ECM,
      bleibt es bei ECM und sagt warum. Ein stiller Wechsel hieße, dass die
      Kamera auf einem Weg läuft, den niemand gewählt hat.</p>
  </div>

  <div class="card" id="cell-apncard">
    <h2>Zugangsdaten</h2>
    <div class="grid">
      <div><label for="cellpreset">Vorlage</label>
        <select id="cellpreset"><option value="">Benutzerdefiniert</option></select></div>
      <div><label for="cellapn">APN</label><input id="cellapn" placeholder="internet.t-d1.de"></div>
      <div><label for="cellpdp">PDP-Typ</label>
        <select id="cellpdp"><option value="IP">IPv4</option><option value="IPV4V6">IPv4/IPv6</option></select></div>
      <div><label for="cellauth">Authentifizierung</label>
        <select id="cellauth"><option value="none">keine</option><option value="pap">PAP</option><option value="chap">CHAP</option></select></div>
      <div><label for="celluser">Benutzername</label><input id="celluser" autocomplete="off"></div>
      <div><label for="cellpw">Passwort</label><input id="cellpw" type="password" autocomplete="new-password" placeholder="unverändert"></div>
    </div>
    <label><input type="checkbox" id="cellauto" style="width:auto"> Selbstverbindung im Modem (persistent)</label>
    <label><input type="checkbox" id="cellnic" style="width:auto"> NIC-Modus: öffentliche Adresse direkt am Host</label>
    <p class="note">Eine Vorlage füllt die Felder nur aus &mdash; sie bleiben
      danach änderbar. Das Passwortfeld ist leer, weil gespeicherte Geheimnisse
      nicht zurückgegeben werden; leer lassen heißt „unverändert".</p>
    <button class="act" id="cellsave">Übernehmen (mit Bestätigungsfrist)</button>
    <div class="msg" id="m-cell" hidden></div>
  </div>

  <div class="card" id="cell-simcard">
    <h2>SIM</h2>
    <table id="cellsim"><tbody></tbody></table>
    <div class="grid">
      <div><label for="cellpin">SIM-PIN</label><input id="cellpin" type="password" autocomplete="new-password" placeholder="unverändert"></div>
    </div>
    <label><input type="checkbox" id="cellpinclear" style="width:auto"> Gespeicherte PIN löschen</label>
    <button class="act" id="cellpinsave">PIN speichern (mit Bestätigungsfrist)</button>
    <div class="msg" id="m-cellpin" hidden></div>
    <p class="note">Eine konfigurierte PIN wird pro Startvorgang <b>höchstens
      einmal</b> gesendet. Drei falsche Versuche sperren die Karte und danach
      braucht es den PUK &mdash; deshalb wiederholt hier nichts, auch kein
      Knopfdruck. Eine geänderte PIN darf wieder einmal versuchen.</p>
  </div>

  <div class="card" id="cell-diagcard">
    <h2>Diagnose</h2>
    <table id="celldiag"><tbody></tbody></table>
    <p class="note">Nur gelesen. Eine freie AT-Konsole gibt es hier bewusst
      nicht: ein falsch abgesetztes Kommando stellt die USB-Komposition des
      Modems dauerhaft um.</p>
  </div>
</section>

<section id="t-routing" hidden>
  <div class="card">
    <h2>Reihenfolge und Failover</h2>
    <label for="order">Reihenfolge (höchste Priorität zuerst, Komma getrennt)</label>
    <input id="order" placeholder="ethernet,wifi,cellular">
    <p class="note">Jeder Eintrag ist eine Uplink-ID (&bdquo;wlan0&ldquo;,
    &bdquo;lte1&ldquo;) oder ein Typ (&bdquo;ethernet&ldquo;,
    &bdquo;wifi&ldquo;, &bdquo;cellular&ldquo;). Eine ID gewinnt gegen einen
    Typ &mdash; bei zwei Modems könnte &bdquo;cellular&ldquo; sonst nicht
    sagen, welches.</p>
    <label><input type="checkbox" id="failover" style="width:auto"> Automatisches Failover</label>
    <label><input type="checkbox" id="prefer" style="width:auto"> Zurück auf den bevorzugten Uplink, sobald er wieder da ist</label>
    <label><input type="checkbox" id="pinned" style="width:auto"> Festnageln auf genau einen Uplink</label>
    <input id="pinnedid" placeholder="z. B. wlan0">
    <p class="note">Festgenagelt wird auch ein Uplink benutzt, der gerade nicht
    funktioniert. Das ist gewollt: still auf einen anderen auszuweichen würde
    diese Seite zur Lüge machen.</p>
    <button class="act" id="savepolicy">Speichern</button>
    <div class="msg" id="m-policy" hidden></div>
  </div>
</section>

<section id="t-usbhost" hidden>
  <div class="card">
    <h2>USB-Host</h2>
    <table id="usbhost"><tbody></tbody></table>
  </div>
</section>

<section id="t-usbpower" hidden>
  <div class="card">
    <h2>Stromversorgung des Ports</h2>
    <div id="usb-unavail" class="unavail" hidden></div>
    <div id="usb-form">
      <label><input type="checkbox" id="usben" style="width:auto"> Port mit Strom versorgen</label>
      <label><input type="checkbox" id="usbboot" style="width:auto"> Beim Start automatisch einschalten</label>
      <div id="usbexpertbox">
        <label><input type="checkbox" id="usbexpert" style="width:auto"> Expertenmodus: beliebigen Pin erlauben</label>
        <p class="note">Ohne Expertenmodus sind nur Pins wählbar, die das
        Boardprofil als für diesen Zweck verdrahtet ausweist. Der übrige
        GPIO-Bereich ist auf diesem Board der Sensor-Reset, der PHY-Reset und
        der Flash.</p>
        <div class="row">
          <div><label for="usbpin">Pin</label><input id="usbpin" placeholder="Board-Default"></div>
          <div><label for="usblvl">Aktiver Pegel</label>
            <select id="usblvl"><option value="high">high</option><option value="low">low</option></select></div>
        </div>
      </div>
      <button class="act" id="usbsave">Übernehmen</button>
      <div class="msg" id="m-usb" hidden></div>
    </div>
  </div>
</section>

<section id="t-usbmode" hidden>
  <div class="card">
    <h2>USB-Nutzung</h2>
    <p class="note">Die Kamera hat <b>einen</b> USB-Port. Er trägt entweder
      nichts, ein WLAN-Modul oder ein 4G-Modem &mdash; nicht zweierlei.</p>
    <label><input type="radio" name="usbmode" value="off" style="width:auto"> Deaktiviert</label>
    <label><input type="radio" name="usbmode" value="wifi" style="width:auto"> WLAN</label>
    <label><input type="radio" name="usbmode" value="cellular" style="width:auto"> 4G-Mobilfunk</label>
    <p class="note" id="usbmode-note">Änderung wird nach einem Neustart wirksam.</p>
    <button class="act" id="usbmodesave">Übernehmen</button>
    <div class="msg" id="m-usbmode" hidden></div>
    <p class="note">Bei <b>Deaktiviert</b> wird beim Start kein Treiber geladen,
      der Portstrom auf PB18 bleibt unten und es läuft kein Dienst dafür. Der
      USB-Port steht dann vollständig für ein anderes Gerät zur Verfügung.
      Das ist auch der Grund für den Neustart: Kernelmodule bei laufender
      Medien-Pipeline zu tauschen wäre der unsichere Weg.</p>
  </div>
</section>

<section id="t-usbdev" hidden>
  <div class="card">
    <h2>Angeschlossene Geräte</h2>
    <table id="usbdev"><tbody></tbody></table>
    <p class="note">USB gehört nicht zum Medien-Lebenszyklus. Ein Gerät, das
    auftaucht, verschwindet oder fehlschlägt, kann den Videopfad nicht
    berühren; die Kamera streamt weiter, was auch am Port passiert.</p>
  </div>
</section>

</main>
<script>
"use strict";
const TABS = [
  ["t-overview","Übersicht"],["t-ethernet","Ethernet"],["t-wifi","WLAN"],
  ["t-cellular","Mobilfunk"],["t-routing","Routing"],
  ["t-usbhost","USB-Host"],["t-usbpower","Stromversorgung"],
  ["t-usbmode","USB-Nutzung"],["t-usbdev","Geräte"]
];
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
  if (r.status === 401) { location.href = "/login.html?next=/machino/net"; throw new Error("unauthorized"); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  return {status: r.status, body: j};
}

function msg(el, text, kind) {
  el.textContent = text;
  el.className = "msg" + (kind ? " " + kind : "");
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
  s.className = "pill" + (kind ? " " + kind : "");
  s.textContent = text; return s;
}
function stateKind(s) {
  if (s === "connected") return "ok";
  if (s === "connecting") return "warn";
  if (s === "absent") return "";
  return "bad";
}

// ---------------------------------------------------------------- tabs
function buildTabs() {
  const nav = $("tabs");
  TABS.forEach(([id, label], i) => {
    const b = document.createElement("button");
    b.textContent = label;
    b.setAttribute("aria-selected", i === 0 ? "true" : "false");
    b.onclick = () => {
      TABS.forEach(([oid]) => { $(oid).hidden = oid !== id; });
      [...nav.children].forEach((c) => c.setAttribute("aria-selected", c === b ? "true" : "false"));
      location.hash = id.slice(2);
    };
    nav.appendChild(b);
  });
  const want = location.hash.slice(1);
  const idx = TABS.findIndex(([id]) => id.slice(2) === want);
  if (idx >= 0) nav.children[idx].click();
}

// ------------------------------------------------------------- network
let LAST_NET = null;

async function loadNetwork() {
  const res = await api("GET", "/api/v1/network");
  if (res.status !== 200) {
    $("active").textContent = "Netzwerkverwaltung ist in diesem Build nicht verfügbar";
    return;
  }
  const n = res.body; LAST_NET = n;
  $("active").textContent = "Aktiv: " + (n.activeUplink || "–");

  const tb = $("uplinks").querySelector("tbody"); tb.textContent = "";
  (n.uplinks || []).forEach((u) => {
    const m = u.metrics || {};
    const box = document.createElement("div");
    box.appendChild(pill(u.state, stateKind(u.state)));
    if (u.active) box.appendChild(pill("aktiv", "ok"));
    // "connected" and "has internet" are different answers on purpose: a
    // camera on a WLAN with no uplink is connected and useless.
    if (u.state === "connected" && !u.internet) box.appendChild(pill("kein Internet", "warn"));
    const extra = document.createElement("div");
    extra.className = "note";
    extra.textContent = [u.interface, u.ipv4, u.gateway ? "GW " + u.gateway : ""].filter(Boolean).join(" · ");
    box.appendChild(extra);
    row(tb, u.id + " (" + u.type + ")", box);
  });
  if (!(n.uplinks || []).length) row(tb, "–", "keine Uplinks registriert");

  const eth = (n.uplinks || []).find((u) => u.type === "ethernet");
  const etb = $("eth").querySelector("tbody"); etb.textContent = "";
  if (eth) {
    const m = eth.metrics || {};
    row(etb, "Zustand", pill(eth.state, stateKind(eth.state)));
    row(etb, "Schnittstelle", eth.interface);
    row(etb, "IPv4", eth.ipv4);
    row(etb, "Netzmaske", eth.netmask);
    row(etb, "Gateway", eth.gateway);
    row(etb, "DNS", eth.dns);
    row(etb, "Verbindung", m.linkMbit ? m.linkMbit + " Mbit/s" : "–");
    row(etb, "Empfangen / Gesendet", (m.rxBytes || 0) + " / " + (m.txBytes || 0) + " B");
  } else { row(etb, "–", "kein Ethernet-Uplink registriert"); }

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
    row(tb, "–", "WLAN ist in diesem Build nicht verfügbar");
    $("c-station").hidden = true; $("c-ap").hidden = true;
    return;
  }
  const w = res.body, c = w.capabilities || {};
  // Ein abgeschaltetes WLAN sieht von hier aus genau wie ein fehlendes
  // Funkmodul aus -- kein wlan0, keine Faehigkeiten. Der Unterschied ist fuer
  // den Bedienenden aber alles: das eine ist ein Haken, den er selbst gesetzt
  // hat, das andere ein Hardwareproblem. Also wird er benannt.
  if (!c.present && modeSaved !== null && modeSaved !== "wifi") {
    row(tb, "Funkmodul", "aus (USB-Nutzung steht auf „"
        + (MODE_LABEL[modeSaved] || modeSaved) + "\")");
    $("c-station").hidden = true; $("c-ap").hidden = true;
    return;
  }
  row(tb, "Funkmodul", c.present ? (c.driver || "vorhanden") : "nicht vorhanden");
  row(tb, "Schnittstelle", c.interface);
  row(tb, "Modus", w.mode);
  row(tb, "Zustand", pill(w.state, stateKind(w.state)));
  if (w.connected) {
    row(tb, "Verbunden mit", w.connected.ssid);
    row(tb, "Sicherheit", w.connected.security);
    row(tb, "Kanal / Signal", (w.connected.channel || "–") + " / " + (w.connected.rssiDbm || "–") + " dBm");
  }

  // Capabilities, not guesses: the reason a control is missing is shown next
  // to where it would have been.
  const sta = c.usable && c.usable.station;
  $("c-station").hidden = false;
  $("join").disabled = !sta;
  $("scan").disabled = !(c.usable && c.usable.scan);
  if (!sta) msg($("m-station"), "Client-Modus ist nicht nutzbar: " +
      (c.present ? "wpa_supplicant ist nicht erreichbar" : "kein Funkmodul vorhanden"), "");

  // Three states, not two. "accessPoint" means we may OFFER the attempt;
  // "accessPointVerified" means the driver is known to do it. An unverified
  // board still gets the form -- refusing every radio nobody has interrogated
  // would make the feature unreachable on all of them -- but it is labelled
  // as unverified rather than presented as a capability.
  const ap = c.usable && c.usable.accessPoint;
  const apSure = c.usable && c.usable.accessPointVerified;
  $("ap-form").hidden = !ap;
  $("ap-unavail").hidden = !!ap;
  $("ap-unavail").textContent = c.apUnavailableReason || "Access-Point-Modus ist nicht verfügbar.";
  $("ap-unverified").hidden = !(ap && !apSure);
}

// Was zuletzt GESPEICHERT war, nicht was im Kasten steht. Ohne den
// Unterschied kann die Seite nicht sagen, ob der Neustart noch aussteht --
// und "Neustart erforderlich" dauerhaft anzuzeigen waere genauso falsch wie
// es nie anzuzeigen.
let modeSaved = null;      // was in der Konfiguration steht
let modeBooted = null;     // was beim Start tatsaechlich geladen wurde

const MODE_LABEL = {off: "Deaktiviert", wifi: "WLAN", cellular: "4G-Mobilfunk"};

function selectedMode() {
  const r = document.querySelector('input[name=usbmode]:checked');
  return r ? r.value : null;
}

function markModePending() {
  const n = $("usbmode-note");
  if (modeSaved === null) { n.textContent = "Änderung wird nach einem Neustart wirksam."; return; }
  const want = selectedMode();
  if (want !== modeSaved) {
    n.textContent = "Nicht gespeichert. Übernehmen, dann neu starten.";
    return;
  }
  // Drei Zustaende, nicht zwei. "Gespeichert" und "laeuft" sind hier
  // verschiedene Dinge, und sie fallen genau zwischen Speichern und Neustart
  // auseinander -- deshalb vergleicht die Seite mit dem, was der Boot-Helfer
  // wirklich gestartet hat, statt sich ein Kennzeichen zu merken, das den
  // Neustart ueberleben wuerde.
  if (modeBooted !== null && modeSaved !== modeBooted) {
    n.textContent = "Gespeichert: " + (MODE_LABEL[modeSaved] || modeSaved)
      + ". Aktiv ist noch " + (MODE_LABEL[modeBooted] || modeBooted)
      + " — ein Neustart ist erforderlich.";
    return;
  }
  n.textContent = modeSaved === "off"
    ? "Der USB-Port wird nicht benutzt: kein Treiber, kein Portstrom, kein Dienst."
    : (MODE_LABEL[modeSaved] || modeSaved) + " ist aktiv.";
}

async function loadUsb() {
  const res = await api("GET", "/api/v1/usb");
  const tb = $("usbhost").querySelector("tbody"); tb.textContent = "";
  if (res.status !== 200) {
    row(tb, "–", "USB-Host ist in diesem Build nicht verfügbar");
    $("usb-form").hidden = true;
    $("usb-unavail").hidden = false;
    $("usb-unavail").textContent = "USB-Host ist in diesem Build nicht verfügbar.";
    return;
  }
  const u = res.body;
  const c = u.capabilities || {}, cp = c.power || {};
  const cfg = u.config || {}, cfgp = cfg.power || {}, st = u.power || {};
  row(tb, "Host-Betrieb", c.hostSupported ? (u.hostActive ? "aktiv" : "unterstützt, nicht aktiv") : "nicht unterstützt");
  row(tb, "Controller", c.controller);
  row(tb, "Höchste Geschwindigkeit", c.maxSpeed);
  row(tb, "Port-Strom schaltbar", cp.switchable ? "ja" : "nein");
  if (cp.voltageMv) row(tb, "Port-Spannung", (cp.voltageMv / 1000) + " V");
  // "unknown" is a third answer and is kept as one: a hard-wired rail cannot
  // be read back, and showing "aus" there would look like a fault.
  row(tb, "Strom am Port", st.state === "on" ? "an" : st.state === "off" ? "aus" : "nicht auslesbar");
  row(tb, "Aufgelöster Modus", (st.mode || "–") + (st.pin ? " an " + st.pin : ""));
  if (cp.allowedPins && cp.allowedPins.length)
    row(tb, "Freigegebene Pins", cp.allowedPins.join(", "));

  const sw = !!cp.switchable;
  $("usb-form").hidden = !sw;
  $("usb-unavail").hidden = sw;
  $("usb-unavail").textContent = c.hostSupported
      ? "Dieses Board hat keinen schaltbaren Port-Strom; es gibt nichts einzustellen."
      : "Dieses Board hat keinen USB-Host.";
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
  if (!list.length) row(dtb, "–", "kein Gerät angeschlossen");
  list.forEach((d) => {
    const box = document.createElement("div");
    box.textContent = [d.vid, d.pid].filter(Boolean).join(":");
    const n2 = document.createElement("div"); n2.className = "note";
    n2.textContent = [d.product, d.manufacturer, d.speed, d.maxPowerMa ? d.maxPowerMa + " mA" : ""]
        .filter(Boolean).join(" · ");
    box.appendChild(n2);
    (d.interfaces || []).forEach((i) => {
      const li = document.createElement("div"); li.className = "note";
      li.textContent = "Interface class " + i["class"] + (i.driver ? " · Treiber " + i.driver : " · kein Treiber");
      box.appendChild(li);
    });
    row(dtb, d.path || d.product || "Gerät", box);
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
  if (res.status !== 200) { alert("Bestätigung fehlgeschlagen: " + reason(res)); }
  await refresh();
};

// ------------------------------------------------------------- actions
$("scan").onclick = async () => {
  const b = $("scan"); b.disabled = true; b.textContent = "suche …";
  const res = await api("POST", "/api/v1/network/wifi/scan");
  b.disabled = false; b.textContent = "Netzwerke suchen";
  const tb = $("scanres").querySelector("tbody"); tb.textContent = "";
  if (res.status !== 200) { msg($("m-station"), reason(res, "Suche fehlgeschlagen"), "bad"); return; }
  msg($("m-station"), "", null);
  // The endpoint answers with the array itself.
  const nets = Array.isArray(res.body) ? res.body : [];
  if (!nets.length) {
    // The API answers 503 for a failed scan and 200 with an empty list for a
    // quiet band, so this really does mean "nothing on the air".
    row(tb, "–", "keine Netzwerke gefunden");
    return;
  }
  nets.forEach((n) => {
    const tr = document.createElement("tr");
    const a = document.createElement("td"); a.textContent = n.ssid || "(versteckt)";
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
    msg($("m-station"), "Angewendet. Bitte oben bestätigen, sobald die Kamera erreichbar ist.", "ok");
    showPending({pending: true, token: res.body.token, remaining_ms: res.body.confirm_within_ms});
  } else {
    msg($("m-station"), reason(res, "Verbinden fehlgeschlagen"), "bad");
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
    msg($("m-ap"), "Access Point gestartet. Bitte oben bestätigen.", "ok");
    showPending({pending: true, token: res.body.token, remaining_ms: res.body.confirm_within_ms});
  } else {
    msg($("m-ap"), reason(res, "Access Point konnte nicht gestartet werden"), "bad");
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
  if (res.status === 200) { msg($("m-policy"), "Gespeichert.", "ok"); await loadNetwork(); }
  else msg($("m-policy"), reason(res, "Speichern fehlgeschlagen"), "bad");
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
    // Kein "Übernommen." allein: uebernommen ist die EINSTELLUNG, nicht der
    // Zustand des Ports. Wer hier nur Erfolg meldet, laesst jemanden auf ein
    // Modem warten, das erst nach einem Neustart existiert.
    const pending = (modeBooted !== null && modeBooted !== modeSaved);
    msg($("m-usbmode"), pending
        ? "Gespeichert. Wirksam nach einem Neustart — jetzt läuft noch "
          + (MODE_LABEL[modeBooted] || modeBooted) + "."
        : "Gespeichert.", "ok");
    await loadCellular();
  } else {
    msg($("m-usbmode"), reason(res, "Speichern fehlgeschlagen"), "bad");
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
  if (res.status === 200) { msg($("m-usb"), "Übernommen.", "ok"); await loadUsb(); }
  else msg($("m-usb"), reason(res, "Übernehmen fehlgeschlagen"), "bad");
};

// ------------------------------------------------------------ Mobilfunk
//
// EIN Wert wird hier nie erfunden. Was das Modem nicht geliefert hat, kommt
// als null aus der API und wird zu "–". Eine 0 stuende an derselben Stelle
// wie eine Messung, und "-0 dBm" sieht aus wie eine.
function num(v, unit) { return (v === null || v === undefined) ? "–" : (v + (unit || "")); }
function txt(v) { return (v === null || v === undefined || v === "") ? "–" : v; }

const CELL_STATE_TEXT = {
  disabled: "ausgeschaltet",
  wait_device: "kein Modem gefunden",
  wait_at: "Modem antwortet nicht auf dem AT-Port",
  wait_sim: "SIM nicht bereit",
  wait_registration: "nicht im Netz",
  ensure_ecm_mode: "Betriebsart wird geprüft",
  wait_reenumeration: "Modem startet neu",
  configure_pdp: "APN wird gesetzt",
  start_data: "Datenkanal wird aufgebaut",
  wait_netif: "warte auf das Netzwerkinterface",
  addressing: "warte auf eine Adresse",
  up: "verbunden",
  failed: "fehlgeschlagen"
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
      "Mobilfunk ist in diesem Build nicht verfügbar.";
    return;
  }
  const c = res.body || {};

  // Der USB-Modus entscheidet, ob hier ueberhaupt etwas passieren kann.
  const usable = (modeSaved === null) || (modeSaved === "cellular");
  $("cell-offmode").hidden = usable;
  if (!usable) {
    $("cell-offmode").textContent =
      "4G-Mobilfunk ist für USB nicht aktiviert. Unter „USB-Nutzung\" auswählen "
      + "und neu starten; die Zugangsdaten bleiben dabei gespeichert.";
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
  row(tb, "Verbindung", pill(txt(c.state), stateKind(c.state)));
  row(tb, "Erklärung", CELL_STATE_TEXT[dl.state] || txt(dl.detail));
  row(tb, "Modem vorhanden", c.available ? "ja" : "nein");
  row(tb, "Hersteller", txt(md.manufacturer));
  row(tb, "Modell", txt(md.model));
  row(tb, "Firmware", txt(md.firmware));
  row(tb, "Betreiber", txt(nw.operatorName));
  row(tb, "Registrierung", txt(nw.registration) + (nw.roaming ? " (Roaming)" : ""));
  row(tb, "Funktechnik", txt(nw.rat));
  row(tb, "Band", num(rf.band) + (rf.bandMhz ? " (" + rf.bandMhz + " MHz)" : ""));
  row(tb, "RSRP", num(rf.rsrpDbm, " dBm"));
  row(tb, "RSRQ", num(rf.rsrqDb, " dB"));
  row(tb, "SINR", num(rf.sinrDb, " dB"));
  row(tb, "Datenlink", txt(dl.kind) + " / " + txt(dl.state)
      + (dl.rebootRequired ? " — gewählt ist " + txt(dl.selected) + ", Neustart erforderlich" : ""));
  row(tb, "Interface", txt(c.interface));
  row(tb, "IPv4", txt(ad.ipv4));
  row(tb, "Internet", c.internet ? "erreichbar" : "nicht bestätigt");

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
        ? "Gespeichert: " + txt(dl.selected) + ". Aktiv ist noch " + txt(dl.kind)
          + " — ein Neustart ist erforderlich."
        : "Änderung wird nach einem Neustart wirksam.";
    $("cellnic").checked  = !!cfg.nicMode;
    $("cellpw").placeholder = cfg.passwordSet ? "gespeichert — leer lassen für unverändert" : "";
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
  $("cellpin").placeholder = cfg.simPinSet ? "gespeichert — leer lassen für unverändert" : "";

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
  row(dtb, "Adresse / Gateway", txt(ad.ipv4) + " / " + txt(ad.gateway));
  row(dtb, "DNS", (ad.dns && ad.dns.length) ? ad.dns.join(", ") : "–");
  row(dtb, "Adressbezug", dl.nicMode ? "statisch aus CGCONTRDP (NIC-Modus)" : "DHCP (Routing-Modus)");
  row(dtb, "Fehlversuche", num(dl.attempts));
  row(dtb, "Letzter Fehler", txt(c.lastError));
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
  // Ein leeres Passwortfeld heisst "unverändert", nicht "löschen". Wer es
  // löschen will, hat dafür keinen Weg über dieses Feld -- das ist Absicht:
  // ein versehentlich geleertes Feld darf nicht das gespeicherte Geheimnis
  // mitnehmen.
  if ($("cellpw").value) body.password = $("cellpw").value;
  const res = await api("PATCH", "/api/v1/network/cellular", body);
  if (res.status === 202) {
    $("cellpw").value = "";
    msg($("m-cell"), "Übernommen — bitte oben bestätigen, sonst wird zurückgerollt.", "ok");
    await loadNetwork();
  } else {
    msg($("m-cell"), reason(res, "Speichern fehlgeschlagen"), "bad");
  }
};

$("celldlsave").onclick = async () => {
  const r = document.querySelector('input[name=celldl]:checked');
  if (!r) return;
  const res = await api("PATCH", "/api/v1/network/cellular", { dataLink: r.value });
  if (res.status === 202) {
    msg($("m-celldl"), "Übernommen — bitte oben bestätigen. Wirksam nach einem Neustart.", "ok");
    await loadNetwork();
  } else {
    msg($("m-celldl"), reason(res, "Speichern fehlgeschlagen"), "bad");
  }
};

$("cellpinsave").onclick = async () => {
  const clear = $("cellpinclear").checked;
  const pin = $("cellpin").value;
  if (!clear && !pin) { msg($("m-cellpin"), "Keine PIN eingegeben.", "warn"); return; }
  const res = await api("PATCH", "/api/v1/network/cellular", { simPin: clear ? "" : pin });
  if (res.status === 202) {
    $("cellpin").value = ""; $("cellpinclear").checked = false;
    msg($("m-cellpin"), clear
        ? "PIN gelöscht — bitte oben bestätigen."
        : "PIN gespeichert — bitte oben bestätigen. Sie wird beim nächsten Versuch EINMAL gesendet.", "ok");
    await loadNetwork();
  } else {
    msg($("m-cellpin"), reason(res, "Speichern fehlgeschlagen"), "bad");
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

buildTabs();
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
</script>
<script src="/machino/chrome.js" defer></script>
</body>
</html>
)MACHINO_HTML";

const char* machino_net_page() { return kPage; }
size_t      machino_net_page_len() { return sizeof(kPage) - 1; }

}} // namespace machino::http
