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

### Kernel-IPsec — nicht vorhanden, wie angenommen

```
/proc/net/xfrm_stat        existiert nicht
ESP/AH in /proc/net/protocols   keine
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

**Das Flashbudget ist die engste Stelle.** 4,6 MB frei, und ein `weirdiked` mit
statisch gelinktem mbedTLS liegt erfahrungsgemäß im Bereich 0,5–1,5 MB — dazu
`weirdikectl`, Initskript, CGI. Machino selbst belegt bereits 2,6 MB. Das geht
aus, ist aber kein Fall für großzügige Annahmen; die Paketgröße gehört in die
Abnahmekriterien.

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
   (thingino xburst2 musl gcc 15.3.0) und dieselbe gepinnte
   mbedTLS 3.6.7 wie der WebRTC-Pfad  -> AP34.3 ist damit erfüllt,
   ohne etwas Neues zu erfinden
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
| Platz für einen zweiten Daemon | **knapp** — 4,6 MB frei, gehört in die Abnahme |

**Kein Machino-Code wurde für AP34 geändert.** Das ist die einzige Zeile der
Zielarchitektur, die heute schon vollständig erfüllt ist.
