# Machino OpenIPC install bundle

Installs Machino next to Majestic on an OpenIPC camera and adds the selector
you use to switch between them.

Full instructions: [`INSTALL.md`](INSTALL.md) in this directory (also in the
repository as `machino/docs/install-openipc.md`).

## Quick start

```sh
# BusyBox tar has no -z, so unpack with gzip:
#   gzip -dc machino-openipc-<target>.tar.gz | tar xf -
./install.sh                 # install; does not switch anything over
streamerctl status           # what is selected, what is running, who serves port 80
streamerctl set machino      # switch to Machino (rolls back if it fails)
streamerctl set majestic     # switch back
./uninstall.sh               # remove and restore the previous setup
```

While Machino is active it serves port 80 itself as a drop-in Majestic: the
same OpenIPC WebUI with the SAME login (the camera's root account, Machino's
session /login//logout), static files relayed from an internal loopback-only
BusyBox httpd. Camera-local requests (127.0.0.1) are trusted without
credentials, exactly like Majestic.

A Machino-specific adaptation of the OpenIPC WebUI camera pages (Dashboard,
Live, Camera settings) is a separate work item; this bundle no longer ships a
standalone `machino.cgi` page or edits the WebUI navigation.

## Contents

```
machino            the daemon (statically linked against the Ingenic SDK)
machino.conf       default configuration
install.sh         installer
uninstall.sh       uninstaller, restores the state recorded at install time
sbin/streamerctl   the selector - the only thing that switches media owners
init/S95streamer   boot script: starts the selected service
init/machino       start/stop for the Machino daemon
init/S42usb        boot: brings up the stack selected by usb.mode
init/S39machinodev boot: carries out a pending device install/remove.
                   Runs BEFORE S40network, so a freshly selected adapter
                   works in the same boot instead of the next one.
sbin/machino-device registers a device with the HOST: payload -> /lib/modules,
                   profile -> /etc/wireless/usb. The only place that knows
                   what "installed" means to OpenIPC.
devices/           one manifest per device. The single source for its profile
                   name, module order and USB id - shell and C++ read the
                   same file.
wifi/              kernel modules, firmware and hostapd
BUILDINFO          commit, toolchain, SDK version of this build
SHA256SUMS         checksums of every file above
```

## Three things worth knowing

**Majestic also serves the WebUI.** On OpenIPC, Majestic is the HTTP server on
port 80 (`system.webPort`, `staticDir /var/www`). Stopping it would take the
WebUI down with it - including the page you switch back from. While Machino is
active, `streamerctl` therefore serves the same `/var/www` with busybox
`httpd`, and hands port 80 back when Majestic is selected again.

**`chmod -x` does not disable an OpenIPC service.** `rcS` runs every
`/etc/init.d/S??*` without testing the executable bit; removing it only makes
the start fail with a permission error. The installer therefore moves
`S95majestic` out of that glob to `/etc/init.d/majestic`, where it stays fully
usable, and puts `S95streamer` in the boot slot. The uninstaller moves it back.

**A driver that runs is not a driver OpenIPC can see.** The AIC8800 was built,
shipped and running on the camera on 2026-09-25, and the network page still
offered only "None": `adapter_scan` reads the `modprobe` names out of
`/etc/wireless/usb` and requires every one of them to exist as a `.ko` under
`/lib/modules`. Our modules lived elsewhere and were loaded with `insmod`,
which that scanner cannot see. `machino-device` is the one place that knows
this rule. The payload stays at `/etc/machino/payload/<id>/` and survives the
device manager's "uninstall"; only the registration comes and goes.

No proprietary SDK libraries are shipped or installed: they are linked into the
binary at build time.

**Machino does not touch OpenIPC's WebUI files.** The device manager and the
network page are served by machino itself and are reachable directly:

```
http://<camera>/machino/devices
http://<camera>/machino/net
```

`install.sh --with-device-page` / `--with-network-page` can add a marked entry
to the stock System menu, and `uninstall.sh` removes exactly it -- but that is
an explicit choice by whoever installs, not part of the normal deployment.
`machino-manager install` does not pass it unless asked with `--with-pages`.
