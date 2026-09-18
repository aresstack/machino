#!/usr/bin/env python3
"""WebUI simulation for the Machino HTTP API v1 (test tool, not a frontend).

Behaves like the future WebUI: discovery -> capabilities -> config/state ->
subscribe to events -> change bitrate -> change profile/fps -> verify the
reflected effective state -> check that reading the API never wakes the
pipeline. Uses only the standard library. Exit code = number of failed checks.

    python3 tools/webui_sim.py <camera-ip> [port] [--no-restart]
"""
import json, sys, threading, time, urllib.request, urllib.error

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 8080
NO_RESTART = "--no-restart" in sys.argv
BASE = "http://%s:%d/api/v1" % (HOST, PORT)
fails = 0

def check(cond, what):
    global fails
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond: fails += 1

def http(method, path, body=None, headers=None):
    req = urllib.request.Request(BASE + path, method=method, data=(json.dumps(body).encode() if body is not None else None))
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items(): req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read().decode() or "null")
        except Exception: return e.code, None

class Events(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True); self.events = []; self.stop = False
    def run(self):
        try:
            req = urllib.request.Request(BASE + "/events")
            with urllib.request.urlopen(req, timeout=60) as r:
                ev, data = None, None
                while not self.stop:
                    line = r.readline().decode("utf-8", "replace")
                    if not line: break
                    line = line.rstrip("\n")
                    if line.startswith("event:"): ev = line[6:].strip()
                    elif line.startswith("data:"): data = line[5:].strip()
                    elif line == "" and ev:
                        try: self.events.append((ev, json.loads(data)))
                        except Exception: self.events.append((ev, data))
                        ev, data = None, None
        except Exception as e:
            self.events.append(("_error", str(e)))
    def types(self): return [t for t, _ in self.events]

print("== Machino WebUI simulation against %s ==" % BASE)
st, disc = http("GET", "")
check(st == 200 and disc and disc.get("api") == 1 and "capabilities" in disc.get("endpoints", {}), "discovery")

st, caps = http("GET", "/capabilities")
check(st == 200 and caps and caps.get("api") == 1, "GET /capabilities")
ctl = (caps or {}).get("controls", {})
check(all(k in ctl for k in ("sensor_fps", "stream_fps", "bitrate", "isp_clock", "encoder_clock", "cpu_frequency")), "capabilities: all control slots present")
check(all(ctl[k].get("status") in ("supported", "unsupported", "unknown") for k in ctl), "capabilities: tri-state status")
print("      platform=%s board=%s sensor=%s modes=%s" % ((caps or {}).get("platform"), (caps or {}).get("board", {}).get("id"),
      (caps or {}).get("sensor", {}).get("model"), (caps or {}).get("sensor", {}).get("modes")))
# capability-driven UI decision: no SoC name involved
show = {k: ctl[k]["status"] == "supported" for k in ctl}
print("      UI would show: " + ", ".join(k for k, v in show.items() if v) + " | hide: " + ", ".join(k for k, v in show.items() if not v))

st, cfg = http("GET", "/config")
check(st == 200 and cfg and "revision" in cfg and "video" in cfg, "GET /config")
st, state0 = http("GET", "/state")
check(st == 200 and state0 and state0.get("lifecycle") in ("cold_idle", "active", "grace_idle", "failed", "starting", "stopping"), "GET /state")
lifecycle0 = (state0 or {}).get("lifecycle")
st, tel = http("GET", "/telemetry")
check(st == 200 and tel and "process" in tel and "power" in tel and "media" in tel, "GET /telemetry")
check(all(tel["power"].get(k) is None or isinstance(tel["power"].get(k), (int, float)) for k in ("isp_clock_hz", "encoder_clock_hz", "cpu_frequency_hz")), "telemetry: unavailable values are null, not 0")

ev = Events(); ev.start(); time.sleep(1.5)
st, state1 = http("GET", "/state")
check(state1 and state1.get("lifecycle") == lifecycle0, "reads + SSE do not change the lifecycle (%s)" % lifecycle0)

