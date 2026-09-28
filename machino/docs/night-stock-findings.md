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
20`, `Delay: 3`, `ircutOutTrig: 1`). Machinos Schema laesst die Automatik-
Felder weiterhin ABSICHTLICH weg, bis eine Automatik implementiert ist —
mit PB17 + diesen Schwellen ist der Bauplan jetzt aber dokumentiert.

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
