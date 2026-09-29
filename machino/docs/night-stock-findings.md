# Day/Night in der Stock-Firmware (sq418): Beweise und Ableitungen

Quelle: Stock-Dump `/c/tmp/stockx/sq418/` (ovfs_boardsystem, BoardSys.json,
ko.tar.xz), disassembliert mit llvm-objdump (mipsel). Anlass: der IR-Cut
klickt beim Stock-Boot hoerbar — also existiert die Hardware, und die
Ansteuerung ist aus dem Binary ablesbar.

## IR-Cut-Filter: PD22 + PD23, sysfs-GPIO, Gegenpuls

`HalPerip_Init` (0x431cb4) initialisiert die Filter-Struct und ruft zweimal
`HalGpio_Init`:

```
431ea0: addiu $4, $zero, 0x60    ; base 96  (= Port D: PA=0, PB=32, PC=64, PD=96)
431ea4: bal   HalGpio_Init
431ea8: addiu $5, $zero, 0x16    ; idx 22   (Delay-Slot)  -> PD22 (sysfs 118)
...zweiter Aufruf mit idx aus der Struct: 0x17 = 23        -> PD23 (sysfs 119)
```

`HalGpio_Init` (0x4380a4) rechnet nachweislich `addu $17, $4, $5`
(base + index) und exportiert `/sys/class/gpio/gpio%d` — dieselbe Mechanik
wie Machinos `SysfsGpio`. Die Machino-`NightService`-Zwei-Pin-Pulslogik
(150 ms Gegenpuls, danach stromlos) entspricht dem Stock-Verhalten; das
Doppel-Klicken beim Stock-Boot ist der Init-Schwenk in eine definierte Lage.

**Machino:** `night_defaults_for_board("t40nn-imx307-board-a")` liefert
PD22/PD23 als Vorgabe (UI/Store ueberschreibt; die irCut-FREIGABE bleibt
eine bewusste Nutzerentscheidung).

## Lichtsensor: PB17 als Eingang (Automatik-Grundlage)

`InitIrLight` (0x4317dc):

```
4318c0: addiu $4, $zero, 0x20    ; base 32 (Port B)
4318c4: addiu $5, $zero, 0x11    ; idx 17            -> PB17 (sysfs 49)
4318fc: bal   HalGpio_SetDir     ; $7 = 0            -> EINGANG
```

Das ist der Tag/Nacht-Fotosensor, den Stocks „Automatic day/night" abfragt
(BoardSys.json `DayNight`: `NightToDayThreshold: 35`, `DayToNightThreshold:
20`, `Delay: 3`, `ircutOutTrig: 1`).

**Seit 2026-09-29 implementiert** (Fotosensor-Automatik):

* `nightMode.lightMonitor` (nativ `night.light_monitor`) schaltet sie ein. Der
  NightService liest PB17 in einem EIGENEN Thread im 2-s-Takt als EINGANG
  (nicht in der Hauptschleife: ein IR-Cut-Puls haelt den Thread 150 ms, und
  die Automatik darf nicht am Netzwerk-Timer haengen — Review-Fixes M4/M6)
  und schaltet erst, wenn ein Wechsel `autoNightDelay`/`autoDayDelay`
  Sekunden stabil war (Default 3, wie Stocks `Delay`). Umgeschaltet wird nur
  bei einem Wechsel: ein manueller `/night/*`-Knopf bleibt stehen, bis sich
  das Licht wirklich aendert. Beim Einschalten wird der aktuelle Zustand
  einmal angewendet.
* Ein Wechsel gilt erst als vollzogen, wenn das Schalten GELUNGEN ist.
  Schlaegt es fehl (ISP beim Boot noch nicht da, GPIO belegt), wird beim
  naechsten Takt erneut geschaltet — nicht erst beim naechsten Lichtwechsel
  (Review-Fix H1); die Telemetrie zeigt solange `pending_s: 0` und den
  Grund in `night.auto.error`.
* Der Nachtmodus ist ein Laufzeit-OVERRIDE des ISP-RunningMode
  (`TuningService::set_image_override`): nach jedem Pipeline-Start wieder
  angewendet, aber nie der Wert, den `/api/v1/config` meldet — eine nachts
  gespeicherte Image-Seite schreibt das Graubild nicht in den Tag
  (Review-Fix M1).
* Polaritaet: HIGH = dunkel (die gaengige Fotozelle mit Komparator). Fuer
  dieses Board **nicht gemessen** — liegt der Sensor andersherum,
  `lightSensorInvert` setzen. Pruefen: `cat /sys/class/gpio/gpio49/value` bei
  Licht und abgedecktem Sensor; die Telemetrie zeigt `night.auto.dark`.
* `nightMode.colorToGray` (nativ `night.color_to_gray`, Default an wie in der
  `majestic.yaml` dieser Kamera): nur dann bringt der Nachtmodus das
  Schwarzweissbild (ISP-RunningMode). Aus = nachts Filter raus, Bild farbig.
  Stock macht dasselbe (`libisp_imx307.so`: `SetISPRunningMode` beim
  day->night-Wechsel, `ColorMode` getrennt).
* Der Nachtmodus wird im TuningService VORGEMERKT und nach jedem
  Pipeline-Start neu angewendet; vorher ging er verloren, wenn das Video beim
  Umschalten kalt war.
* Nicht gebaut: die Automatik aus der Sensorverstaerkung
  (`minThreshold`/`maxThreshold`, `autoNightGain`/`autoDayGain`) — diese
  Felder bleiben aus dem Schema.

## IR-Licht (Lampe): PWM-basiert

Stock faehrt die Lampen-Helligkeit ueber `pwm_core.ko`/`pwm_hal.ko`
(BoardSys.json `IrLight` mit `Level: 100`); der Ausgangspin kommt aus einer
Laufzeit-Struct, nicht aus einer Konstante im Init-Pfad. Machinos
GPIO-Ein/Aus-Pfad (`night.backlight_pin`) deckt eine geschaltete Lampe ab;
**PWM-Dimmen ist ein dokumentiertes Folgepaket** (T40-PWM via sysfs/ioctl).
Diese Kamera hat keine Lampe bestueckt; der Anschluss existiert auf der
Platine.

## „Motor": MS41908-Objektivtreiber via SPI — geparkt, mit Grund

Der Stock-„Motor" ist KEIN PTZ-Stepper, sondern der Fokus/Zoom-Linsentreiber
**MS41908** hinter `ko/ourdrv/ants_spi_ms41908_ex.ko` + `libants_autolens_ex.so`
(BoardSys.json `AutoLen`, `Fouce`[sic], `Iris`). Fuer Machino hiesse das ein
eigener MS41908-Userspace-Treiber ueber spidev (Registersatz, Mikroschritt-
Rampen, Kalibrierfahrt). Geparkt, weil:
1. der Vendor-.ko ein Stock-Blob ist (kommt nicht ins Repo/Release),
2. diese Kamera keinen Linsenmotor bestueckt hat — nichts, woran eine
   Implementierung ehrlich verifiziert werden koennte,
3. es ein eigenstaendiges Treiberprojekt ist, kein Nachmittag.

Wenn ein Board mit bestuecktem MS41908 vorliegt, ist dieser Abschnitt der
Startpunkt (SPI-Bus/CS aus dem ko ermitteln: `modinfo`-Parameter in
`ants_spi_ms41908_ex.ko`).
