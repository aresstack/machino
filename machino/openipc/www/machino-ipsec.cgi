#!/usr/bin/haserl
<%in p/common.cgi %>
<%
# IPsec / IKEv2: eine machino-eigene OpenIPC-Seite (haserl, wie
# machino-cellular.cgi). Die WAHRHEIT liegt bei machinod -- diese Seite ist
# eine duenne native Oberflaeche ueber /api/v1/ipsec* (Config/Status/Connect).
# Kein zweites UI, keine OpenIPC-Datei angefasst; der Menuepunkt wird
# serverseitig neben WireGuard injiziert.
#
# Der PSK ist write-only: das Passwortfeld sendet nur, wenn etwas eingegeben
# wurde; keine API gibt den Wert je zurueck. "stored yes/no" ist die einzige
# Auskunft.
page_title="IPsec"
%>
<%in p/header.cgi %>

<div id="mch">
<style>
#mch .mj-mono { font-variant-numeric: tabular-nums; }
#mch dt { font-weight: 500; }
</style>
<div class="row g-4">

<!-- Card 1: status + actions -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">IPsec / IKEv2</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">A site-to-site IKEv2 tunnel (WeirdIKE). It runs in its own
	  process &mdash; a broken tunnel never touches the video path.</p>
	<div id="ips-msg" class="alert py-2" hidden></div>
	<dl class="row mb-0">
		<dt class="col-5">State</dt><dd class="col-7"><span id="ips-state">&ndash;</span></dd>
		<dt class="col-5">Gateway</dt><dd class="col-7 mj-mono"><span id="ips-gw">&ndash;</span></dd>
		<dt class="col-5">Underlay</dt><dd class="col-7"><span id="ips-underlay">&ndash;</span></dd>
		<dt class="col-5">Local IP</dt><dd class="col-7 mj-mono"><span id="ips-localip">&ndash;</span></dd>
		<dt class="col-5">NAT-T</dt><dd class="col-7"><span id="ips-natt">&ndash;</span></dd>
	</dl>
	<div class="mt-3 d-flex gap-2">
		<button id="ips-connect" class="btn btn-sm btn-primary" type="button">Connect</button>
		<button id="ips-disconnect" class="btn btn-sm btn-outline-secondary" type="button">Disconnect</button>
		<button id="ips-reconnect" class="btn btn-sm btn-outline-secondary" type="button">Reconnect</button>
		<button id="ips-rekey" class="btn btn-sm btn-outline-secondary" type="button" title="CREATE_CHILD_SA now (PFS proof)">Rekey</button>
		<button id="ips-rekey-ike" class="btn btn-sm btn-outline-secondary" type="button" title="IKE-SA rekey now">Rekey IKE</button>
	</div>
</div></div></div>

