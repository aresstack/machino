#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Audio" %>
<%
# Audio: eine machino-eigene OpenIPC-Seite (haserl, wie machino-ai.cgi). Sie
# bedient machinod ueber dessen API (/api/v1/config PATCH, /api/v1/audio/tone,
# /api/v1/audio/monitor, /audio.pcm). Die eigentliche Audioverarbeitung
# (Aufnahme/Wiedergabe ueber IMP_AI/IMP_AO) gehoert machinod; der Kernel-
# treiber audio.ko und sein Ladeparameter mono_channel gehoeren OpenIPC und
# werden beim Boot gesetzt -- das steht hier ehrlich als Hinweis, nicht als
# Knopf (ein zweiter Besitzer desselben Werts war der Architekturbefund vom
# WLAN, der hier nicht wiederholt wird).

# Nur ANZEIGE: laedt OpenIPC den Codec-Treiber? Der Name in /proc/modules ist
# "audio" (insmod audio.ko). Fehlt er, liefert der Codec nichts -- dann ist es
# ein OpenIPC-/Board-Thema, kein machinod-Thema.
drv=no; grep -q '^audio ' /proc/modules 2>/dev/null && drv=yes
%>
<%in p/header.cgi %>

<div id="mch">
<div class="row g-4">

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Microphone</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">The camera's internal codec, driven by machino
	  (<code>IMP_AI</code>). Off by default. The level meter opens the microphone
	  on demand and reads the raw sample stream (<code>/audio.pcm</code>) &mdash; it
	  is the honest proof that the codec actually delivers audio on this board.</p>
	<div id="au-msg" class="alert py-2" hidden></div>
	<div class="form-check form-switch mb-3">
		<input class="form-check-input" type="checkbox" id="au-mic-en">
		<label class="form-check-label" for="au-mic-en">Microphone enabled</label>
	</div>
	<label class="form-label mb-1">Input volume <span id="au-vol-v" class="mj-card-note"></span></label>
	<input type="range" class="form-range" id="au-vol" min="0" max="100" step="5">
	<label class="form-label mb-1 mt-2">Sample rate</label>
	<select class="form-select form-select-sm" id="au-srate" style="max-width:10rem">
		<option value="8000">8000 Hz</option>
		<option value="16000">16000 Hz</option>
	</select>

	<div class="mt-3">
		<label class="form-label mb-1">Live level</label>
		<div class="progress" style="height:1.25rem">
			<div id="au-meter" class="progress-bar" role="progressbar" style="width:0%"></div>
		</div>
		<div class="d-flex justify-content-between mt-1">
			<span id="au-meter-db" class="mj-card-note">meter off</span>
			<span>
				<button class="btn btn-sm btn-outline-primary" type="button" id="au-meter-on">Start meter</button>
				<button class="btn btn-sm btn-outline-secondary" type="button" id="au-meter-off" disabled>Stop</button>
			</span>
		</div>
	</div>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Speaker</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">The talkback output (<code>IMP_AO</code>). The test tone plays a
	  short sine so you can measure or hear that the output carries audio. The board's
	  audio connector may need an external amplifier &mdash; <code>spk_gpio=-1</code> means
	  the driver was given no speaker-enable pin.</p>
	<div class="form-check form-switch mb-3">
		<input class="form-check-input" type="checkbox" id="au-spk-en">
		<label class="form-check-label" for="au-spk-en">Speaker enabled</label>
	</div>
	<label class="form-label mb-1">Output volume <span id="au-ovol-v" class="mj-card-note"></span></label>
	<input type="range" class="form-range" id="au-ovol" min="0" max="100" step="5">
	<div class="mt-3">
		<button class="btn btn-sm btn-outline-primary" type="button" id="au-tone"
		        title="Plays a 1 kHz sine for ~0.8 s. Enables the speaker first if needed.">Play test tone</button>
		<span id="au-tone-msg" class="mj-card-note" style="display:inline-block;margin-left:.5rem"></span>
	</div>
