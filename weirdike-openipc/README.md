# weirdiked — IKEv2/IPsec for OpenIPC on Ingenic T40

A small IKEv2 client daemon built around [WeirdIKE](https://github.com/Miguel0888/WeirdIKE),
with a userspace ESP data path over TUN.

**It is not part of Machino.** Machino is the media daemon; this is a separate
process with its own lifecycle, its own init script and its own package. A
wedged VPN must never be able to stall the video pipeline, and Machino contains
no USB, no VPN and no networking code beyond its own sockets.

---

## Why userspace ESP

The camera has no kernel IPsec at all — measured, not assumed:

```
/proc/net/xfrm_stat    does not exist
/proc/net/protocols    13 entries, neither ESP nor AH among them
```

So an in-kernel SA is not one option among several; it is unavailable. A
userspace ESP datapath over a TUN device is the only shape this can take, which
is exactly what WeirdIKE is built for.

## What is here

```
vendor/weirdike/   pinned copy of the WeirdIKE core (see PINNED_COMMIT)
src/weirdiked.c    the daemon: one poll() over udp/500, udp/4500, tun, control
src/wd_config.c    config parsing and all input validation
src/wd_net.c       TUN device and the two UDP sockets (ioctl, never a shell)
src/weirdikectl.c  one request, one reply, over a 0600 unix socket
openipc/           init script, config example, WebUI status page
tests/             host tests for the parsing and validation
```

## Scope, stated plainly

* **UDP-encapsulated ESP only** (RFC 3948, port 4500). Bare ESP over IP
  protocol 50 would need a raw socket and separate receive handling; it is not
  built. A camera behind NAT ends up on 4500 anyway. If the peer negotiates no
  NAT-T, the daemon says so rather than pretending to carry traffic.
* **PSK authentication.** WeirdIKE also implements EAP-MSCHAPv2 with an X.509
  trust model; the daemon does not expose it yet, because a certificate store
  on a 4.6 MB overlay is its own decision.
* **No routing policy.** The daemon brings up `ipsec0` and carries what is
  routed into it. Which traffic that is, is a routing question and belongs to
  the system, not to the VPN client.

## Security decisions

| | |
|---|---|
| Config file | Refused outright if group- or world-accessible. Carrying on would make the mode meaningless |
| PSK in memory | Deep-copied by the core, then wiped from our own struct and from the file buffer |
| PSK in logs | Never. WeirdIKE's own wire summaries are secrets-free by construction |
| Control socket | `/var/run/weirdike.sock`, mode 0600, three fixed commands |
| CGI | Status only. No config editing: the secret would cross the browser. Actions are three literal words compared against a whitelist, never interpolated |
| Shell | Not used anywhere. The interface is configured with ioctls, not by calling `ifconfig` |
| Input validation | Hostnames restricted to letters, digits, dot, hyphen. CIDRs parsed by hand, rejecting leading zeros, out-of-range octets and trailing garbage. Unknown config keys are an error, not a warning |

## Building

```sh
# host tests (no camera, no network, no crypto)
make test

# cross build for the T40, against the same pinned mbedTLS machino uses
make MBEDTLS=/path/to/mbedtls-3.6.7 CROSS_COMPILE=/path/to/mipsel-linux-
make MBEDTLS=... CROSS_COMPILE=... strip
```

CI does both and additionally asserts that the result is a 32-bit MIPS LSB ELF
and that `weirdiked` stays under 1.5 MB — the overlay has 4.6 MB free and
Machino already holds 2.6 MB of it.

## Installing

```sh
install -m 0755 weirdiked weirdikectl /usr/sbin/
install -m 0755 S99weirdike /etc/init.d/
install -d -m 0700 /etc/weirdike
install -m 0600 weirdike.conf.example /etc/weirdike/weirdike.conf   # then edit
echo tun >> /etc/modules        # the image ships tun.ko but does not load it
```

`S99` is deliberate: after `S40network`, after `S98wireguard`. The VPN needs a
route to its gateway and must never delay the camera coming up.

WireGuard is left entirely alone. Both can be installed; they do not share a
port, an interface or a config.

## What has and has not been verified

| | |
|---|---|
| Host tests (config, validation) | **57 checks, 0 failed**, run locally |
| Every vendored core file compiles | **22 objects, 0 warnings** at `-Wall -Wextra` |
| MIPS cross build | **CI only** — this workstation has a compiler but no `make` |
| Loading `tun` on the camera | `PENDING_PHYSICAL` — a kernel module load belongs on a device someone is sitting at |
| A real handshake against any gateway | **not performed.** Nothing is claimed about interoperability with FRITZ!Box, LANCOM, strongSwan or anything else from *this* code. WeirdIKE's own CI runs strongSwan interop gates, and its pin notes a FRITZ!Box 6860 reaching CHILD_SA_ESTABLISHED — that is the library's evidence, not this daemon's |

The WebUI page belongs to this package, not to Machino's installer: Machino
asserts byte-for-byte that it leaves the stock WebUI untouched, and that
assertion stays true. Wiring `ipsec.cgi` into the navigation is a change to the
OpenIPC WebUI and belongs in that project.