<!-- Card 2: basic configuration -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Configuration</h3><span class="mj-live-rule"></span></div>
	<form id="ips-form" onsubmit="return false">
	<div class="form-check mb-1">
		<input class="form-check-input" type="checkbox" id="cfg-enabled">
		<label class="form-check-label" for="cfg-enabled">Enable IPsec</label>
	</div>
	<div class="form-check mb-3">
		<input class="form-check-input" type="checkbox" id="cfg-autoconnect">
		<label class="form-check-label" for="cfg-autoconnect">Connect automatically when Machino starts</label>
	</div>
	<div class="mb-2"><label class="form-label" for="cfg-gateway">Server / gateway</label>
		<input class="form-control form-control-sm" id="cfg-gateway" placeholder="vpn.example.org"></div>
	<div class="mb-2"><label class="form-label" for="cfg-port">Port</label>
		<input class="form-control form-control-sm" id="cfg-port" type="number" min="1" max="65535" value="500"></div>
	<div class="mb-2"><label class="form-label" for="cfg-underlay">Underlay</label>
		<select class="form-select form-select-sm" id="cfg-underlay">
			<option value="auto">Automatic</option>
			<option value="ethernet">Ethernet</option>
			<option value="wifi">Wi-Fi</option>
			<option value="cellular">Cellular</option>
		</select></div>
	<div class="mb-2"><label class="form-label" for="cfg-auth">Authentication</label>
		<select class="form-select form-select-sm" id="cfg-auth">
			<option value="psk">Pre-shared key</option>
			<option value="eap-mschapv2">EAP-MSCHAPv2 (username / password)</option>
		</select></div>

	<!-- PSK -->
	<div id="auth-psk" class="mb-2"><label class="form-label" for="cfg-psk">Pre-shared key
			<span id="cfg-pskset" class="badge text-bg-secondary">unknown</span></label>
		<input class="form-control form-control-sm" id="cfg-psk" type="password"
		       autocomplete="new-password" placeholder="leave blank to keep the stored key">
		<div class="form-text">Write-only &mdash; the stored key is never shown.</div></div>

	<!-- EAP-MSCHAPv2 -->
	<div id="auth-eap" hidden>
		<div class="mb-2"><label class="form-label" for="cfg-eapuser">Username</label>
			<input class="form-control form-control-sm" id="cfg-eapuser" placeholder="user@example"></div>
		<div class="mb-2"><label class="form-label" for="cfg-eappw">Password
				<span id="cfg-eappwset" class="badge text-bg-secondary">unknown</span></label>
			<input class="form-control form-control-sm" id="cfg-eappw" type="password"
			       autocomplete="new-password" placeholder="leave blank to keep the stored password">
			<div class="form-text">Write-only &mdash; the stored password is never shown.</div></div>
		<div class="mb-2"><label class="form-label" for="cfg-trust">Trust mode</label>
			<select class="form-select form-select-sm" id="cfg-trust">
				<option value="anchor-pem">Own CA</option>
				<option value="host-store">System CA store</option>
				<option value="host-store-plus-pem">System CA store + extra certificates</option>
				<option value="none">No CA validation</option>
			</select>
			<div class="form-text" id="cfg-trust-note"></div></div>
		<div class="mb-2"><label class="form-label" for="cfg-capem">CA certificate (PEM)
				<span id="cfg-capemset" class="badge text-bg-secondary">not set</span></label>
			<textarea class="form-control form-control-sm mj-mono" id="cfg-capem" rows="3"
			          placeholder="-----BEGIN CERTIFICATE-----"></textarea></div>
		<div class="mb-2"><label class="form-label" for="cfg-extrapem">Extra chain certificates (PEM)
				<span id="cfg-extrapemset" class="badge text-bg-secondary">not set</span></label>
			<textarea class="form-control form-control-sm mj-mono" id="cfg-extrapem" rows="2"
			          placeholder="intermediate certificates that only complete a chain"></textarea>
			<div class="form-text">Server identity is the Remote identity above.</div></div>
	</div>

	<button id="cfg-save" class="btn btn-sm btn-primary" type="submit">Save</button>
	</form>
</div></div></div>

