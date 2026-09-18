# Machino — Herkunft & Lizenzen

**Machino** ist ein Media-Daemon (Majestic-Ersatz) für OpenIPC auf Ingenic-Kameras,
abgeleitet von **[Lu-Fi/timps](https://github.com/Lu-Fi/timps)** ("Tiny IMP Streamer", **MIT**).
Dieses Repository ist ein Fork; die ursprüngliche Attribution bleibt erhalten.

- **Machino-/timps-Quellcode:** MIT (siehe `README.md`, Abschnitt *License*).
- **`include/` (Submodul `gtxaspec/ingenic-headers`):** Ingenic-API-Header, aus öffentlichen
  Quellen gesammelt — **nicht MIT**, eigene/ungeklärte Lizenz. Wird nur zur Build-Zeit
  eingebunden, nicht relizenziert.
- **Ingenic-SDK-Blobs** (`libimp`/`libalog`/`libsysutils`, 1.3.1) via `build.sh` aus
  `gtxaspec/ingenic-lib` bzw. thingino: **Ingenic-proprietär**, werden zur Build-Zeit
  geladen und statisch in `machino` gelinkt; **nicht** in dieses Repo committet.
- **Kein GPL-Code** (z. B. Raptor) wird beigemischt, damit der Eigencode MIT bleiben kann.

Ziel-Hardware zunächst: **T40NN + IMX307** unter OpenIPC (musl); Build gegen SDK **1.3.1**
(Header `include/T40/1.3.1/en`), weil die von OpenIPC mitgelieferte 1.2.0-libimp den
T40NN-Chip nicht kennt und mit dem Majestic-ABI nicht zusammenpasst.

## Upstream-Basis
Fork von Lu-Fi/timps @ `3de3a5694eb340684ecde4cb82741c637ddaa141` (main, Stand 2026-09-18). Machino-Aenderungen darueber.