</div></div></div>

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Monitor (loopback)</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Routes the live microphone straight to the speaker so you can
	  hear what it picks up. There is <b>no echo canceller</b>: with the speaker audible near
	  the microphone this <b>will feed back</b> (a rising howl) &mdash; use headphones on the
	  output, or keep the volume low. It needs both the microphone and the speaker enabled.</p>
	<div class="form-check form-switch">
		<input class="form-check-input" type="checkbox" id="au-mon">
		<label class="form-check-label" for="au-mon">Monitor microphone on the speaker</label>
	</div>
	<div id="au-mon-msg" class="mj-card-note mt-1"></div>
</div></div></div>

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">The audio driver belongs to OpenIPC</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">machino uses the codec (<code>IMP_AI</code>/<code>IMP_AO</code>), but the
	  <b>kernel driver</b> <code>audio.ko</code> is loaded by OpenIPC at boot &mdash; including its
	  parameters. The most board-specific one is <code>mono_channel</code> (OpenIPC loads
	  <code>2</code>; stock firmware used <code>1</code>), which decides whether the microphone
	  delivers a signal at all. Flash the OpenIPC build that matches your board and this is
	  already correct &mdash; that is why it is not a setting here.</p>
	<dl class="row mb-0">
		<dt class="col-7">Codec driver loaded (audio.ko)</dt><dd class="col-5"><%= $drv %></dd>
	</dl>
	<p class="mj-card-note mb-0">If the meter above stays at the floor with the microphone
	  enabled and the driver loaded, the signal is missing before machino sees it &mdash; that
	  is an OpenIPC/board question (<code>mono_channel</code>, wiring), not a machino one.</p>
</div></div></div>

</div>
</div>

<script>
// Lokal (IIFE): header.cgi laedt /a/main.js, das global `function $` deklariert.
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
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-audio.cgi"; throw new Error("unauthorized"); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  return {status: r.status, body: j};
}
function msg(el, text, kind) {
  el.textContent = text;
  el.className = "alert py-2 " + (kind === "ok" ? "alert-success" : kind === "bad" ? "alert-danger" : "alert-secondary");
  el.hidden = !text;
}
function reason(res, fallback) {
  const e = res.body && (res.body.error || res.body);
  return (e && (e.message || e.error)) || fallback || ("HTTP " + res.status);
}

async function patchAudio(fields, okText) {
  const m = $("au-msg");
  msg(m, "applying …", "");
  const r = await api("PATCH", "/api/v1/config", {audio: fields});
  if (r.status === 200) { if (okText) msg(m, okText, "ok"); else msg(m, "", ""); await refresh(); return true; }
  msg(m, reason(r), "bad");
  await refresh();
  return false;
}

async function refresh() {
  const c = await api("GET", "/api/v1/config");
  const a = c.body && c.body.audio;
  if (!a) return;
  $("au-mic-en").checked = !!a.enabled;
  $("au-vol").value = a.volume; $("au-vol-v").textContent = a.volume + " %";
  $("au-srate").value = String(a.srate);
  $("au-spk-en").checked = !!a.output_enabled;
  $("au-ovol").value = a.output_volume; $("au-ovol-v").textContent = a.output_volume + " %";

  const t = await api("GET", "/api/v1/telemetry");
  const ta = t.body && t.body.audio;
  const mon = !!(ta && ta.monitoring);
  $("au-mon").checked = mon;
}

// ---- controls ----
$("au-mic-en").addEventListener("change", (e) => patchAudio({enabled: e.target.checked}, e.target.checked ? "microphone enabled" : "microphone disabled"));
$("au-spk-en").addEventListener("change", (e) => patchAudio({output_enabled: e.target.checked}, e.target.checked ? "speaker enabled" : "speaker disabled"));
$("au-srate").addEventListener("change", (e) => patchAudio({srate: parseInt(e.target.value, 10)}, "sample rate " + e.target.value + " Hz"));
$("au-vol").addEventListener("input", (e) => { $("au-vol-v").textContent = e.target.value + " %"; });
$("au-vol").addEventListener("change", (e) => patchAudio({volume: parseInt(e.target.value, 10)}));
$("au-ovol").addEventListener("input", (e) => { $("au-ovol-v").textContent = e.target.value + " %"; });
$("au-ovol").addEventListener("change", (e) => patchAudio({output_volume: parseInt(e.target.value, 10)}));

