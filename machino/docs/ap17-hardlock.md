# AP17 — Den seltenen T40NN-Hardlock instrumentieren und eingrenzen

Batchlauf 2026-09-23. Kein Fix, keine Hypothese als Tatsache. Was hier steht,
ist entweder gemessen oder ausdrücklich als offen markiert.

---

## 1. Drei Befunde, die immer wieder vermischt wurden

Sie sind verschieden, sie haben verschiedene Beweislagen, und nur einer ist
noch offen.

### Befund A — OOM nach IMP-Reinit (2026-09-22) · **geschlossen**

Ein fehlgeschlagener Bring-up lief in eine Schleife aus fünf sofortigen
Wiederholungen; jeder Versuch ließ IMP-Speicher zurück, und der fünfte hat die
Box in den OOM getrieben. **Die Box blieb dabei erreichbar** — das ist ein
Daemon-Fehler, kein SoC-Fehler.

Behoben: die Wiederholungsschleife ist weg, `init_retries` in
`/api/v1/telemetry` **muss 0 bleiben** und ist genau dafür da, ihre Rückkehr zu
bemerken; die Stage-Telemetrie überlebt jetzt den tmpfs-Log; `FAILED` ist
klebrig geworden, damit ein kaputter Zustand nicht dauernd neu angefahren wird.

Siehe `incident-2026-09-22-oom.md`.

### Befund B — vollständiger Hardlock · **offen**

Netz tot, UART tot, **kein Reboot**. Was belegt ist:

```
Auslöser (gepinnt)   erster authentifizierter GET /cgi-bin/live.cgi
Kernel-Reaktion      ACK auf genau dieses Paket
danach               ein weiterer vollständiger TCP-Handshake, 3,2 ms später
dann                 Stille: kein FIN, kein RST, kein ARP
kernel.panic         20  -> eine Panik hätte neu gestartet
Folgerung            Hänger, keine Panik
```

Ausgeschlossen durch Messung: Asset-Burst, `/ws/video`, WebRTC, MSE,
`/ws/logs`, mehrere gleichzeitige Clients. **Nicht reproduziert** in rund zwölf
synthetischen Versuchen (exakte Header, Verbindungsmuster, Pipeline-Vorlauf,
ONVIF- und Substream-Konfiguration, 200 KiB und 4,2 MB Flash-Schreibvorgänge
mit GC, forklastiger Sampler, vollständiger Seitenaufbau mit 38 Assets, warme
Neuinstallation). Der Warm-Install ist als Voraussetzung **widerlegt** (der
Ausfall vom 2026-09-22 kam nach einem Kaltstart).

**Der Auslöser ist gepinnt, die Ursache nicht.** Das ist der Stand, und es ist
kein anderer.

### Befund C — TCP-Churn im Relay · **geschlossen, und kein Hardlock-Fix**

Das Relay benutzte **EOF als Rahmung**: es hat den Antwortkopf des Upstream nie
gelesen, also musste jede CGI-Antwort die Verbindung schließen. Das erzeugte
eine Verbindung pro Anfrage und entsprechend viele `TIME_WAIT`-Einträge. AP1
hat das behoben (Kopf wird gepuffert und beurteilt, Keep-Alive bei exakter
Länge).

Drei Dinge dazu, ausdrücklich:

* Das ist **kein** Hardlock-Fix und wird hier nicht als einer geführt.
* Die Behauptung „90 % weniger TIME_WAIT" ist **zurückgezogen** — das
  Vorher-Fenster waren neun Minuten mit drei Browsern und sechs
  Medien-Consumern, das Nachher-Fenster zwei Minuten auf einer frischen Box.
  Ungleiche Fenster, keine Aussage.
* `TW` in `/proc/net/sockstat` ist ein **kumulativer Zähler**,
  `tcp_max_tw_buckets` (512) eine **gleichzeitige** Obergrenze. Die beiden
  gegeneinander zu lesen, ist ein Kategorienfehler.

---

