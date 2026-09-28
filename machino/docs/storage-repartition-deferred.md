# Repartitionierung / Machino-Firmware-Modus — ZURÜCKGESTELLT

Stand: 2026-09-28. Entscheidung: **erst mal so lassen.** Overlay bleibt wie es
ist; wir hoffen, dass der Platz für ein KI-Modell (Personenerkennung) reicht.
Dieses Dokument hält den Kontext fest, falls wir es später doch angehen.

## Die Frage, die dahinter stand

majestic ist read-only-Totgewicht im ROM (mtd3): ~1743 KB majestic + ~1006 KB
ungenutzte System-`libimp` = **~58 % des ROM, das machino nicht braucht**. Idee
war, majestic zu entfernen und machino nach mtd3 zu verschieben ("robust
machen"), ggf. mtd3 zu vergrößern — per echtem Repartitionierungs-Tool in der UI.

## Warum zurückgestellt (die Technik)

- **`mtdparts` ist aus Linux schreibbar** (`fw_setenv`, U-Boot-Env mtd1; letzte
  Partition `-(rootfs_data)` = Rest). Repartitionieren ist also **technisch
  machbar** und mit UART **wiederherstellbar** — nicht "gebrickt", nur riskant.
- **Aber "machino nach mtd3 = robust" ist invertiert:** im Overlay überlebt
  machino Stock-Updates (bewiesen, W5). In mtd3 würde ein Stock-OpenIPC-Update
  machino **überschreiben** und majestic zurückholen. Man bräuchte einen
  **eigenen Firmware-Kanal** (nur eigene machino-Images flashen).
- **mtd3 müsste wachsen:** Stock-rootfs 4,7 MB − majestic 1,7 MB + machino
  3,0 MB ≈ 6 MB > 5 MB. Platz käme aus mtd4 (Overlay).
- **Für "Platz für Modelle" bringt es fast nichts:** die 90 % im Overlay sind
  ~8,3 MB aufräumbare Backups/Payloads, nicht majestic. **Overlay aufräumen gibt
  mehrere MB frei — mehr als Repartitionieren netto bringt, bei null Risiko.**

## Wann es sich doch lohnt

Nur wenn das Ziel ein **reines machino-Gerät ohne majestic** ist (Appliance,
maximaler Overlay für Modelle/Recordings, eigener Update-Kanal). Dann als
eigenes Projekt "Machino-Firmware-Modus": Custom-rootfs (buildroot) ohne
majestic + machino als Basis, mtd3 vergrößert, Update über den W5-`/ws/upgrade`-
Pfad mit `--url` auf eigene Images (Quelle sperren, sonst clobbert ein
Stock-Update). Kein WebUI-Knopf für die Repartition ohne dieses Gesamtkonzept.

## Vorher immer zuerst

Overlay über **System → Storage** aufräumen (Backups/Payloads löschen). Das ist
der risikofreie Platzgewinn und war 2026-09-28 der empfohlene Weg.