<!-- Card 3: identities + remote networks -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Identities &amp; remote networks</h3><span class="mj-live-rule"></span></div>
	<div class="mb-2"><label class="form-label" for="cfg-localid">Local identity <span class="text-secondary">(send as)</span></label>
		<div class="input-group input-group-sm">
			<select class="form-select" id="cfg-localidtype" style="max-width:11em">
				<option value="fqdn">FQDN</option>
				<option value="rfc822">E-mail (RFC822)</option>
				<option value="ipv4">IPv4 address</option>
				<option value="keyid">Key ID</option>
			</select>
			<input class="form-control" id="cfg-localid" placeholder="cam.example.org">
		</div>
		<div class="form-text">Empty = derived from the source IP. The gateway checks type <b>and</b> value.</div></div>
	<div class="mb-2"><label class="form-label" for="cfg-remoteid">Remote identity <span class="text-secondary">(expect as)</span></label>
		<div class="input-group input-group-sm">
			<select class="form-select" id="cfg-remoteidtype" style="max-width:11em">
				<option value="fqdn">FQDN</option>
				<option value="rfc822">E-mail (RFC822)</option>
				<option value="ipv4">IPv4 address</option>
				<option value="keyid">Key ID</option>
			</select>
			<input class="form-control" id="cfg-remoteid" placeholder="vpn.example.org">
		</div>
		<div class="form-text">Empty = accept whatever identity the gateway presents.</div></div>
	<div class="mb-2"><label class="form-label" for="cfg-localsubnet">Tunnel address / local subnet (this camera)</label>
		<input class="form-control form-control-sm mj-mono" id="cfg-localsubnet" placeholder="empty = assigned by the gateway">
		<div class="form-text">Leave empty to get the tunnel address from the gateway (IKEv2 Configuration Payload),
		  or set the address this camera presents, e.g. <span class="mj-mono">10.77.0.2/32</span>.</div></div>
	<div class="mb-2"><label class="form-label" for="cfg-remotesubnet">Remote networks (up to 4, comma-separated)</label>
		<input class="form-control form-control-sm mj-mono" id="cfg-remotesubnet" placeholder="10.66.0.0/24, 192.168.178.0/24"></div>
	<p class="mj-card-note">These are what the camera <b>requests</b>. The gateway may narrow
	  them; only the negotiated selectors below become routes.</p>
	<button id="cfg-save2" class="btn btn-sm btn-primary" type="button">Save</button>
</div></div></div>

<!-- Card 4: advanced algorithms (fixed, capability-based) -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Algorithms</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Allow-lists per IKEv2 transform type, the LANCOM Advanced VPN Client grid.
	  A ticked entry may be offered <b>and</b> accepted; the gateway picks. Greyed entries are not
	  implemented in this build and are refused by name &mdash; nothing is offered that cannot be kept.
	  <span class="mj-mono">*</span> = LANCOM DEFAULT profile.</p>
	<div class="row" id="cfg-algos"></div>
	<div class="form-check mt-3 mb-2">
		<input class="form-check-input" type="checkbox" id="cfg-pfs">
		<label class="form-check-label" for="cfg-pfs">Perfect Forward Secrecy on Child rekeys (modp2048)</label>
		<div class="form-text">A fresh Diffie-Hellman exchange in every CREATE_CHILD_SA. Off keeps the
		  behaviour proven in the interop CI; on matches the LANCOM / FRITZ!Box default.</div>
	</div>
	<button id="cfg-save3" class="btn btn-sm btn-primary" type="button">Save</button>
</div></div></div>

