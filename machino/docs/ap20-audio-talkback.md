# AP20 — Audio / Talkback

Batchlauf 2026-09-23. Lesend am Gerät, nichts geöffnet, nichts bespielt.

---

## Kurzfassung

Aufnahmeseitig ist Audio auf diesem Board **plausibel möglich**: der interne
Codec ist hochgefahren, der AIC läuft, `/dev/dsp` existiert, und der SDK bringt
`IMP_AI_*` mit. Wiedergabeseitig ist es **nicht konfiguriert**: der Treiber hat
`spk_gpio = -1` und keinen externen Codec.

Machino hat **kein Audio** — weder Capture noch Ausgabe. Neu ist, dass es das
jetzt *sagt*, statt zu schweigen, und dass ein Schreibversuch eine Begründung
bekommt statt „unknown section".

---

## 1. Was die Hardware hergibt

```
lsmod                    audio  63259  0        (Ingenic-Treiber, geladen)
dmesg                    @@@@@ inner codec power up@@@@@@
                         @@@@ audio driver ok(version H20210524a) @@@@@
/dev/dsp                 char 10,45        (OSS, kein ALSA)
/proc/asound             existiert nicht
Device Tree              KEIN audio/i2s/codec/aic-Knoten
Halter von /dev/dsp      keiner
```

Der **interne Codec des T40 ist hochgefahren**. Das steht nicht im Device Tree,
weil der Vendor-Treiber sich selbst verdrahtet — deshalb war die DT-Suche
allein irreführend.

### Die Modulparameter sind die eigentliche Verdrahtungsauskunft

```
aic_enable     = 1      der Audio-Interface-Controller ist an
dmic_enable    = 0      kein digitales Mikrofon
dmic_gpio      = 2
excodec_addr   = 255    kein externer Codec adressiert
excodec_name   = ""     und keiner benannt
i2c_bus        = 1      (nur für einen externen Codec relevant)
left_mic_gain  = 3
right_mic_gain = 3
mono_channel   = 2
samplerate     = 0
spk_gpio       = -1     KEIN Lautsprecher-Enable-Pin
spk_level      = 1
fragment_time  = 2
```

`spk_gpio = -1` ist die Talkback-Antwort. Der Treiber hat keinen Pin bekommen,
mit dem er einen Verstärker einschalten könnte, und ein externer Codec ist
weder adressiert noch benannt. **Es gibt auf diesem Board keinen
konfigurierten Ausgabepfad.**

Für die Eingabe sprechen dagegen `aic_enable=1`, die gesetzten Mikrofon-Gains
und der hochgefahrene interne Codec. Ob wirklich ein Mikrofon angelötet ist,
sagt das nicht — das sagt nur ein Hörtest oder die Platine.

### SDK

`libimp.so` trägt beide Familien: `IMP_AI_Enable`, `IMP_AI_EnableChn`,
`IMP_AI_GetFrame`, `IMP_AI_EnableAec/Agc/Ns/Hpf` … und ebenso `IMP_AO_Enable`,
`IMP_AO_EnableChn`, `IMP_AO_FlushChnBuf` … Am SDK scheitert es also nicht.

## 2. Der Majestic-Vertrag

`/etc/majestic.yaml` auf dieser Kamera:

```yaml
audio:
  enabled: false
  volume: 30
  srate: 8000
  codec: opus
  outputEnabled: false
  outputVolume: 30
```

Die Schlüssel, die die WebUI wirklich liest: `audio.enabled`, `audio.codec`,
`audio.srate`, `audio.volume`, `audio.outputEnabled`, `audio.opus`,
`audio.pcm`. Der Player entscheidet über `preview-page.js`:

```js
audioConfigured = mjGet(cfg, 'audio.enabled') === true;
```

## 3. Der Befund, der eine Änderung ausgelöst hat

`audio-check.js` unterscheidet ausdrücklich zwischen *nichts gesagt* und *aus*,
und begründet es selbst:

> **Absent is not false.** A camera that never sent the key has not said its
> microphone is off — it has said nothing — and a panel that turns silence into
> "switched off" sends somebody looking for a control to change that may not
> even be there.

Direkt gefragt (`diagnose()` per `module.exports`, unverändert):

```
keine audio-Sektion (Machino bisher)
  {"blocked":true,"why":"The camera has not said what its audio settings are yet."}

enabled=false, outputEnabled=false
  {"blocked":true,"why":"This camera has both its microphone and its speaker
                         switched off, so there is nothing to test yet."}

beide an
  null
```

Der erste Satz ist ein **Wartezustand** über eine Kamera, die nie antworten
wird. Der zweite ist eine Feststellung — und zwar genau die, die eine
**Stock-Kamera mit Stock-Defaults** erzeugt, denn `majestic.yaml` liefert beide
Schalter als `false`.

## 4. Behoben

`majestic_config()` meldet jetzt:

