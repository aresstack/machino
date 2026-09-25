> ## KORREKTUR 2026-09-23 — der Blocker ist aufgehoben
>
> **WeirdIKE existiert und ist erreichbar.** Gefunden bei der AP35-Arbeit: es
> liegt in `Miguel0888/WeirdIKE` (privat, GPL-2.0) und zusaetzlich vendored in
> `Miguel0888/quectel-ec200a-eu` unter `esp32-modem-host/src/weirdike/`. Beide
> Repos sind ueber den vorhandenen Git-Credential-Helper klonbar.
>
> **NACHTRAG 2026-09-26:** Es liegt sogar LOKAL auf dem Entwicklungsrechner:
> `C:\Projects\WeirdIKE` (main, synchron mit origin), daneben
> `weirdike-vcpkg`, die vendored Kopie in `quectel-ec200a-eu` und die
> Referenzen `CycloneIPSEC`/`Contiki-IPsec`. Zwei Suchlaeufe haben es
> verfehlt, weil `C:\Projects` nie durchsucht wurde — nur `/c/tmp` und
> `CLionProjects`. Stand d3c5d1e (2026-09-08): FRITZ-Interop-Werkzeuge im
> Baum; E3 PARTIAL — Split-Tunnel funktioniert, Full-Tunnel (Default-Route
> via ipsec0) fehlt und haengt am E1-Zielrouten-Mechanismus.
>
> ```
> Upstream    146 Dateien, 14 274 C-Zeilen, CMake + Makefile, CI (ci.yml, cmake.yml)
> Umfang      IKEv2-Kern, ESP, NAT-T-Demux, Rekeying, EAP-MSCHAPv2, X.509-Trust,
>             mbedTLS-Crypto-Backend, D-H-Agilitaet, COOKIE, DPD
> Interop     CI-Gates gegen strongSwan; laut PINNED_COMMIT wurde gegen eine
>             FRITZ!Box 6860 CHILD_SA_ESTABLISHED ueber NAT-T/UDP-4500 erreicht
> ```
>
> Warum meine Suche es nicht fand: beide Repos sind **privat**, und `gh` war zu
> dem Zeitpunkt nicht angemeldet. Die oeffentliche API antwortet auf ein
> privates Repo mit 404 — ununterscheidbar von "existiert nicht". Ich habe
> daraus geschlossen, es existiere nicht. Das war die falsche Schlussfolgerung
> aus einem richtigen Messwert; korrekt waere gewesen: "unauthentifiziert nicht
> erreichbar, Existenz unbestimmt".
>
> **Was unveraendert gilt:** alle Plattformbefunde unten (TUN, kein XFRM,
> devtmpfs, Flashbudget, S35modules) sind am Geraet gemessen und weiterhin
> gueltig. Auch die beiden Konflikte weiter unten bleiben bestehen — sie haengen
> nicht an der Quelle.
>
> **AP34 ist damit fortsetzbar** und braucht nur noch die Entscheidung, ob der
> Kernelmodul-Ladevorgang (`tun`) am Geraet gefahren wird.

# AP34 — IKEv2/IPsec für OpenIPC T40: Machbarkeit und Blocker

2026-09-23. **Nicht umgesetzt.** Der Grund ist eine fehlende Voraussetzung, kein
Plattformhindernis — die Plattform ist geprüft und geeignet.

Dieses Dokument liegt im Machino-Repo, weil hier die Projektdokumentation
lebt. **Es beschreibt keinen Machino-Code.** AP34 hält Machino ausdrücklich aus
der VPN-Implementierung heraus, und das ist auch die richtige Trennung.

---

## Der Blocker: WeirdIKE ist hier nicht vorhanden

AP34.2 sagt „Aktuelles WeirdIKE als Basis verwenden". Gesucht wurde:

