# AP22 — Enhanced T40 Stack / neuere Ingenic-Komponenten

Batchlauf 2026-09-23. Bestandsaufnahme, rein lesend. **Kein Modul getauscht,
kein Modul geladen, kein Modul entladen.**

---

## Kurzfassung

Die Referenz bleibt, wo sie ist. Es gibt auf dieser Kamera **keinen Kandidaten,
für den ein Tausch heute einen messbaren Vorteil verspräche** — und für den
einzigen interessanten (NNA) fehlt die Plattformvoraussetzung, nicht die
Motivation. Was AP22 stattdessen eingebracht hat, ist ein **Compile-Time-Gate**,
das vorher gar nicht existierte.

---

## 1. Das Inventar

```
Kernel        Linux 4.4.94 #2 SMP PREEMPT, gebaut 2026-09-17 (der OpenIPC-Build)
vermagic      4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT   (alle Module identisch)
cmdline       mem=48M rmem=64M@0x3000000 ... rootfstype=squashfs
```

28 Module insgesamt, davon 16 unter `/lib/modules/4.4.94/ingenic`:

| Modul | Bytes | geladen |
|---|---:|---|
| `tx-isp-t40` | 988 760 | **ja** (`tx_isp_t40`) |
| `sensor_imx307_t40` | 19 576 | **ja** |
| `avpu` | 30 688 | **ja** |
| `audio` | 90 556 | **ja** |
| `gpio` | 5 032 | **ja** |
| `sinfo` | 26 444 | **ja** |
| `soc-nna` | 20 968 | nein |
| `motor` | 24 024 | nein |
| `mpsys_driver` | 20 324 | nein |
| `sample_pwm_core` / `sample_pwm_hal` | 7 688 / 9 116 | nein |
| `dtrng_dev` | 14 484 | nein |
| `sensor_gc4653/imx334/imx335/imx415_t40` | 15–19 k | nein |

Zwei davon korrigieren frühere Arbeitspakete:

* **`motor.ko` existiert.** AP18 hat im Device Tree keinen Motorknoten gefunden
  — das stimmt, und das Modul ist auch nicht geladen. Aber „es gibt keinen
  Motortreiber" wäre falsch gewesen; es gibt einen, er ist nur nicht in Gebrauch.
* **PWM existiert als Modul.** AP18 notierte „`/sys/class/pwm` leer". Der Grund
  ist, dass `sample_pwm_*` nicht geladen ist — nicht, dass die SoC kein PWM
  hätte. Für die IR-LED-Frage ändert das nichts (es fehlt weiterhin jede
  Pinangabe), aber die Begründung muss die richtige sein.

## 2. Versions- und ABI-Lage

```
System-libimp   /usr/lib/libimp.so    1 030 372 B, meldet "1.2.0"
Machino         libimp 1.3.1, STATISCH gelinkt; die System-1.2.0 wird nie geladen
Header          include/T40/1.3.1/en/imp, eigenes Submodul @ 4e04a60
ISP-Firmware    /etc/sensor/imx307-t40.bin, 204 800 B (IQ-Binärdatei)
```

Die **ABI-Regel für Module** ist auf dieser Kamera einfach und streng zugleich:
`CONFIG_MODVERSIONS` ist aus, es gibt also **keine Symbol-CRCs**. Damit ist der
`vermagic`-String das ganze Kompatibilitätsversprechen — er stimmt bei allen
Modulen überein, und jedes fremde Modul müsste exakt `4.4.94 SMP preempt
mod_unload MIPS32_R2 32BIT` tragen. Das ist eine *notwendige*, keine
hinreichende Bedingung: ohne CRCs lädt der Kernel ein Modul, dessen Strukturen
sich geändert haben, klaglos — und stürzt später ab.

Genau deshalb ist ein Tausch von `tx-isp-t40` oder einem Sensortreiber ohne
garantierte Remote-Recovery `PENDING_PHYSICAL`. Auf dieser Kamera gibt es keine
solche Garantie: ein Modul, das den ISP unbrauchbar macht, kostet das Bild, und
ein Kernel, der nicht mehr bootet, kostet die Erreichbarkeit vollständig.

## 3. Die Userspace-Lage: 1.3.1 über einem 2022er tx-isp

Das ist die Kombination, die **produktiv abgenommen** ist (Speicherstand
`machino-sdk-stack`). Sie funktioniert, und AP22 sagt ausdrücklich: „Kein
Wechsel nur wegen neuerer Versionsnummer."

Ein Punkt verdient aber Aufmerksamkeit, und er ist der eigentliche Ertrag
dieses Arbeitspakets.

### Der Header/Bibliothek-Skew, jetzt abgesichert