<!-- Card 4b: liveness, lifetimes, MTU -->
<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Liveness &amp; lifetimes</h3><span class="mj-live-rule"></span></div>
	<div class="form-check mb-2">
		<input class="form-check-input" type="checkbox" id="cfg-dpd">
		<label class="form-check-label" for="cfg-dpd">Dead Peer Detection (own probes; the peer's probes are always answered)</label>
	</div>
	<div class="row g-2">
		<div class="col-6 mb-2"><label class="form-label" for="cfg-dpdinterval">DPD interval (s, 0 = default)</label>
			<input class="form-control form-control-sm" id="cfg-dpdinterval" type="number" min="0" max="3600"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-dpdretries">DPD retries (0 = default 5)</label>
			<input class="form-control form-control-sm" id="cfg-dpdretries" type="number" min="0" max="20"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-nattka">NAT-T keepalive (s, 0 = default 20)</label>
			<input class="form-control form-control-sm" id="cfg-nattka" type="number" min="0" max="600"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-mtu">Tunnel MTU</label>
			<input class="form-control form-control-sm" id="cfg-mtu" type="number" min="576" max="9000"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-ikelt">IKE-SA lifetime (s, 0 = default)</label>
			<input class="form-control form-control-sm" id="cfg-ikelt" type="number" min="0" max="604800"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-childlt">Child-SA lifetime (s, 0 = default)</label>
			<input class="form-control form-control-sm" id="cfg-childlt" type="number" min="0" max="604800"></div>
		<div class="col-6 mb-2"><label class="form-label" for="cfg-childmb">Child-SA byte lifetime (MiB, 0 = none)</label>
			<input class="form-control form-control-sm" id="cfg-childmb" type="number" min="0" max="1048576"></div>
	</div>
	<button id="cfg-save4" class="btn btn-sm btn-primary" type="button">Save</button>
</div></div></div>

<!-- Card 5: tunnel test ping (AP12) -->
<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Tunnel test ping</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Sends a real ICMP echo from the camera through the tunnel interface
	  (not generic routing) &mdash; the proof that a host in the private LAN behind the gateway is
	  really reachable. Target e.g. the LAN address of your router. The dropdown keeps the last
	  targets on the camera, not in the browser.</p>
	<div class="row g-2 align-items-end">
	  <div class="col-12 col-md-5"><label class="form-label" for="ping-target">Target (IPv4 behind the gateway)</label>
	    <input class="form-control form-control-sm mj-mono" id="ping-target" list="ping-targets" placeholder="192.168.178.1" inputmode="decimal" autocomplete="off">
	    <datalist id="ping-targets"></datalist></div>
	  <div class="col-4 col-md-2"><label class="form-label" for="ping-count">Echos</label>
	    <select class="form-select form-select-sm" id="ping-count"><option value="1">1</option><option value="3" selected>3</option><option value="5">5</option></select></div>
	  <div class="col-8 col-md-5 d-flex gap-2">
	    <button id="ping-send" class="btn btn-sm btn-primary" type="button">Ping</button>
	    <button id="ping-forget" class="btn btn-sm btn-outline-secondary" type="button" title="Remove this target from the camera's list">Forget target</button>
	  </div>
	</div>
	<div id="ping-result" class="mt-2 mj-mono" hidden></div>
</div></div></div>

<!-- Card 6: diagnostics -->
<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Status &amp; diagnostics</h3><span class="mj-live-rule"></span></div>
	<div class="row">
	  <div class="col-12 col-lg-6"><table class="table table-sm mb-0"><tbody id="ips-diag-a"></tbody></table></div>
	  <div class="col-12 col-lg-6"><table class="table table-sm mb-0"><tbody id="ips-diag-b"></tbody></table></div>
	</div>
	<div class="mt-3">
	  <div class="mj-card-note">Configured vs negotiated vs installed are kept separate on purpose.</div>
	  <table class="table table-sm mb-0 mt-1"><thead><tr><th>Installed route</th><th>Source</th><th>Device</th></tr></thead>
	    <tbody id="ips-routes"></tbody></table>
	</div>
	<div id="ips-cell" class="mj-card-note mt-3" hidden>
	  Underlay is <b>cellular</b>. APN and SIM settings live on the
	  <a href="machino-cellular.cgi">Cellular</a> page &mdash; this page only shows the link.
	</div>
</div></div></div>

</div>
</div>

<script>
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
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-ipsec.cgi"; throw new Error("unauthorized"); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  return {status: r.status, body: j};
}

function msg(text, kind) {
  const el = $("ips-msg");
  el.textContent = text;
  el.className = "alert py-2 " + (kind === "ok" ? "alert-success" : kind === "bad" ? "alert-danger" : "alert-secondary");
  el.hidden = !text;
}
// A failure reply is shown verbatim -- the API answers with the concrete
// reason (AUTHENTICATION_FAILED, unsupported algorithm, cellular unavailable,
// route conflict); a generic "Error" would throw away the only useful part.
function reason(res, fallback) {
  const e = res.body && (res.body.error || res.body);
  return (e && (e.message || e.error)) || fallback || ("HTTP " + res.status);
}

function pill(text, kind) {
  const s = document.createElement("span");
  s.className = "badge " + (kind === "ok" ? "text-bg-success" : kind === "warn" ? "text-bg-warning"
                           : kind === "bad" ? "text-bg-danger" : "text-bg-secondary");
  s.textContent = text; return s;
}
function runtimeKind(s) {
  if (s === "dataPlaneUp" || s === "childEstablished") return "ok";
  if (s === "ikeConnecting" || s === "ikeEstablished" || s === "resolving"
      || s === "binding" || s === "rekeying") return "warn";
  if (s === "failed") return "bad";
  return "";
}
function row(tb, k, v) {
  const tr = document.createElement("tr");
  const th = document.createElement("th"); th.textContent = k; th.className = "col-5";
  const td = document.createElement("td");
  if (v instanceof Node) td.appendChild(v); else td.textContent = (v === "" || v == null) ? "–" : v;
  tr.append(th, td); tb.appendChild(tr);
}

