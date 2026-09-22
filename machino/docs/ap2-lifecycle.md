# AP2 — IMP-/Lifecycle-Stabilisierung und fehlersicherer Re-Init

**Status: implementiert, hardwareverifiziert, ab jetzt Regression-Gate.**

Commits: `107f05f` (persistenter LogReader), `078c9a6` (kein Retry, Telemetrie),
`065da41` (sticky FAILED), `8927093` (Fehlermatrix + Unwind-Korrektur).

## Der Vertrag

```
0 Consumer            -> COLD_IDLE, IMP/Sensor/Pipeline aus
erster Consumer       -> STARTING -> ACTIVE
letzter Consumer weg  -> GRACE_IDLE -> STOPPING -> COLD_IDLE
```

Ein **Listener erzeugt keinen Demand**: `:554` und `:80` lauschen, ohne die
Pipeline hochzufahren, und `/ws/logs` ist ausdrücklich **kein** Media-Consumer.

## AP2.2/2.3 — `/ws/logs` darf zur Laufzeit nie `fork()`

Im Feld reproduziert:

```
IMP ACTIVE -> /ws/logs geöffnet -> Teardown -> nächster IMP-Init schlägt fehl
```

Gegenprobe ebenfalls reproduziert:

```
logread schon in COLD_IDLE gestartet -> IMP hoch -> Teardown -> Re-Init OK
```

Daher wird `logread -f` **genau einmal beim Daemonstart** erzeugt, bevor
irgendeine IMP-Initialisierung möglich ist, und für die gesamte Lebensdauer
behalten. `/ws/logs` fügt nur Subscriber hinzu oder entfernt sie — kein `fork`,
kein `exec`, kein `kill`, kein zweiter `logread`.

Die Pipe wird **immer** geleert, auch bei null Clients, damit `logread` nicht
blockiert. Beim Shutdown: Pipe schließen → `SIGTERM` → bounded `waitpid` →
notfalls `SIGKILL` → zwingend reapen. Ein unerwartet gestorbener LogReader wird
zur Laufzeit **nicht** neu geforkt, weil IMP dann schon aktiv sein könnte.

### Was hier belegt ist und was nicht

```
Belegt:        Öffnen von /ws/logs während aktivem IMP war mit dem späteren
               Reinit-Fehler reproduzierbar verknüpft.
Stark gestützt: fork() während aktivem IMP ist der auslösende Mechanismus.
NICHT isoliert: welcher konkrete Vendor-Treiber-/VMA-/DMA-Mechanismus dahinter
               liegt.
```

## AP2.4/2.5 — kein Retry, und FAILED ist sticky

Gemessen: rund **1,2 MB zusätzlicher VmRSS pro fehlgeschlagenem Bring-up**. Die
frühere Schleife machte daraus fünf unmittelbare Versuche — ein OOM-Verstärker.

```
ein Demand-Ereignis -> genau ein bring_up()
Fehler              -> Rollback -> FAILED
```

`FAILED` ist **sticky**. Keines dieser Ereignisse löst einen neuen Bring-up aus:
bestehender Demand, neuer Consumer, Demand 0→1, `acquire_base()`, `snapshot()`.
Solange kein ausdrücklich definierter Recovery-Mechanismus existiert, ist
Recovery der **Daemon-Neustart** — es wurde bewusst keine implizite
Selbstheilung ergänzt.

Ohne diese Regel wäre aus „5 Retries in einer Schleife" nur „1 Retry pro
Reconnect" geworden, und ein reconnectender Browser ist eine unbegrenzte Quelle
von Reconnects.

Telemetrie: `init_failures`, `init_retries`, `last_init_rc`, `last_init_stage`.
**`init_retries` muss 0 bleiben** — der Zähler existiert, damit eine
zurückkehrende Schleife sichtbar wäre statt stumm.

## AP2.6 — Reverse-Order-Unwind, und ein Defekt den die Matrix fand

Die Fehlermatrix aus AP2.11 schlug beim ersten Lauf fehl — genau dafür war sie
da. `FrameSource` hatte **gar keine** Abdeckung, und nichts prüfte die
**Reihenfolge** des Unwinds oder dass eine spätere Stufe nie lief.

