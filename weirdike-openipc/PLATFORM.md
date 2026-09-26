# AP1 (Feature 2): Plattformvertrag T40NN/OpenIPC und Datenpfad-Entscheidung

Stand 2026-09-26. Abgleich des GPT-Plans mit dem BESTAND — weirdiked
existiert seit f5e1208 und erfuellte grosse Teile von AP1 bereits; dieses
Dokument haelt die Matrix, die Entscheidung und die zwei bewussten
Abweichungen fest.

## Dependency und Provenienz (AP1 §1)

WeirdIKE ist als **byte-identische, flache Vendor-Kopie** gepinnt
(`vendor/weirdike/PINNED_COMMIT`: d3c5d1e, GPL-2.0-or-later, LICENSE und
NOTICE liegen daneben, Update-Prozess = neu kopieren + SHA bumpen; die
"nie in-place editieren"-Regel steht dort). BEWUSST kein git-Submodule:
`Miguel0888/WeirdIKE` ist **privat**, und ein Submodule haette jeden
oeffentlichen CI-Lauf an ein Token gekettet. Die Kopie ist die Form der
Abhaengigkeit, die ohne Zugriff aufs Private reproduzierbar baut — das
IST der bestehende Dependency-Mechanismus dieses Repos (vgl. machinos
ingenic-headers). Seit AP1 zusaetzlich vendored: upstreams
`src/transport/udp_bsd.*` (fuer Tests/AP2-Nutzer; der Daemon behaelt
seinen eigenen Transport, s.u.).

## Minimaler Linux-Build + Lifecycle (AP1 §2)

- CI kompiliert seit je JEDE vendored Datei einzeln (Host, -Werror) und
  cross-linkt weirdiked/weirdikectl gegen das gepinnte mbedTLS 3.6.7
  (derselbe Pin wie machinos WebRTC — genau eine Kryptobibliothek auf
  4,6 MB Flash).
- NEU (AP1): `make test-lifecycle` — weirdike_mem_req(), mbedTLS-Adapter
  init/bind, udp_bsd an konkreter Source-IP, weirdike_new() mit
  PSK-Minimalkonfiguration, Zustand IDLE, weirdike_free(). Laeuft als
  eigener CI-Step.

## Zielplattform-Matrix T40NN/OpenIPC (AP1 §3)

| Faehigkeit | Befund | Beleg |
|---|---|---|
| POSIX-UDP, Bind an konkrete Source-IP | JA | busybox/musl-Userland; dieselben Syscalls traegt der Cellular-Helfer produktiv |
| UDP/500 + UDP/4500 frei | JA | machinod belegt 80/554/8080; kein IKE-Dienst im Image |
| mbedTLS mit AES-CBC/HMAC-SHA2/MODP2048 | JA | Pin 3.6.7, im WebRTC-Pfad der SELBEN Kamera hardwareverifiziert (DTLS) |
| /dev/net/tun bzw. tun.ko | JA (Modul vorhanden, 4.4.94, ungeladen) | Messung 2026-09-23; Ladeweg `tun`-Zeile in /etc/modules (install.sh --with-weirdike setzt sie) — erster modprobe: PENDING_PHYSICAL |
| Kernel-XFRM/ESP | **NEIN** | gemessen: kein /proc/net/xfrm_stat, /proc/net/protocols ohne ESP/AH (README) |
| Raw-Socket IPPROTO_ESP | vermutlich ja (AF_INET/SOCK_RAW ist Kern-INET) | **irrelevant per Entscheidung, s.u.**; falls je gebraucht: PENDING_PHYSICAL |
| Rechte | root, keine Capability-Huerden | gesamtes OpenIPC-Userland laeuft als root |
| Routing-Werkzeuge | busybox `ip` | die ganze Nacht produktiv benutzt (Cellular-Routen) |
| SO_BINDTODEVICE | Kernel kann es; NICHT der Vertrag | Underlay-Wahl laeuft ueber konkrete Source-IP (udp_bsd-Modell) — Interface-Namen bleiben aus dem Core draussen |

## Datenpfad-Entscheidung (AP1 §4)

```
IKE:   WeirdIKE-Core, UDP 500/4500 (Daemon-Transport, poll-basiert)
ESP:   WeirdIKE esp_session — USERSPACE. Kernel-XFRM existiert nicht
       (gemessen) und waere auch sonst ein zweiter SA-State: abgelehnt.
IF:    Linux TUN "ipsec0" (tun.ko vorhanden)
AUSSEN: **NAT-T UDP/4500 AUSSCHLIESSLICH** (RFC 3948)
```

Abweichung von GPTs "Raw-ESP oder NAT-T": **Raw-ESP ist gestrichen, kein
Verlust.** Beide realen Underlays dieser Kamera sitzen hinter NAT —
Cellular (Telekom CGNAT, heute Nacht vermessen) IMMER, Ethernet hinter
der FRITZ!Box ebenso. NAT-D erkennt das, NAT-T ist dann Pflicht; ein
Raw-ESP-Pfad waere toter Code mit eigener Fehlerklasse. weirdikeds
README traegt diese Scope-Entscheidung seit f5e1208.

## Schichtgrenze (AP1 §5) — und die zweite Abweichung

```
Machino (machinod)          KEIN VPN-Code. Kennt weirdike nicht.
        |
   (kein Link!)
        |
weirdiked (eigener Prozess) = GPTs "LinuxIpsecRuntime/Platform":
  ├─ WeirdIKE-Core + crypto_mbedtls (vendored, gepinnt)
  ├─ eigener poll-Transport (statt udp_bsd, Begruendung im PINNED_COMMIT:
  │  EIN poll() ueber udp500/udp4500/tun/control; natt_classify()
  │  demultiplext ESP vs. IKE auf 4500 — dieselbe Form wie die
  │  P4-Integration; udp_bsd blockiert und besitzt sein Socket)
  ├─ wd_net: TUN/ipsec0 + Sockets (ioctl, nie eine Shell)
  └─ wd_config: Validierung, PSK 0600, write-only
weirdikectl                 = Status/Steuerung, 0600-Unix-Socket
OpenIPC-UI (ipsec.cgi)      = Whitelist-Aktionen, keine Secrets
```

Abweichung von GPTs "IpsecService IN Machino": Der Prozess ist die
Grenze — dieselbe Entscheidung wie beim NNA-Helfer und beim
Cellular-Helfer, plus GPL-Hygiene (weirdike GPL-2.0-or-later bleibt in
einem eigenen Binary). Die P4-Dateien (ipsec_service/_config_fields/
_crypto_caps/vpn_status) sind SEMANTIK-Referenz fuer AP3+ (Feldnamen,
Trust-Modi, Diag-Trennung), nicht Portiervorlage — Arduino-Typen kommen
nirgends her.

## AP1-Abnahme

- [x] exakter Pin (d3c5d1e) + Provenienz + GPL/NOTICE
- [x] Core + mbedTLS + udp_bsd bauen im Linux-Build (CI)
- [x] Host-Lifecycle-Test (make test-lifecycle, CI-Step)
- [x] Zielplattform-Matrix (oben; Messungen benannt)
- [x] Offenes ehrlich PENDING_PHYSICAL: erster modprobe tun am Geraet;
      Interop/AP2 folgt in CI, nicht am Geraet
- [x] keine UI-Aenderung, kein VPN-Connect in AP1