function setBadge(id, present, yes) {
  const b = $(id);
  b.textContent = present ? (yes || "stored") : "not set";
  b.className = "badge " + (present ? "text-bg-success" : "text-bg-secondary");
}

function applyAuthVisibility() {
  const eap = $("cfg-auth").value === "eap-mschapv2";
  $("auth-psk").hidden = eap;
  $("auth-eap").hidden = !eap;
}

let HOST_STORE_OK = false;
function applyTrustAvailability() {
  // AP9 §4/§8: grey out host-store modes when this image has no system CA
  // store -- never a silent fallback, the operator must pick an own CA.
  const opts = $("cfg-trust").options;
  for (let i = 0; i < opts.length; i++) {
    const v = opts[i].value;
    const needsStore = (v === "host-store" || v === "host-store-plus-pem");
    opts[i].disabled = needsStore && !HOST_STORE_OK;
  }
  $("cfg-trust-note").textContent = HOST_STORE_OK
    ? "" : "No system CA store in this image — use an own CA (anchor-pem).";
}

async function loadConfig() {
  const res = await api("GET", "/api/v1/ipsec");
  if (res.status === 404) { msg("IPsec is not available on this platform.", "bad"); return; }
  if (res.status !== 200 || !res.body) { msg(reason(res, "could not read config"), "bad"); return; }
  const c = res.body;
  $("cfg-enabled").checked = !!c.enabled;
  $("cfg-gateway").value = c.gateway || "";
  $("cfg-port").value = c.port || 500;
  $("cfg-underlay").value = c.underlay || "auto";
  $("cfg-localid").value = c.localId || "";
  $("cfg-remoteid").value = c.remoteId || "";
  $("cfg-localidtype").value = c.localIdType || "fqdn";
  $("cfg-remoteidtype").value = c.remoteIdType || "fqdn";
  $("cfg-localsubnet").value = c.localSubnet || "";
  $("cfg-pfs").checked = !!c.pfs;
  $("cfg-autoconnect").checked = c.autoConnect !== false;
  renderAlgos(c.algorithms || {}, c);
  $("cfg-dpd").checked = c.dpd !== false;
  $("cfg-dpdinterval").value = c.dpdIntervalS != null ? c.dpdIntervalS : 30;
  $("cfg-dpdretries").value = c.dpdRetries || 0;
  $("cfg-nattka").value = c.nattKeepaliveS || 0;
  $("cfg-mtu").value = c.mtu || 1400;
  $("cfg-ikelt").value = c.ikeLifetimeS || 0;
  $("cfg-childlt").value = c.childLifetimeS || 0;
  $("cfg-childmb").value = c.childLifetimeMb || 0;
  $("cfg-remotesubnet").value = c.remoteSubnet || "";
  $("cfg-auth").value = c.auth || "psk";
  $("cfg-eapuser").value = c.eapUser || "";
  $("cfg-trust").value = c.trustMode || "host-store";
  HOST_STORE_OK = !!c.hostStoreAvailable;
  setBadge("cfg-pskset", c.pskSet);
  setBadge("cfg-eappwset", c.eapPasswordSet);
  setBadge("cfg-capemset", c.caPemSet);
  setBadge("cfg-extrapemset", c.extraPemSet);
  applyAuthVisibility();
  applyTrustAvailability();
}

const ALGO_GROUPS = [
  ["ikeDh",   "DH groups (IKE_SA_INIT, PFS)"],
  ["ikeEnc",  "IKE-SA encryption"],
  ["ikeHash", "IKE-SA hash (PRF + integrity)"],
  ["espEnc",  "Child-SA encryption (ESP)"],
  ["espHash", "Child-SA hash (ESP integrity)"]
];