## 2. Der Watchdog: warum er nie gesprungen ist

Die Frage war: „warum springt der 15-Sekunden-Watchdog nicht an? Die Cam
rebootet nie von alleine."

Die Antwort ist unspektakulär und heute gemessen:

```
Prozesse, die /dev/watchdog offen halten   keine
Watchdog-Daemon in /etc/init.d              keiner
laufender Build                             machino c1edd92
```

`c1edd92` ist **älter als AP4**. Bei jedem bisher beobachteten Hardlock war
also **überhaupt kein Watchdog scharf**. Es gibt nichts zu erklären, warum er
nicht ausgelöst hat.

Der AP4-Watchdog liegt seit heute als `341a8d4` auf `/usr/bin/machino`, aber
der laufende Prozess ist noch der alte — die Ablösung wartet auf den nächsten
Kaltstart. Ab dann gilt: **der Watchdog ist Wiederherstellung, keine
Ursachenbehebung.** Er sagt nichts darüber, warum die Box hängt; er sorgt nur
dafür, dass sie von selbst zurückkommt. Und selbst das ist unbewiesen: ob ein
Hänger, der auch den UART verstummen lässt, den TCU-Watchdog noch feuern lässt,
weiß niemand. `nowayout` ist gemessen **aus** (offen → `V` → schließen → 89 s
ohne Reset), der Hardwarepfad also funktionsfähig.

---

## 3. Was heute an Instrumentierung entstanden ist

### 3.1 Der UART war nicht stumm — er war stummgeschaltet

Das ist der konkreteste Fund dieses Arbeitspakets.

```
/etc/sysctl.conf   kernel.printk = 3 3 1 3     <- so war es gemeint
/proc/sys/.../printk   0   0   0   0           <- so lief es
```

`console_loglevel = 0` heißt: **der Kernel schreibt nichts auf die serielle
Konsole.** Die Stille beim Hardlock am 2026-09-22 war also nie ein Beleg für
einen ruhigen Kernel. Ein Oops, eine RCU-Stall-Warnung, ein Hung-Task-Report —
nichts davon hätte man gesehen.

Weiteres, gemessen:

* `dmesg` funktioniert, der Kernel-Ringpuffer **wird** beschrieben (Treiber­
  meldungen von `imx307` stehen drin). Nur die Konsole war zu.
* Die Kommandozeile enthält **kein** `quiet` und kein `loglevel=`, also kommt
  die Null nicht vom Bootloader.
* `net.core.bpf_jit_enable` aus derselben `sysctl.conf` **ist** gesetzt, und
  ein manuelles `sysctl -p /etc/sysctl.conf` setzt `printk` sofort und dauerhaft
  auf `3 3 1 3`. Es wird also beim Booten später wieder zugedrückt. **Von wem,
  ist offen** — in `/etc/init.d/`, `/init` und `/etc` steht nichts, das es
  täte. Ich habe die Suche auf die Binärdateien nicht ausgedehnt: der erste
  Versuch hat die Kamera minutenlang beschäftigt, und das ist auf einer Box,
  deren Fehlerbild „hängt sporadisch" ist, genau die falsche Last.

Verankert als Letztes im Bootlauf, weil rc.local (S99) nach allem anderen
läuft:

```sh
# /etc/rc.local          (Vorgänger liegt als /etc/rc.local.before-ap17)
echo "3 3 1 3" > /proc/sys/kernel/printk
```

Ein kleiner jffs2-Schreibvorgang, vollständig umkehrbar, kein Kernel-, Flash-
oder Partitionseingriff. Live steht der Wert bereits.

**Damit ist der UART ab jetzt ein Messgerät und nicht mehr eine Leitung, an der
man nichts sieht.**

### 3.2 `tools/hardlock-watch.ps1`

Passives Warten, nichts Provozierendes. Drei Regeln, alle aus eigenen Fehlern:

1. **Der Ring ist begrenzt.** `-b files:12 -b filesize:20480` = harte
   Obergrenze 240 MB. Zwei frühere Mitschnitte sind auf 1,1 GB und 2,77 GB
   gelaufen, weil `dumpcap` ohne Dateizahl gestartet wurde — zweimal.
2. **Die Lebendprobe ist langsam und billig:** ein TCP-Connect alle 10 s, auf
   der Kamera selbst passiert gar nichts. Kein Sekundensampler, kein
   Fork-Sturm. Ein Instrument, das den Fehler auslösen könnte, ist kein
   Instrument.
3. **Bei Kontaktverlust wird der Ring sofort eingefroren.** `dumpcap`
   überschreibt sonst weiter, und der ganze Zweck ist, dass der Tod noch im
   Fenster steht.

Beide Zweige sind geprüft: mit der echten Kamera schreibt der Ring
(`ring_00001_….pcapng`), und gegen eine unbenutzte Adresse (die Kamera bleibt
unberührt) läuft die Abbruchkette sauber durch bis zur Dateiliste und dem
Hinweis, **vor** dem Power-Cycle die UART-Zeile zu lesen.

Auf dem Weg dahin ein eigener Fehler: `Start-Process -ArgumentList` fügt
Argumente ohne Anführungszeichen zusammen, `-f host 192.168.1.10` kam als zwei
Argumente an, `dumpcap` beendete sich sofort — mit verstecktem Fenster, also
lautlos, und der erste Testlauf schrieb null Bytes, ohne das zu melden. Jetzt
wird der Filter zitiert, stderr landet in `dumpcap.err`, und ein sofort
beendetes `dumpcap` bricht den Wächter mit der Fehlermeldung ab.

---

## 4. Was **nicht** getan wurde

| | Warum |
|---|---|
| Den Hardlock absichtlich auslösen | Batch-Regel: nicht provozieren, wenn ein physischer Power-Cycle daraus folgen könnte. |
| Flash/JFFS2, SFC, IRQ, ISP weiter aufbohren | AP17 erlaubt das „nur bei konkreter Evidenz". Es gibt keine. Die Flash-Schreibversuche (200 KiB und 4,2 MB mit GC) haben nichts ausgelöst. |
| Einen Fix „auf Verdacht" | Genau das verbietet dieses Arbeitspaket, und genau das ist beim TIME_WAIT-Thema schon einmal schiefgegangen. |
| Die Binärdateien nach dem printk-Schreiber durchsuchen | Zu teuer auf dieser Box, siehe oben. Für einen späteren Lauf mit Zugriff auf das Rootfs-Image auf dem PC ist es eine Zehn-Sekunden-Suche. |

---

## 5. Stand gegen „Fertig wenn"

> entweder eine belastbare Ursachenkette/Reproducer existiert **oder** der
> Diagnosezustand sauber dokumentiert und für spätere Feldereignisse
> instrumentiert ist.

Der zweite Fall ist erfüllt, und zwar mit einem Zugewinn gegenüber gestern:

* Die drei Befunde sind getrennt, jeder mit seiner eigenen Beweislage.
* Die Watchdog-Frage ist beantwortet: es lief keiner.
* Die UART-Stille ist erklärt und **behoben** — die Konsole spricht wieder.
* Ein begrenzter Mitschnitt-Ring mit Einfrieren bei Kontaktverlust liegt
  getestet im Repo.

**Ein Reproducer existiert weiterhin nicht**, und die Ursache von Befund B ist
offen. Das nächste Feldereignis ist damit zum ersten Mal eines, bei dem Netz
**und** Kernel etwas hinterlassen.

### Offen, `PENDING_PHYSICAL`

* `printk = 3 3 1 3` über einen echten Reboot bestätigen (rc.local greift erst
  beim nächsten Start).
* Den AP4-Watchdog scharf sehen (`watchdog_available: true` in der Telemetrie)
  — braucht die Ablösung auf `341a8d4`.
* Beim nächsten Hardlock: UART-Ausgabe lesen, **bevor** die Box vom Strom geht.