// ---- test tone ----
$("au-tone").addEventListener("click", async () => {
  const b = $("au-tone"), m = $("au-tone-msg");
  b.disabled = true; m.textContent = "playing …";
  try {
    if (!$("au-spk-en").checked) { if (!await patchAudio({output_enabled: true}, null)) { m.textContent = "could not enable the speaker"; return; } }
    const r = await api("POST", "/api/v1/audio/tone", {hz: 1000, ms: 800});
    m.textContent = r.status === 200 ? "played 1 kHz" : reason(r);
  } catch (e) { m.textContent = "" + e; }
  finally { b.disabled = false; }
});

// ---- monitor / loopback ----
$("au-mon").addEventListener("change", async (e) => {
  const on = e.target.checked, m = $("au-mon-msg");
  m.textContent = on ? "starting …" : "stopping …";
  const r = await api("POST", "/api/v1/audio/monitor", {on});
  if (r.status === 200) m.textContent = (r.body && r.body.monitoring) ? "monitoring (mind feedback)" : "stopped";
  else { m.textContent = reason(r); e.target.checked = !on; }
});

// ---- live level meter: read /audio.pcm, s16le mono, show dBFS ----
let meterAbort = null;
function setMeter(db) {
  const bar = $("au-meter");
  const pct = Math.max(0, Math.min(100, Math.round((db + 60) / 60 * 100)));   // -60..0 dBFS -> 0..100%
  bar.style.width = pct + "%";
  bar.className = "progress-bar" + (db > -3 ? " bg-danger" : db > -12 ? " bg-warning" : " bg-success");
  $("au-meter-db").textContent = (db <= -120 ? "silence" : db.toFixed(1) + " dBFS");
}
async function startMeter() {
  if (!$("au-mic-en").checked) { if (!await patchAudio({enabled: true}, null)) return; }
  $("au-meter-on").disabled = true; $("au-meter-off").disabled = false;
  meterAbort = new AbortController();
  try {
    const r = await fetch("/audio.pcm", {credentials: "same-origin", signal: meterAbort.signal});
    if (!r.ok || !r.body) { $("au-meter-db").textContent = "stream: HTTP " + r.status; stopMeter(); return; }
    const reader = r.body.getReader();
    let odd = null;
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      let bytes = value;
      if (odd) { const j = new Uint8Array(bytes.length + 1); j[0] = odd; j.set(bytes, 1); bytes = j; odd = null; }
      if (bytes.length & 1) { odd = bytes[bytes.length - 1]; bytes = bytes.subarray(0, bytes.length - 1); }
      const n = bytes.length >> 1, dv = new DataView(bytes.buffer, bytes.byteOffset, n * 2);
      let peak = 0;
      for (let i = 0; i < n; i++) { const s = dv.getInt16(i * 2, true), a = s < 0 ? -s : s; if (a > peak) peak = a; }
      setMeter(peak > 0 ? 20 * Math.log10(peak / 32768) : -120);
    }
  } catch (e) { if (e.name !== "AbortError") $("au-meter-db").textContent = "" + e; }
  finally { stopMeter(); }
}
function stopMeter() {
  if (meterAbort) { meterAbort.abort(); meterAbort = null; }
  $("au-meter-on").disabled = false; $("au-meter-off").disabled = true;
  $("au-meter").style.width = "0%"; $("au-meter-db").textContent = "meter off";
}
$("au-meter-on").addEventListener("click", startMeter);
$("au-meter-off").addEventListener("click", stopMeter);
window.addEventListener("beforeunload", () => { if (meterAbort) meterAbort.abort(); });

refresh().catch((e) => msg($("au-msg"), "machino API: " + e, "bad"));
})();
</script>

<%in p/footer.cgi %>