```
lokaler Arbeitsbereich  /c/tmp, CLionProjects        nichts
Machino-Repo + Cam-Tool  grep -rli weirdike          nichts
Projektgedächtnis                                    nichts
GitHub-Reposuche         "weirdike"                  0 Treffer
GitHub-Codesuche         "weirdiked"                 0 Treffer
Konto aresstack          20 Repos, alle geprüft      nichts Verwandtes
```

Das Arbeitspaket verweist mehrfach auf frühere Analysen („der bereits
vorgeschlagenen Architektur", „die vorhandene WeirdIKE-Implementierung") mit
Fußnotenmarken. Diese Anlagen liegen mir nicht vor. WeirdIKE mag im privaten
Kontext existieren — **erreichbar ist es von hier aus nicht.**

### Was ich stattdessen nicht getan habe, und warum

Ich habe **keinen** IKEv2-Stack mit Userspace-ESP, NAT-T und Rekeying von Grund
auf geschrieben. Das ist eine sicherheitskritische Protokollimplementierung;
AP34 formuliert sie durchgehend als *Integration* einer vorhandenen, nicht als
Neuentwicklung. Ein selbstgebauter IPsec-Stack, der im Labortest funktioniert,
ist gefährlicher als gar keiner — er sieht wie Schutz aus.

Sobald WeirdIKE verfügbar ist, ist AP34 ohne weitere Klärung fortsetzbar: alles
unten ist bereits am Gerät gemessen.

---

## Die Plattform, geprüft statt angenommen

Alle Angaben von der laufenden T40NN, rein lesend.

### TUN — der Datenpfad ist erreichbar

```
/lib/modules/4.4.94/kernel/drivers/net/tun.ko   vorhanden, 30 968 B
vermagic                                        4.4.94 SMP preempt mod_unload
                                                MIPS32_R2 32BIT   (passt exakt)
in /proc/modules                                NICHT geladen
/dev/net                                        existiert nicht
/dev/net/tun                                    existiert nicht
/dev                                            devtmpfs
misc-Major                                      10
/proc/misc: tun                                 nicht registriert
mdev-Regel für tun                              keine, und keine nötig
```

**Befund:** Die AP-Annahme `CONFIG_TUN=m` stimmt. Weil `/dev` ein **devtmpfs**
ist, legt der Kernel `/dev/net/tun` beim Laden des Moduls selbst an — es braucht
weder eine mdev-Regel noch ein manuelles `mknod`. Ein `modprobe tun` genügt.

**Aber:** das ist ein Kernelmodul-Ladevorgang, und der berührt die No-Go-Grenze
dieses Projekts. `tun.ko` ist ein In-Tree-Standardmodul mit passendem vermagic,
kein ISP-/Treibertausch — trotzdem gehört der erste Ladeversuch an ein Gerät,
an dem jemand sitzt. **`PENDING_PHYSICAL`.**

### Das Laden hat bereits einen vorgesehenen Ort

```
/etc/init.d/S35modules   liest /etc/modules und modprobet jede Zeile
/etc/modules             enthaelt heute: vfat, exfat
geladen                  gpio, audio, sensor_imx307_t40, tx_isp_t40,
                         avpu, sinfo, vfat, fat
```

Für `tun` braucht es damit **kein neues Initskript und keinen
`modprobe`-Aufruf im VPN-Dienst**: eine Zeile `tun` in `/etc/modules` ist der
Stock-Weg dieser Firmware, liegt persistent auf dem Overlay und läuft als
`S35` **vor** `S40network`. Das ist der unaufdringlichste verfügbare Eingriff
— er fasst kein bestehendes Skript an, sondern ergänzt eine Datenzeile.

### Kernel-IPsec — nicht vorhanden, wie angenommen

```
/proc/net/xfrm_stat   existiert nicht
/proc/net/protocols   13 Eintraege, kein ESP und kein AH darunter:
                      PACKET PINGv6 RAWv6 UDPLITEv6 UDPv6 TCPv6 UNIX
                      UDP-Lite PING RAW UDP TCP NETLINK
```