// The grid comes from the API's catalogue: every LANCOM entry is shown, the
// ones this build cannot negotiate are disabled -- nothing here is hardcoded.
function renderAlgos(cat, c) {
  const root = $("cfg-algos"); root.replaceChildren();
  for (const [field, title] of ALGO_GROUPS) {
    const list = cat[field] || [];
    const chosen = new Set(c[field] || []);
    const col = document.createElement("div"); col.className = "col-12 col-md-6 mb-2";
    const h = document.createElement("div"); h.className = "fw-semibold small mb-1"; h.textContent = title; col.appendChild(h);
    for (const a of list) {
      const id = "alg-" + field + "-" + a.id;
      const wrap = document.createElement("div"); wrap.className = "form-check";
      const cb = document.createElement("input"); cb.type = "checkbox"; cb.className = "form-check-input";
      cb.id = id; cb.dataset.group = field; cb.dataset.algo = a.id;
      cb.checked = chosen.has(a.id); cb.disabled = !a.implemented;
      const lb = document.createElement("label"); lb.className = "form-check-label" + (a.implemented ? "" : " text-secondary");
      lb.htmlFor = id;
      lb.textContent = a.label + (a.lancomDefault ? " *" : "") + (a.implemented ? "" : " (not implemented)");
      wrap.appendChild(cb); wrap.appendChild(lb); col.appendChild(wrap);
    }
    root.appendChild(col);
  }
}

function algoList(field) {
  const out = [];
  document.querySelectorAll('#cfg-algos input[data-group="' + field + '"]').forEach(cb => { if (cb.checked) out.push(cb.dataset.algo); });
  return out;
}

function buildBody() {
  const body = {
    enabled: $("cfg-enabled").checked,
    gateway: $("cfg-gateway").value.trim(),
    port: parseInt($("cfg-port").value, 10) || 500,
    underlay: $("cfg-underlay").value,
    localId: $("cfg-localid").value.trim(),
    remoteId: $("cfg-remoteid").value.trim(),
    localIdType: $("cfg-localidtype").value,
    remoteIdType: $("cfg-remoteidtype").value,
    localSubnet: $("cfg-localsubnet").value.trim(),
    pfs: $("cfg-pfs").checked,
    autoConnect: $("cfg-autoconnect").checked,
    dpd: $("cfg-dpd").checked,
    dpdIntervalS: parseInt($("cfg-dpdinterval").value, 10) || 0,
    dpdRetries: parseInt($("cfg-dpdretries").value, 10) || 0,
    nattKeepaliveS: parseInt($("cfg-nattka").value, 10) || 0,
    mtu: parseInt($("cfg-mtu").value, 10) || 1400,
    ikeLifetimeS: parseInt($("cfg-ikelt").value, 10) || 0,
    childLifetimeS: parseInt($("cfg-childlt").value, 10) || 0,
    childLifetimeMb: parseInt($("cfg-childmb").value, 10) || 0,
    remoteSubnet: $("cfg-remotesubnet").value.trim(),
    auth: $("cfg-auth").value,
    eapUser: $("cfg-eapuser").value.trim(),
    trustMode: $("cfg-trust").value
  };
  // The algorithm allow-lists, only when the grid has been rendered (an empty
  // list would be refused by the API, never silently narrowed).
  for (const [field] of ALGO_GROUPS) { const l = algoList(field); if (l.length) body[field] = l; }
  // All secrets write-only: only send when the operator typed/pasted one.
  const psk = $("cfg-psk").value;         if (psk) body.psk = psk;
  const eappw = $("cfg-eappw").value;     if (eappw) body.eapPassword = eappw;
  const capem = $("cfg-capem").value;     if (capem.trim()) body.caPem = capem;
  const extra = $("cfg-extrapem").value;  if (extra.trim()) body.extraPem = extra;
  return body;
}