Mit diesen beiden Zusicherungen zeigte sich: `stop_unit_locked` rief
`disable()` auf einer FrameSource, die **nie enabled** war, und `stop()` auf
einem Encoder, der **nie gestartet** war. `bound` wurde verfolgt und `unbind`
korrekt daran geknüpft — `enable` und `start` nicht.

Der Ingenic-Adapter fängt das zufällig ab (`FrameSourceChannel::disable()` kehrt
bei `!enabled_` früh zurück), im Feld ist also nichts kaputtgegangen. Aber der
Lifecycle darf sich nicht darauf verlassen, dass ein Adapter nachsichtig ist —
und ein Unwind, der Schritte protokolliert die er nie ausgeführt hat, ist genau
dann unlesbar, wenn jemand ihn liest um herauszufinden was passiert ist.

`Unit` trägt jetzt `fs_enabled` und `enc_started`.

## AP2.7 — Fehlersemantik nicht überinterpretieren

`IMP_System_Init() == -1` bedeutet **nicht** „IMP war bereits initialisiert".
Im beobachteten Fehlerfall erschienen vorher `probe ok`, `imx307 chip found`,
die Frame-Channels, `tx_isp_vic_start` und `imx307 stream on`.

Daraus folgt **nur**: der Fehler wird erst nach weit fortgeschrittenem
Driver-Bring-up gemeldet. Ob die Ursache in libimp, im ioctl-/Treiberzustand,
in Vendor-Ressourcen, in DMA/MMAP/VMA oder in der Speicherallokation liegt, ist
**offen**. Keine dieser Schichten wird ohne Beweis als Ursache benannt.

## AP2.8 — Speicherverhalten

```
Boot COLD_IDLE:        ~1536 kB VmRSS
nach warmem Zyklus #1: ~4948 kB
nach Zyklus #2:        ~4952 kB   (+4 kB)
```

Das belegt **kein Hinweis auf ein lineares Lifecycle-Leak** und **bounded warm
retention über die beobachteten Zyklen**.

Es belegt **nicht**, dass die retained ~3,4 MB „definitiv Pools plus musl" sind.
Pools, die ihre Kapazität behalten, und ein musl-Heap, der Freigegebenes nicht
ans System zurückgibt, sind plausible Erklärungen — **experimentell isoliert
wurde die Ursache nicht**.

## AP2.9/2.10 — Hardware-Acceptance

Bereits verifiziert:

| | |
|---|---|
| 3 parallele Media-Consumer | ~15 min, 302 MB ausgeliefert |
| persistenter `logread` | durchgehend genau 1 |
| Teardown | sauber, COLD_IDLE |
| **Re-Init danach** | 2 105 342 Bytes in 6 s |
| `init_failures` / `init_retries` | 0 / 0 |
| Zombies / CLOSE_WAIT | 0 / 0 |

Ebenfalls verifiziert: die Sequenz, die vor Fix A mit `503` endete — RTSP aktiv,
`/ws/logs` **während** ACTIVE geöffnet, beides geschlossen, Grace abgelaufen,
RTSP erneut — liefert wieder Daten.

## Regression-Gate

Bei Änderungen an `PipelineManager`, `DemandHandle`, dem IMP-Backend, dem
HTTP-/MSE-/WebRTC-/RTSP-Lifecycle oder an Logging-/Kindprozessen erneut fahren:

```
Cold Boot
COLD_IDLE -> MAIN -> ACTIVE -> Media -> schließen
          -> GRACE_IDLE -> STOPPING -> COLD_IDLE
          -> MAIN erneut -> ACTIVE -> Media          (mehrfach)

ACTIVE -> /ws/logs öffnen -> Logs -> schließen
       -> Media schließen -> COLD_IDLE -> Media erneut

prüfen: genau 1 logread, 0 Zombies, 0 CLOSE_WAIT,
        init_failures = 0, init_retries = 0, pool exhausted = 0
```

Die 15-Minuten-Mehrclient-Variante muss nicht bei jedem Commit laufen, aber bei
Änderungen an den oben genannten Stellen.

## Abgrenzung

Nicht Bestandteil und mit eigenen APs geführt: der seltene SoC-Hardlock, der
Relay (AP1), der Watchdog, MSE-Latenz, WebRTC-Browserkompatibilität,
Substream-WebUI, ONVIF, RTSP-Auth-Policy, JPEG/Snapshot.