```json
"audio": { "enabled": false, "outputEnabled": false }
```

Nur die zwei Schalter. **Nicht** `volume`, `srate`, `codec`, `outputVolume` —
das wären Einstellungen, die nichts tun.

Am Medienpfad ändert das nichts: `preview-page.js` prüft `=== true`, und
abwesend und `false` stimmen dort schon überein. Der Gewinn ist allein, dass
das Prüfpanel eine Aussage statt eines Wartezustands zeigt.

Und ein Schreibversuch bekommt eine Begründung statt „unknown section" —
dieselbe Regel wie bei `nightMode` (AP19) und `jpeg` (AP14): eine Sektion, die
die API **meldet**, darf beim Schreiben nicht wie ein Tippfehler behandelt
werden.

```
POST {"audio":{"enabled":"true"}}
  -> 403 unsupported_control
     "this build has no audio path: nothing captures from /dev/dsp and nothing
      plays to it. The T40 inner codec is up and the SDK has IMP_AI/IMP_AO, so
      capture is possible later - but the audio driver was given spk_gpio=-1
      and no external codec, so there is no configured output on this board at
      all."
```

Die Meldung nennt beides getrennt: Capture ist später möglich, Ausgabe nicht.
Das ist die Trennung, die AP20 verlangt („Talkback getrennt behandeln").

## 5. Warum Capture nicht jetzt gebaut wurde

Es wäre zu bauen: `IMP_AI_*`-Capture, ein Codec (der Vertrag nennt `opus` bei
8 kHz), A/V-Synchronisation gegen die Videozeitachse, RTSP-Ausgabe mit einem
zweiten Track und der `&audio=`-Pfad auf `/ws/video`.

Dagegen stehen drei Dinge, und keines davon ist Bequemlichkeit:

1. **Ob überhaupt ein Mikrofon dranhängt, ist unbekannt.** `aic_enable=1` sagt,
   dass der Controller an ist, nicht dass ein Wandler angeschlossen ist. Ein
   Audio-Track, der Stille überträgt, ist schlechter als keiner — er ist die
   „Erfolgsmeldung, obwohl nichts passiert", die AP19 verboten hat, nur eine
   Ebene tiefer. Das ist `PENDING_PHYSICAL`, und zwar zwingend vorher.
2. **AP20 sagt: „Audio darf den bestehenden Video-Low-Latency-Pfad nicht
   destabilisieren."** Der Pfad ist hardwareabgenommen bei ~100 ms über WebRTC
   und wurde in diesem Batch gerade erst vermessen. Einen zweiten
   Medienerzeuger mit eigener Zeitachse daneben zu stellen, ohne ihn auf
   Hardware prüfen zu können — der Daemon lässt sich nicht neu starten —, ist
   der schlechtere Tausch.
3. **Opus.** Der Vertrag nennt `codec: opus`. Das Makefile sagt „no third-party
   frameworks", mit mbedTLS als einziger begründeter Ausnahme. Ein zweiter
   Encoder ist eine Architekturentscheidung, kein Batchschritt — dieselbe
   Grenze, an der AP13 den TTF-Rasterer abgelegt hat.

## Tests

`test_unoffered_subsystems()` deckt jetzt fünf Sektionen ab:

* `records`, `analytics`, `peers` → `400 unknown_field` (die Kamera erwähnt sie
  nie, also sind sie ihr wirklich unbekannt)
* `nightMode`, `audio` → `403 unsupported_control` mit Begründung (die Kamera
  meldet sie)
* und für alle fünf: der übersetzte Patch bleibt **leer**.

2531 Hosttests, 0 failed.

## Stand gegen „Fertig wenn"

> Audio/Talkback entweder real funktionieren oder aufgrund belegter
> Hardwaregrenzen sauber deaktiviert.

* **Talkback:** sauber deaktiviert, mit Beleg — `spk_gpio = -1`, kein externer
  Codec. Keine Hardwaregrenze, die man wegprogrammieren könnte.
* **Audio-In:** sauber deaktiviert, aber **nicht** mit einer Hardwaregrenze
  begründet — der Pfad existiert vermutlich. Begründet ist es mit einer
  ungeprüften Voraussetzung (hängt ein Mikrofon dran?) und mit dem Risiko für
  den Videopfad. Das ist eine schwächere Begründung als bei Talkback, und sie
  wird hier nicht stärker dargestellt, als sie ist.
* **Im Schema veröffentlicht:** nichts. Zwei Schalter in der Config, beide
  `false`, beide wahr.

### `PENDING_PHYSICAL`

* Hörtest: hat dieses Exemplar ein Mikrofon? Ohne das ist jede Capture-Arbeit
  auf Verdacht.
* Platine: gibt es einen Lautsprecheranschluss, den der Treiber nur nicht
  kennt? `spk_gpio = -1` ist eine Treiberkonfiguration, keine Aussage über
  Kupfer.