async function save() {
  const res = await api("PUT", "/api/v1/ipsec/config", buildBody());
  if (res.status === 200) {
    $("cfg-psk").value = ""; $("cfg-eappw").value = "";   // never keep secrets around
    $("cfg-capem").value = ""; $("cfg-extrapem").value = "";
    msg("Saved.", "ok");
    loadConfig();
  } else {
    msg(reason(res, "save failed"), "bad");   // e.g. "ikeEnc: ... rejected: 'chacha20'"
  }
}

async function act(path, label) {
  msg(label + "…", "");
  const res = await api("POST", path);
  if (res.status === 200) { msg(label + " ok.", "ok"); }
  else { msg(reason(res, label + " failed"), "bad"); }
  loadStatus();
}

async function loadStatus() {
  const res = await api("GET", "/api/v1/ipsec/status");
  if (res.status !== 200 || !res.body) return;
  const s = res.body;

  $("ips-state").replaceChildren(pill(s.runtimeState || s.state || "–", runtimeKind(s.runtimeState)));
  $("ips-gw").textContent = s.gateway || "–";
  let u = s.actualUnderlay || s.requestedUnderlay || "–";
  if (s.reconnectAttempt) u += " (reconnect #" + s.reconnectAttempt + ")";
  $("ips-underlay").textContent = u;
  $("ips-localip").textContent = s.underlayIpv4 || "–";
  $("ips-natt").textContent = s.natDetected ? "detected" : (s.natT ? "enabled" : "off");

  const a = $("ips-diag-a"); a.replaceChildren();
  row(a, "Runtime state", s.runtimeState || "–");
  row(a, "Authentication", s.auth || "–");
  row(a, "IKE state", s.rawState || "–");
  row(a, "IKE generation", s.ikeGeneration != null ? String(s.ikeGeneration) : "–");
  row(a, "Child generation", s.childGeneration != null ? String(s.childGeneration) : "–");
  row(a, "Peer", s.peerIpv4 || "–");
  row(a, "Actual underlay", s.actualUnderlay || "–");
  row(a, "Underlay interface", s.underlayInterface || "–");
  row(a, "Source IP", s.underlayIpv4 || "–");

  const b = $("ips-diag-b"); b.replaceChildren();
  row(b, "NAT detected", s.natDetected ? "yes" : "no");
  row(b, "IKE transport", s.ikeTransport || "–");
  row(b, "ESP transport", s.espTransport || "–");
  row(b, "Configured remote", $("cfg-remotesubnet").value || "–");
  row(b, "Negotiated TSr", s.remoteTs || "–");
  row(b, "Local TSi", s.localTs || "–");
  row(b, "Tunnel address", s.tunnelIpv4 || "–");
  if (s.requestCp !== undefined)
    row(b, "Config payload", s.requestCp ? (s.cpAddress ? "assigned " + s.cpAddress : "requested, nothing assigned") : "not requested");
  if (s.pfsGroup !== undefined) row(b, "PFS", s.pfsGroup ? "on (DH group " + s.pfsGroup + ")" : "off");
  if (s.ikeSuite) row(b, "IKE suite (negotiated)", s.ikeSuite);
  if (s.childSuite) row(b, "Child suite (negotiated)", s.childSuite);
  if (s.failure) row(b, "Last failure", pill(s.failure.code || "failed", "bad"));
  if (s.fullTunnelRefused) row(b, "Full tunnel", pill("requested, refused (unsupported)", "warn"));

  const rt = $("ips-routes"); rt.replaceChildren();
  const routes = s.routes || [];
  if (routes.length === 0) {
    const tr = document.createElement("tr");
    const td = document.createElement("td"); td.colSpan = 3; td.className = "mj-card-note";
    td.textContent = "No routes installed."; tr.appendChild(td); rt.appendChild(tr);
  } else {
    routes.forEach((r) => {
      const tr = document.createElement("tr");
      ["prefix", "source", "device"].forEach((k) => {
        const td = document.createElement("td"); td.textContent = r[k] || "–";
        if (k === "prefix") td.className = "mj-mono"; tr.appendChild(td);
      });
      rt.appendChild(tr);
    });
  }

  $("ips-cell").hidden = !((s.actualUnderlay || s.requestedUnderlay) === "cellular");
}