**Damit ist die Architekturentscheidung des AP belegt, nicht bloß plausibel:**
ein Userspace-ESP-Datenpfad über TUN ist auf dieser Kamera nicht eine von
mehreren Optionen, sondern die einzige.

### Umgebung

```
Krypto im Kernel     24 Algorithmen (u. a. aes, sha256) — für einen
                     mbedTLS-Userspace-Pfad ohnehin unerheblich
WireGuard            /usr/bin/wg, wireguard.ko, /etc/init.d/S98wireguard
                     -> AP34.12s Prämisse stimmt
vtun                 /etc/init.d/S98vtun
Interfaces           eth0, lo, sit0, tunl0
freier Flash         4,6 MB auf dem jffs2-Overlay
MemAvailable         ~20,9 MB
```

**Das Flashbudget ist die engste Stelle.** 4,6 MB frei; `/usr/bin/machino`
belegt davon bereits 2 636 028 B. Wie groß ein `weirdiked` mit statisch
gelinktem mbedTLS ausfällt, **weiß ich nicht** — das hängt an Code, den ich
nicht habe. Eine Hausnummer wäre hier genau die Sorte Zahl, die drei Dokumente
später wie eine Messung gelesen wird. Die Paketgröße gehört deshalb als hartes
Abnahmekriterium in die Umsetzung, nicht als Annahme in die Planung.

---

## Zwei Konflikte, die vor der Umsetzung entschieden werden müssen

### 1. AP34.7 gegen eine getestete Invariante

AP34.7 will `ipsec.cgi`, einen Eintrag in `p/pages.cgi` und eine Navigationszeile
in der OpenIPC-WebUI. Dieses Projekt hat durchgehend die Gegenregel erzwungen
und **getestet**:

> Die Installation fasst die Stock-WebUI nicht an: kein `machino.cgi`, keine
> Menüzeile — der Test vergleicht `header.cgi` byteweise vor und nach
> Installation **und** Deinstallation.

Das ist kein Widerspruch im Ziel, sondern eine Frage der Zuständigkeit: die
WebUI-Erweiterung gehört in das **OpenIPC-WebUI-Projekt** bzw. in das
WeirdIKE-Paket, nicht in Machinos Installer. Sonst kann Machino nicht mehr
behaupten, die Stock-Oberfläche unberührt zu lassen — und genau diese Behauptung
ist durch Tests gedeckt.

Zusätzlich: `/c/tmp/majestic-webui` ist in diesem Projekt das **Orakel**, aus
dem jeder Vertrag abgeleitet wird. Dort etwas hineinzuschreiben würde die
Referenz beschädigen, gegen die alles geprüft wird.

### 2. Feature Detection löst das Henne-Ei-Problem — in der richtigen Reihenfolge

AP34.8 (`[ -x /usr/sbin/weirdiked ]`) ist gut gewählt: eine `ipsec.cgi`, die
ohne Daemon unsichtbar bleibt, lügt nicht. Sie wäre also *baubar*, bevor der
Daemon existiert. Ich habe sie trotzdem nicht gebaut — eine Seite, deren Backend
niemand kennt, wird gegen ein erfundenes `weirdikectl`-Protokoll geschrieben,
und die Statusfelder aus AP34.5 (`natt`, `rekey_in`, `child=`) sind genau die,
die die tatsächliche Implementierung festlegen muss. Das wäre geraten.

---

## Was ein Fortsetzen braucht — in dieser Reihenfolge