`IMP_Encoder_GetStream` füllt eine `IMPEncoderStream`, und `fetch()` läuft
anschließend über `st.pack[0..packCount)` mit `st.virAddr` als Basis. Jeder
dieser Zugriffe ist ein **Offset in eine Struktur, deren Layout aus einem
Header stammt, der nicht mit der Bibliothek versioniert ist**, gegen die er
spricht. Die Header sind ein eigenes Submodul und lassen sich unabhängig
umhängen.

Wenn die beiden sich je uneinig werden, ist das **kein Compilerfehler** — es
ist das Lesen einer Länge und eines Zeigers aus den falschen Wörtern.

Im gesamten Quellbaum stand **kein einziger `static_assert`**. Jetzt sind genau
die Felder festgenagelt, die `fetch()` anfasst:

```cpp
static_assert(sizeof(void*) == 4, "these layouts assume 32-bit pointers (T40 o32)");
static_assert(sizeof(IMPEncoderPack) == 32, ...);
static_assert(offsetof(IMPEncoderPack, offset)    == 0,  ...);
static_assert(offsetof(IMPEncoderPack, length)    == 4,  ...);
static_assert(offsetof(IMPEncoderPack, timestamp) == 8,  ...);
static_assert(sizeof(IMPEncoderStream) == 28, ...);
static_assert(offsetof(IMPEncoderStream, virAddr)   == 4,  ...);
static_assert(offsetof(IMPEncoderStream, pack)      == 12, ...);
static_assert(offsetof(IMPEncoderStream, packCount) == 16, ...);
```

Diese Zahlen sind **nicht** dazu da, grün gehalten zu werden. Ändert ein neues
SDK die Struktur wirklich, soll das hier **scheitern**, gelesen und der Leser
darunter bewusst angepasst werden. Das ist der ganze Zweck.

Meine erste Handrechnung sagte `sizeof(IMPEncoderPack) == 24` und war falsch —
`frameEnd` ist ein `bool` bei 16, zwei Enums folgen bei 20 und 24, und der
`int64_t timestamp` gibt der Struktur 8-Byte-Ausrichtung, also wird auf 32
aufgefüllt. Gegen den echten Header nachgerechnet statt abgezählt. Die
32-Bit-Zahlen für `IMPEncoderStream` hat der Crossbuild bestätigt.

## 4. Die Kandidaten, einzeln bewertet

| Kandidat | Befund | Entscheidung |
|---|---|---|
| `tx-isp-t40` neuer | Kein neueres Modul auf der Kamera, keine zweite Quelle im Baum. Ein Tausch bräuchte ein passendes vermagic **und** einen Kernel, der ohne CRC-Prüfung nicht klaglos das Falsche lädt. | bleibt |
| Sensortreiber `imx307` | Läuft, 20 fps @1080p hardwareverifiziert. Vier weitere Sensortreiber liegen bereit, betreffen aber andere Sensoren. | bleibt |
| ISP-Firmware / IQ | `imx307-t40.bin`, 204 800 B. Der Stock-Audit (AP6) hat sie als **bytegleich** mit der Stock-Firmware belegt — es gibt nichts Neueres zu vergleichen. | bleibt |
| `avpu` | Geladen, Videopfad funktioniert. | bleibt |
| `soc-nna` | Nicht geladen, und kann es auf dieser Kommandozeile nicht — siehe AP23. | blockiert, nicht abgelehnt |
| libimp 1.2.0 statt 1.3.1 | Rückschritt. 1.3.1 ist produktiv abgenommen, und 1.2.0 hat die Encoder-Tuning-APIs bereits, aber keinen Vorteil. | nein |

## Stand gegen „Fertig wenn"

> klar dokumentiert, welche neueren Komponenten messbare Vorteile bringen und
> welche besser beim bewährten OpenIPC-Stand bleiben.

**Messbare Vorteile: keine, für keinen Kandidaten.** Nicht weil nicht gesucht
wurde, sondern weil auf dieser Kamera nichts Neueres liegt: ein Kernel, ein
vermagic, eine ISP-Firmware, ein Satz Sensortreiber. Der einzige ungenutzte
interessante Baustein ist die NNA, und die scheitert an der Kommandozeile
(AP23), nicht an einem Versionsvergleich.

Die Messliste aus AP22 (Boot, IMP-Init/Reinit, Bildqualität, FPS, Latenz, RAM,
CPU, Stabilität, Sensormodi) wurde **nicht** abgearbeitet, weil es nichts zu
messen gab — eine Messreihe gegen sich selbst ist keine.

### `PENDING_PHYSICAL`

* Jeder Modultausch (`tx-isp`, Sensor, `avpu`). Ohne CRCs lädt der Kernel auch
  ein inkompatibles Modul und stürzt erst später ab; ohne garantierte
  Remote-Recovery gehört das an ein Gerät, an dem jemand sitzt.
* `sample_pwm_*` und `motor` laden, um zu sehen, was sie anlegen. Beides ist
  ein Schreibvorgang in den Kernel und steht auf der No-Go-Liste.
