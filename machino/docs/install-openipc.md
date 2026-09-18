# Installing Machino on an OpenIPC camera

This guide starts at a camera that is already running OpenIPC and reachable
over the network. It installs Machino **next to** Majestic, so you can switch
between them and switch back.

Installing does not take your camera off the air: whatever is streaming keeps
streaming until you switch on purpose.

> **Read this first if you only read one thing.** Majestic is not only the
> video daemon — it is also the web server that serves the OpenIPC WebUI on
> port 80. Switching to Machino therefore stops the process that serves the
> page you are looking at. Machino handles this by serving the same WebUI with
> busybox `httpd` while it is the active service, so the switch page stays
> reachable in both directions. The [Recovery](#recovery) section at the end
> tells you what to do if something goes wrong anyway.

---

## Supported hardware

| SoC | Sensor | Status |
|---|---|---|
| Ingenic T40NN | Sony IMX307 | tested on real hardware (board profile `t40nn-imx307-board-a`) |

Everything else is untested. Machino refuses to start rather than guess an
I²C bus, an address or a reset pin, so an unknown board will stop with a clear
message instead of doing something unpredictable.

You need:

* an OpenIPC camera with the WebUI installed (`/var/www/cgi-bin` exists)
* SSH access as `root`
* about 1.5 MB free on the overlay filesystem — check with `df -h /`

---

## 1. Download the bundle

Get the bundle matching your SoC from the releases page, for example:

```sh
machino-openipc-t40nn.tar.gz
```

## 2. Copy it to the camera

```sh
scp machino-openipc-t40nn.tar.gz root@CAMERA:/tmp/
```

Replace `CAMERA` with your camera's IP address.

## 3. Install

```sh
ssh root@CAMERA
cd /tmp
gzip -dc machino-openipc-t40nn.tar.gz | tar xf -
cd machino-openipc-t40nn
./install.sh --webui-password 'YOUR-PASSWORD'
```

> `tar xzf` does **not** work here: the camera has BusyBox tar, which has no
> `-z`. Pipe it through `gzip -dc` as above (`tar xaf ...` works on newer
> BusyBox builds too).

`--webui-password` sets the password for the switch page for the time Machino
serves the WebUI - see [WebUI access](#webui-access) below. You can leave it
out and set it later with `streamerctl webui-password`.

The installer prints what it did and finishes with the current status. It
does **not** start Machino and does **not** stop Majestic.

### What gets installed

| Path | What it is |
|---|---|
| `/usr/bin/machino` | the daemon |
| `/usr/sbin/streamerctl` | the selector — the only thing that switches services |
| `/etc/machino/machino.conf` | your configuration (kept on upgrades) |
| `/etc/machino/streamer` | which service is selected; survives a reboot |
| `/etc/machino/streamer.preinstall` | what the camera looked like before, used by the uninstaller |
| `/etc/machino/backup/` | untouched copies of the files the installer modified |
| `/etc/init.d/machino` | start/stop for Machino |
| `/etc/init.d/S95streamer` | starts the selected service at boot |
| `/etc/init.d/majestic` | Majestic's original init script, moved out of the boot slot |
| `/var/www/cgi-bin/machino.cgi` | the WebUI page |

**Why Majestic's init script is moved:** OpenIPC's `rcS` runs every
`/etc/init.d/S??*` without checking the executable bit, so `chmod -x` does not
disable a service — it only makes the start fail with a permission error. The
only reliable way to keep Majestic from auto-starting is to take it out of that
glob. It stays fully functional as `/etc/init.d/majestic`, and the uninstaller
moves it back.

---

## WebUI access

While **Majestic** is active it serves the WebUI with its own login, exactly as
before - nothing changes.

While **Machino** is active the WebUI is served by BusyBox `httpd` instead, and
Majestic's login is not part of that. The switch page runs `streamerctl` as
root, so it is not left open:

* **With a password set** (`--webui-password`, or `streamerctl webui-password
  <password>` later) `/cgi-bin` requires HTTP Basic auth, user `root`, that
  password. Set it before you switch over.
* **Without one** the CGI is restricted to the camera itself (`A:127.0.0.1`,
  `D:*`): the switch page is then unreachable from a browser, and the CLI over
  SSH is the way to switch. Refusing beats a root-level switch open to whoever
  can reach the camera.

`streamerctl status` says which of the two you are in, and `streamerctl set
machino` warns when no password is set.

Basic auth is not TLS. Do not expose such a camera directly to an untrusted
network.

## 4. The WebUI after installing

Reload the camera's web interface. Under **System** there is a new entry,
**Media service**. If your WebUI navigation looks different and the installer
could not add the entry, it says so — the page is then still reachable
directly:

```
http://CAMERA/cgi-bin/machino.cgi
```

The page shows:

```
Active media service
[ Majestic       v ]
    Majestic
    Machino

[ Save ]
```

plus which service is selected at boot, which one is running, and who is
serving the page you are looking at.

---

## 5. Switch to Machino

**In the WebUI:** choose *Machino*, press *Save*, wait a few seconds.

**Over SSH:**

```sh
streamerctl set machino
```

What happens, in this order:

1. Majestic is stopped and the script waits until the process is really gone.
2. busybox `httpd` takes over port 80 with the same `/var/www`, so the WebUI
   stays up.
3. Machino is started.
4. Machino's API is polled until it answers.
5. If it does not come up, everything is rolled back to Majestic.

`COLD_IDLE` is a healthy state. Machino follows "no consumer, no pipeline":
with nobody watching, there is deliberately no pipeline running.

Check it:

```sh
streamerctl status
```

```
selected:     machino
running:      machino
port 80:      httpd
machino api:  cold_idle (port 8080)
```

And the stream:

```sh
ffplay rtsp://CAMERA:554/ch0
```

---

## 6. Switch back to Majestic

**In the WebUI:** choose *Majestic*, press *Save*.

**Over SSH:**

```sh
streamerctl set majestic
```

Machino is stopped, port 80 is handed back, and Majestic is started — it then
serves the WebUI itself again.

---

## 7. After a reboot

The selection is stored in `/etc/machino/streamer` and applied at boot by
`/etc/init.d/S95streamer`. A camera set to Machino comes back on Machino; a
camera set to Majestic comes back on Majestic. Exactly one media service is
started either way.

---

## 8. Upgrading

Copy the newer bundle over and run `./install.sh` again. It replaces the
binary and the helper scripts but keeps `/etc/machino/machino.conf`; the new
default config is written to `machino.conf.default` next to it so you can
compare.

If Machino is the active service, restart it to pick up the new binary:

```sh
streamerctl set majestic
streamerctl set machino
```

or simply reboot the camera.

If a WebUI update removed the **Media service** menu entry, put it back with:

```sh
cd /tmp/machino-openipc-t40nn
./install.sh --webui-only
```

That really does only the WebUI: the page and the menu entry. The binary, the
configuration, the init scripts and the boot slot are not touched.

---

## 9. Diagnosis

```sh
streamerctl status                 # selection, running service, who serves port 80
/etc/init.d/machino status         # is the daemon running?
tail -50 /var/log/machino.log      # what the daemon says
logread | tail -50                 # system log
wget -q -O- http://127.0.0.1:8080/api/v1/state     # Machino state as JSON
```

A `note: selection and running process differ` line means the stored selection
and reality disagree — usually because a start failed. `streamerctl set <the
one you want>` puts both back in sync.

---

## 10. If Machino fails to start

`streamerctl` rolls back on its own: it restarts the previous service and
tells you what failed. The camera keeps streaming with Majestic and the WebUI
stays reachable.

Find out why:

```sh
tail -50 /var/log/machino.log
```

Common causes:

* **`hardware resolution failed: ... refusing to guess`** — the board profile
  does not match this camera. Machino will not invent an I²C address or a
  reset pin. Set `board = <profile>` or the explicit `sensor.*` values in
  `/etc/machino/machino.conf`.
* **`bind/listen :554 failed`** — something else holds the RTSP port. With
  Majestic fully stopped this should not happen; check with
  `netstat -ltnp | grep 554`.
* **API not answering** — the daemon started but its HTTP API did not come up.
  Check `api.enabled`, `api.bind` and `api.port` in the config.

---

## Recovery

**The WebUI is gone and you cannot switch back.** Use SSH:

```sh
ssh root@CAMERA
streamerctl set majestic
```

**Neither service is running.**

```sh
streamerctl status
streamerctl set majestic        # or: streamerctl set machino
```

**`streamerctl` itself is broken or missing.** Everything it does can be done
by hand:

```sh
/etc/init.d/machino stop         # make sure Machino is gone
pgrep -x machino                 # must print nothing
killall httpd                    # free port 80
/etc/init.d/majestic start       # Majestic takes port 80 and the camera back
```

**The camera boots without any media service.** Log in over SSH or the serial
console and check what is selected:

```sh
cat /etc/machino/streamer
/etc/init.d/S95streamer start
```

**Put everything back the way it was** — see the next section.

---

## 11. Uninstall

```sh
cd /tmp/machino-openipc-t40nn
./uninstall.sh
```

This stops Machino, removes its files, restores `/etc/init.d/S95majestic` into
the boot slot and removes the WebUI entry.

It restores the state recorded at install time. If Majestic was deliberately
disabled before you installed Machino, it stays disabled — the uninstaller
does not switch a service on that you had switched off.

Keep your configuration for a later reinstall:

```sh
./uninstall.sh --keep-config
```

---

## What this does not do yet

* No `.ipk` package and no package manager integration — this is a tar bundle
  with `install.sh`/`uninstall.sh`. A Buildroot package comes later.
* The bundle contains no proprietary Ingenic SDK libraries. Machino links them
  statically at build time; nothing extra is installed on the camera.
* While Machino is active, the WebUI is served by BusyBox `httpd` with HTTP
  Basic auth instead of Majestic's login and session handling - see
  [WebUI access](#webui-access). Basic auth is not TLS; do not expose such a
  camera directly to an untrusted network.
* The Majestic WebUI pages that talk to Majestic's own API (dashboard, live
  preview) do not work against Machino yet. The
  [API compatibility layer](api/v1.md) covers the configuration endpoints;
  the rest follows in a later milestone.