// AP12: the tunnel test ping. The measurement runs in machinod (raw ICMP
// bound to the tunnel interface); the target list lives on the camera.
function fillPingTargets(list) {
  const dl = $("ping-targets"); dl.replaceChildren();
  (list || []).forEach((t) => { const o = document.createElement("option"); o.value = t; dl.appendChild(o); });
  if (!$("ping-target").value && list && list.length) $("ping-target").value = list[0];
}
async function loadPingTargets() {
  const res = await api("GET", "/api/v1/ipsec/ping");
  if (res.status === 200 && res.body) fillPingTargets(res.body.targets);
}
function pingResult(text, kind) {
  const el = $("ping-result"); el.textContent = text; el.hidden = !text;
  el.className = "mt-2 mj-mono " + (kind === "ok" ? "text-success" : kind === "bad" ? "text-danger" : "text-muted");
}
const fmtMs = (v) => (typeof v === "number" ? v.toFixed(1) : "?");
async function sendPing() {
  const target = $("ping-target").value.trim();
  if (!target) { pingResult("Enter a target address.", "bad"); return; }
  const count = parseInt($("ping-count").value, 10) || 1;
  $("ping-send").disabled = true;
  pingResult("Pinging " + target + " through the tunnel\u2026", "");
  try {
    const res = await api("POST", "/api/v1/ipsec/ping", {target, count, timeoutMs: 2000});
    const b = res.body || {};
    if (res.status !== 200) { pingResult(reason(res, "ping failed"), "bad"); return; }
    if (b.targets) fillPingTargets(b.targets);
    if (b.ok) {
      pingResult("OK \u2013 " + b.received + "/" + b.sent + " replies via " + (b.via || "tunnel") +
                 ", RTT " + fmtMs(b.rttMs) + " ms (min " + fmtMs(b.rttMinMs) + ", max " + fmtMs(b.rttMaxMs) + ")", "ok");
    } else {
      pingResult("No reply \u2013 " + b.received + "/" + b.sent + " via " + (b.via || "tunnel") + (b.error ? ": " + b.error : ""), "bad");
    }
  } finally { $("ping-send").disabled = false; }
}
async function forgetPingTarget() {
  const target = $("ping-target").value.trim();
  if (!target) return;
  const res = await api("POST", "/api/v1/ipsec/ping/forget", {target});
  if (res.status === 200 && res.body) { fillPingTargets(res.body.targets); $("ping-target").value = ""; pingResult("Forgot " + target + ".", ""); }
  else pingResult(reason(res, "could not forget the target"), "bad");
}
$("ping-send").addEventListener("click", sendPing);
$("ping-forget").addEventListener("click", forgetPingTarget);
$("ping-target").addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); sendPing(); } });

$("cfg-save").addEventListener("click", save);
$("cfg-save2").addEventListener("click", save);
$("cfg-save3").addEventListener("click", save);
$("cfg-save4").addEventListener("click", save);
$("ips-rekey").addEventListener("click", () => act("/api/v1/ipsec/rekey", "Rekey"));
$("ips-rekey-ike").addEventListener("click", () => act("/api/v1/ipsec/rekey-ike", "IKE rekey"));
$("cfg-auth").addEventListener("change", applyAuthVisibility);
$("ips-connect").addEventListener("click", () => act("/api/v1/ipsec/connect", "Connect"));
$("ips-disconnect").addEventListener("click", () => act("/api/v1/ipsec/disconnect", "Disconnect"));
$("ips-reconnect").addEventListener("click", async () => {
  msg("Reconnecting…", "");
  await api("POST", "/api/v1/ipsec/disconnect");
  const res = await api("POST", "/api/v1/ipsec/connect");
  if (res.status === 200) msg("Reconnect ok.", "ok"); else msg(reason(res, "reconnect failed"), "bad");
  loadStatus();
});

loadConfig();
loadStatus();
loadPingTargets();
setInterval(loadStatus, 3000);
})();
</script>

<%in p/footer.cgi %>
