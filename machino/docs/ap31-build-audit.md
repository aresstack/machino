# AP31 — Build / Install / Upgrade / Reproducibility Audit

2026-09-23. Die Frage: funktioniert das auch außerhalb meines Arbeitsbaums?

```
RELEASE_BUILDABLE = yes
Blocker            = keine
```

---

## Der saubere Klon — die Prüfung, die vorher nie gemacht wurde

Alles bisher gebaute lief in einem Arbeitsbaum, der seit Wochen benutzt wird.
Ein frischer `git clone` samt Submodul beantwortet die eigentliche Frage:

```
git clone /c/tmp/machino-src /c/tmp/clean
git submodule update --init --recursive
  -> include @ 4e04a60  (gtxaspec/ingenic-headers), T40 1.2.0 und 1.3.1 da

66 Quelldateien laut Makefile         alle vorhanden
machino unit tests                    2582 bestanden, 0 fehlgeschlagen
streamerctl tests                     29 bestanden, 0 fehlgeschlagen
openipc install tests                 99 bestanden, 0 fehlgeschlagen, 1 übersprungen
check-linux-only                      ok
```

**Keine untracked Datei wird gebraucht.** Das Submodul ist die einzige externe
Abhängigkeit und hängt an einem festen Commit.

Einschränkung, die dazugehört: auf dieser MSYS2-Installation gibt es kein
`make`, also wurde `TEST_SRC` aus dem Makefile gelesen und direkt kompiliert
statt `make test` aufzurufen. Den Makefile-Pfad selbst fährt CI unter Linux.

## CI gegen lokal

```
                        lokal (MSYS2)              CI (Linux)
machino unit tests      2582 / 0                   2582 / 0
streamerctl tests       29 / 0                     29 / 0
openipc install tests   99 / 0, 1 übersprungen     103 / 0
```

Die Differenz von vier ist **vollständig erklärt** und keine Abweichung: der
`XBIT`-Block („Majestic war absichtlich abgeschaltet") braucht ein Dateisystem,
das das Ausführbar-Bit darstellt. MSYS2 kann das nicht, der Block wird
übersprungen und meldet das; unter Linux läuft er und trägt genau **vier**
Prüfungen bei — 2 × `is`, 1 × `has`, 1 × `if … ok`. 99 + 4 = 103.

Nachgezählt, nicht überschlagen: mein erster `grep` fand nur drei, weil das
`if … else ok; fi`-Muster nicht auf ihn passte.

## Reproduzierbarkeit

Derselbe Commit zweimal gebaut — regulärer CI-Lauf und ein `gh run rerun`
desselben Laufs (`attempt=2`, Artefakt um 01:35:27 Z neu erzeugt):

```
Versuch 1   bf72da8c02968c720b75cf5e6c876a5a41f718df966e9d81d11b72a654eb9241
Versuch 2   bf72da8c02968c720b75cf5e6c876a5a41f718df966e9d81d11b72a654eb9241
```

**Bitidentisch**, mit der Grenze: das gilt für *denselben* Commit. Der
Versionsstring steckt per `-DMACHINO_VERSION` im Binary, zwei verschiedene
Commits ergeben also notwendigerweise verschiedene Hashes.

## Das Installationspaket

```
-rwxr-xr-x   2636068  machino
-rwxr-xr-x     15970  install.sh
-rwxr-xr-x      5597  uninstall.sh
-rwxr-xr-x     11650  sbin/machino-manager
-rwxr-xr-x     15456  sbin/streamerctl
-rwxr-xr-x       974  init/S95streamer
-rwxr-xr-x      2707  init/machino
-rw-r--r--      6107  machino.conf
-rw-r--r--       892  SHA256SUMS
-rw-r--r--      1360  BUILDINFO
-rw-r--r--     10624  INSTALL.md
-rw-r--r--      2719  README.md
```

Rechte stimmen: alles Ausführbare 0755, alle Daten 0644. Jede der vier
mitgelieferten Shell-Dateien und beide Init-Skripte sind syntaktisch geprüft
(`sh -n`).

`BUILDINFO` trägt Commit, Zeitpunkt, SDK, Toolchain, libc, Ziel und die
vollständigen Compiler-Flags; `-DMACHINO_VERSION='<commit>'` steckt im Binary
und kommt über `machino --version` und den RTSP-`Server:`-Header wieder heraus
(auf der Kamera bestätigt: `machino 341a8d4` auf der Platte,
`Server: machino/c1edd92` vom laufenden Prozess).

## Installation, Upgrade, Rückweg — mit dem **echten** Artefakt geprüft

Nicht mit dem synthetischen Testbündel, sondern mit dem CI-Tarball gegen einen
Fake-Root, **alle Prüfungen scharf**:

| Fall | Ergebnis |
|---|---|
| Erstinstallation | `sha256 verified against SHA256SUMS`; Binary bytegleich; keine `*.machino-new.*` übrig; `header.cgi` unangetastet |
| verdorbenes `SHA256SUMS` | abgelehnt, beide Hashes genannt, **nichts geschrieben** |
| Gerätebaum `sigmastar,ssc335` | abgelehnt (der AP26-Nachschärfung zu verdanken — der erste Wurf ließ es durch) |
| Gerätebaum `ingenic,shark` | installiert, meldet „platform NOT verified" |
| Gerätebaum mit `t40` | installiert |
| Upgrade über bestehende Installation | Config **bytegleich** erhalten: eigene Notiz, eigener Wert und ein fremder Schlüssel, den dieser Build nicht kennt; `machino.conf.default` danebengelegt; `machino.prev` angelegt; Vorzustand `majestic-auto` bewahrt |
| Deinstallation | `S95majestic` zurück im Bootslot, Binary weg, `/etc/machino` weg |

Config-Migration zusätzlich durch Hosttests gedeckt: fremde Schlüssel,
eingerückte Zeilen mit eigenwilligen Abständen, zweimaliges Schreiben, das
nichts außer der Revision ändert, und ein Entfernen, das die fremden Zeilen in
Ruhe lässt.

Rückweg dreistufig in `RELEASE.md`: automatischer Rollback bei fehlgeschlagener
Nachprüfung (samt Neuschreiben des Manifests), Handrückweg über
`backup/machino.prev`, und `machino-manager uninstall` zurück auf Majestic.

## Hygiene

```
Passwort im Repo            keins
PCAP / Diagnosedateien      keine
eingecheckte Binaries       keine
lokale Pfade in tools/      behoben ($env:TEMP, -Dumpcap als Parameter)
lokale Pfade im Artefakt    keine
```

Die erste Hygieneprüfung meldete „alles sauber" und war ein **falsches
Bestehen** — das `git grep`-Muster war falsch maskiert und traf nichts. Erst
ein einfacher `grep` fand die `C:\tmp`-Vorgaben in drei Entwicklerwerkzeugen.

## Verdikt

```
RELEASE_BUILDABLE = yes
```

Keine Blocker. Die einzige Einschränkung ist keine des Bauens: der laufende
Daemon auf der Kamera ist `c1edd92` und damit älter als alles hier — siehe
`pending-physical.md`, insbesondere die Sicherheitszeilen S1–S3.