```
1. WeirdIKE-Quelle bereitstellen (Repo-URL oder Tarball)
2. modprobe tun EINMAL am Gerät, mit jemandem davor   -> PENDING_PHYSICAL
   danach: /dev/net/tun vorhanden? ipsec0 anlegbar?
3. Crossbuild gegen dieselbe Toolchain wie Machino
   (thingino-toolchain-x86_64_xburst2_musl_gcc15-linux-mipsel; die
   genaue Compilerversion schreibt das CI beim Bauen nach BUILDINFO)
   und dieselbe gepinnte mbedTLS 3.6.7 wie der WebRTC-Pfad
   -> AP34.3 ist damit erfüllt, ohne etwas Neues zu erfinden
4. Paketgröße gegen 4,6 MB freien Flash prüfen
5. weirdikectl-Protokoll festschreiben, DANN ipsec.cgi
6. WebUI-Änderung im OpenIPC-Projekt, nicht in Machinos Installer
7. Interop gegen strongSwan, bevor irgendeine
   Gegenstellen-Kompatibilität behauptet wird
```

---

## Stand gegen „Fertig wenn"

Von fünfzehn Kriterien ist **keines** erfüllt, weil alle auf der Quelle
aufbauen. Erfüllt sind stattdessen die Voraussetzungen, die das AP als gegeben
angenommen hatte und die ich nachgemessen habe:

| AP-Annahme | Status |
|---|---|
| `CONFIG_TUN=m` vorhanden | **BELEGT** — Modul da, vermagic passt |
| `XFRM_USER` und Kernel-ESP fehlen | **BELEGT** — kein `xfrm_stat`, keine ESP/AH-Protokolle |
| Userspace-ESP über TUN ist der richtige Weg | **BELEGT** als einzige Möglichkeit |
| WireGuard ist vorhanden und bleibt unangetastet | **BELEGT** — nichts angefasst |
| `/dev/net/tun` ist anlegbar | **STARK GESTÜTZT** — devtmpfs erledigt es beim Modulladen; der Ladevorgang selbst ist `PENDING_PHYSICAL` |
| Platz für einen zweiten Daemon | **OFFEN** — 4,6 MB frei, davon nichts reserviert; ohne die Quelle ist die Paketgröße unbekannt |
| Modulladen hat einen Stock-Hook | **BELEGT** — `S35modules` liest `/etc/modules`, läuft vor `S40network` |

**Kein Machino-Code wurde für AP34 geändert.** Das ist die einzige Zeile der
Zielarchitektur, die heute schon vollständig erfüllt ist.

---

## Review dieses Dokuments

Vier Angaben der ersten Fassung stammten nicht aus einer Messung, sondern aus
dem Gedächtnis. Alle am Gerät nachgeprüft:

| Behauptung | Ergebnis |
|---|---|
| keine ESP/AH-Protokolle | **bestätigt** — jetzt mit der vollständigen Liste aus `/proc/net/protocols` belegt |
| `/etc/init.d/S98vtun` existiert | **bestätigt** |
| Machino belegt ~2,6 MB | **bestätigt** — exakt 2 636 028 B |
| Toolchain „gcc 15.3.0" | **FALSCH.** Diese Version steht im Baum ausschließlich für die *xburst1-uclibc*-Toolchain. Machino baut mit `xburst2-musl-gcc15`. Wer Schritt 3 gefolgt wäre, hätte den falschen Compiler gepinnt. Korrigiert; die genaue Version hält ohnehin BUILDINFO fest. |

Dabei fiel auf, was die erste Fassung übersehen hatte: `S35modules` und
`/etc/modules` existieren bereits, das Modulladen braucht also keinen neuen
Mechanismus. Ergänzt.

Entfernt: die geschätzten „0,5–1,5 MB" Binärgröße für einen Daemon, dessen
Quelltext ich nicht habe.

Ein Werkzeugfehler dabei, protokolliert wie in AP29: die erste
Ersetzungsschleife verglich UTF-8-dekodierten Dateiinhalt gegen
undekodierte Bytes aus `__DATA__` und fand null Treffer bei jedem Block mit
Umlaut. Sie **brach ab**, statt still nichts zu tun — genau der Unterschied
zum awk-Selektor in AP21 und zur `git grep`-Maskierung in AP26, die beide
„geprüft" meldeten und das Falsche geprüft hatten.
