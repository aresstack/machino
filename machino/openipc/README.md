# Machino OpenIPC install bundle

Installs Machino next to Majestic on an OpenIPC camera and adds the selector
you use to switch between them.

Full instructions: [`INSTALL.md`](INSTALL.md) in this directory (also in the
repository as `machino/docs/install-openipc.md`).

## Quick start

```sh
./install.sh                 # install; does not switch anything over
streamerctl status           # what is selected, what is running, who serves port 80
streamerctl set machino      # switch to Machino (rolls back if it fails)
streamerctl set majestic     # switch back
./uninstall.sh               # remove and restore the previous setup
```

In the camera's WebUI the switch is under **System -> Media service**.

## Contents

```
machino            the daemon (statically linked against the Ingenic SDK)
machino.conf       default configuration
install.sh         installer
uninstall.sh       uninstaller, restores the state recorded at install time
sbin/streamerctl   the selector - the only thing that switches media owners
init/S95streamer   boot script: starts the selected service
init/machino       start/stop for the Machino daemon
webui/machino.cgi  the WebUI page
BUILDINFO          commit, toolchain, SDK version of this build
SHA256SUMS         checksums of every file above
```

## Two things worth knowing

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

No proprietary SDK libraries are shipped or installed: they are linked into the
binary at build time.
