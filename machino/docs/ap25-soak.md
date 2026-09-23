# AP25 — Performance / RAM / Long-Run-Soak

Lauf vom 2026-09-23, 02:52–04:43. **5 von 6 Zyklen** — der sechste fiel aus,
weil die Sitzung endete, nicht weil etwas abbrach. Die vorhandenen Daten
reichen für das Urteil.

Gemessen mit `tools/soak.ps1`: alles über HTTP von außen
(`/api/v1/telemetry` und `/metrics` sind native Machino-Routen, eine Probe
forkt auf der Kamera nichts), Last durch echte `/ws/video`-Consumer.

---

## Der Befund, der zuerst kommt: die COLD_IDLE-Böden steigen

Ein Leck zeigt sich nicht als große Zahl während drei Clients streamen, sondern
als **Boden, der nach jedem Zyklus höher liegt**. Die Böden, alle 19:

```
Zyklus 1   4752  4800  4716  4908      Mittel 4794 kB
Zyklus 2   4908  4816  4976  4924      Mittel 4906 kB
Zyklus 3   4924  4964  4944  4980      Mittel 4953 kB
Zyklus 4   4980  5080  5112  5080      Mittel 5063 kB
Zyklus 5   5080  5120  5108            Mittel 5103 kB

erster Boden 4752 kB  →  letzter 5108 kB   =  +356 kB in ~1 h 50 min
```

Die **Zyklusmittel steigen monoton**: +112, +47, +110, +40 kB. Das sind rund
**77 kB pro COLD_IDLE↔ACTIVE-Zyklus**, und die Kurve flacht nicht überzeugend
ab — die Deltas wechseln zwischen ~110 und ~45, was nach Rauschen um einen
konstanten Trend aussieht, nicht nach einem auslaufenden.

**Damit ist das AP25-Akzeptanzkriterium „keine linearen Leaks" auf dieser
Datenlage nicht erfüllt.**

### Korrektur an meiner eigenen früheren Aussage

Im AP29-Bericht steht, die Böden „schwanken in beide Richtungen, kein monotoner
Anstieg". Das stimmt für die **Einzelböden** — 4908 → 4816 → 4976 geht auf und
ab — und ist als Aussage über den **Trend** trotzdem falsch. Wer über Zyklen
mittelt, sieht eine gerade Linie nach oben. Ich hatte zu dem Zeitpunkt vier
Zyklen und die richtige Zahl nicht gebildet.

### Was es sein könnte, und was ich davon weiß

| | |
|---|---|
| Allokator-Retention (musl-Arenen geben nicht zurück) | plausibel; würde irgendwann ein Plateau zeigen. **Fünf Zyklen reichen nicht, um ein Plateau von einer Geraden zu unterscheiden.** |
| Echtes Leck pro Zyklus | genauso vereinbar mit den Daten |
| IMP-seitige Retention über `IMP_System_Exit` hinweg | vereinbar; die OOM-Untersuchung hat bereits gezeigt, dass ein fehlgeschlagener Init ~1,2 MB nicht zurückgibt |

Einordnung nach `evidence.md`: **NICHT ENTSCHIEDEN**. Weder „kein Leck" noch
„Leck" ist belegt.

### Größenordnung, damit die Dringlichkeit stimmt

`MemAvailable` lag am Ende bei rund 20,8 MB und ist über den ganzen Lauf nicht
gefallen. Bei 77 kB pro Zyklus wären grob **260 Zyklen** bis zur Erschöpfung.
Machino ist nachfragegesteuert: jeder Zuschauer, der sich verbindet und wieder
geht, ist ein Zyklus. Eine Kamera, die eine Weile beobachtet wird, erreicht
diese Zahl — das ist dieselbe Endstelle wie beim OOM vom 2026-09-22.

Kein Sofortproblem, aber die wichtigste offene Sache nach dem Hardlock.

---

## Was der Lauf sonst zeigt — und das ist durchweg gut

```
Phase              RSS (typ.)   MemAvail   CPU    Threads  fps     dropped
COLD_IDLE          4.8-5.1 MB   ~20.8 MB   0-1 %  4        —       0
MSE MAIN           6.5-6.8 MB   ~18.2 MB   4-12 % 12       20.01   0
MSE MAIN+SUB       6.9-7.0 MB   ~17.8 MB   7-9 %  18       20.01   0
MSE 3 Clients      6.8-7.1 MB   ~17.8 MB   9-13 % 12       20.00   0
```

* **`dropped_frames` bleibt über den ganzen Lauf 0.** Keine einzige verworfene
  Frame in ~1 h 50 min unter wechselnder Last.
* **`init_failures` und `init_retries` bleiben 0.** Die entfernte
  Fünffach-Retry-Schleife bleibt entfernt — 27 Pipeline-Generationen lang.
* **`pipeline_generation` zählt 13 → 27.** Jeder Zyklus ist ein echter
  Kaltstart der Pipeline im selben Prozess, keiner ist fehlgeschlagen.
* **Die Gauges gehen nach jeder Phase auf 0** (`ws_video_clients`,
  `rtsp_sessions`, `webrtc_sessions`). Keine Sitzung bleibt hängen.
* **fps konstant 20,00–20,01** unter einem, zwei und drei Clients.
* Ein einzelner CPU-Ausreißer auf 60,6 % in Zyklus 2 (eine Stichprobe), sonst
  unter 13 %. Eine Momentaufnahme, kein Muster.

MAIN+SUB kostet 18 Threads gegen 12 — ein zweiter Encoder mit eigenem
Capture-Thread, wie erwartet.

## Was nicht gemessen wurde

* **RTSP und WebRTC als Soak-Last.** Der Lauf fährt nur MSE-Consumer; ein
  RTSP-Client in PowerShell wäre eigener Code gewesen. Ein Leck, das nur auf
  diesen Pfaden liegt, hätte der Lauf nicht gesehen.
* **Leistungsaufnahme und Temperatur.** Kein Sensor erreichbar
  (`/sys/class/hwmon` ist leer) — `PENDING_PHYSICAL`, braucht ein Messgerät.
* **Der sechste Zyklus.** Abgebrochen durch das Sitzungsende.

## Nächster Schritt, wenn jemand die Frage schließen will

1. Denselben Lauf über **15–20 Zyklen** fahren (`-Cycles 20`). Flacht die Kurve
   ab, ist es Retention; bleibt sie gerade, ist es ein Leck.
2. Falls gerade: dieselbe Messung mit **nur** RTSP und mit **nur** WebRTC, um
   den Pfad einzugrenzen.
3. Erst dann im Code suchen. Vorher ist jede Codesuche geraten — die
   statische Paarungsprüfung aus AP29 hat nichts gefunden, also liegt es nicht
   an einer offensichtlich unpaarigen Freigabe.