# --- bitrate change (live when supported) --------------------------------------
cur_kbps = cfg["video"]["0"]["bitrate_kbps"]; new_kbps = 1500 if cur_kbps != 1500 else 2000
rev = cfg["revision"]
st, res = http("PATCH", "/config", {"video": {"0": {"bitrate_kbps": new_kbps}}}, {"If-Match": str(rev)})
check(st == 200 and res and res.get("ok") and res["changes"][0]["path"] == "video.0.bitrate_kbps", "PATCH bitrate -> 200 ok")
ch = (res or {}).get("changes", [{}])[0]
print("      change: %s" % json.dumps(ch))
check(ch.get("requested") == new_kbps and ch.get("status") in ("applied", "stored"), "bitrate: requested reflected, status applied/stored")
check((res or {}).get("revision") == rev + 1, "revision incremented (%s -> %s)" % (rev, (res or {}).get("revision")))
st, cfg2 = http("GET", "/config")
check(cfg2 and cfg2["video"]["0"]["bitrate_kbps"] == new_kbps and cfg2["performance"]["profile"] == "custom", "config reflects the new bitrate, profile now custom")
check(cfg2 and cfg2["video"]["0"]["fps"] == cfg["video"]["0"]["fps"] and cfg2["sensor"]["fps"] == cfg["sensor"]["fps"], "partial PATCH preserved fps fields")
# stale revision
st, res2 = http("PATCH", "/config", {"video": {"0": {"bitrate_kbps": new_kbps}}}, {"If-Match": str(rev)})
check(st == 409 and res2 and res2["error"]["code"] == "conflict", "stale If-Match -> 409 conflict")
# invalid / unknown / unsupported
st, r = http("PATCH", "/config", {"sensor": {"fps": 1234}}); check(st == 422 and r["error"]["code"] == "invalid_value" and r["error"]["path"] == "sensor.fps", "sensor.fps=1234 -> 422 invalid_value (no clamp)")
st, r = http("PATCH", "/config", {"video": {"0": {"colour": 1}}}); check(st == 400 and r["error"]["code"] == "unknown_field", "unknown field -> 400")
st, r = http("PATCH", "/config", {"power": {"cpu_performance": "high"}}); check(st in (422,) and r["error"]["code"] in ("unsupported_control", "invalid_value"), "cpu_performance=high -> %d %s" % (st, r["error"]["code"]))
req = urllib.request.Request(BASE + "/config", method="PATCH", data=b"{bad json"); req.add_header("Content-Type", "application/json")
try: urllib.request.urlopen(req, timeout=10); st = 200
except urllib.error.HTTPError as e: st = e.code
check(st == 400, "malformed JSON -> 400")

# --- profile / fps (pipeline restart when running) -----------------------------
if not NO_RESTART and ctl.get("stream_fps", {}).get("status") == "supported":
    profiles = (caps or {}).get("profiles", [])
    target = "balanced" if "balanced" in profiles else "custom"
    st, res = http("PATCH", "/config", {"performance": {"profile": target}})
    check(st == 200 and res.get("ok"), "PATCH profile=%s -> 200" % target)
    if st == 200:
        print("      changes: " + json.dumps(res["changes"]))
        st, s2 = http("GET", "/state")
        check(s2 and s2["profile"] == target, "state.profile == %s" % target)
        print("      state.media = %s lifecycle=%s" % (json.dumps(s2["media"]), s2["lifecycle"]))
    # restore original bitrate/profile values
    http("PATCH", "/config", {"performance": {"profile": cfg["performance"]["profile"]}})
http("PATCH", "/config", {"video": {"0": {"bitrate_kbps": cur_kbps, "fps": cfg["video"]["0"]["fps"]}}, "sensor": {"fps": cfg["sensor"]["fps"]}})

time.sleep(2.5); ev.stop = True
types = ev.types()
print("      events received: %s" % types[:12])
check("state" in types, "SSE: initial state snapshot")
check("config_changed" in types, "SSE: config_changed event")
check("telemetry" in types, "SSE: telemetry event (1 Hz)")
st, s3 = http("GET", "/state")
print("      final lifecycle=%s revision=%s" % ((s3 or {}).get("lifecycle"), (s3 or {}).get("revision")))
print("== %d failed ==" % fails)
sys.exit(fails)
