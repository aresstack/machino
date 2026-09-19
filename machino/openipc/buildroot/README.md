# Buildroot integration

A Buildroot package so Machino can be built into an OpenIPC/thingino image
instead of installed on top at runtime. This is the *image* path; the runtime
path (install next to Majestic, switch with `streamerctl` / `machino-manager`)
is unchanged and documented in [../../docs/install-openipc.md](../../docs/install-openipc.md).

> CI-validated by Buildroot's own `utils/check-package` linter (the
> `buildroot-package` workflow). The actual cross-compile + IMP linkage is
> covered by the `build-machino-t40` workflow, which runs the exact same
> `make` invocation this `.mk` uses. We do not build a full Buildroot
> toolchain in CI (30 min for no extra coverage). What CI cannot know is your
> tree's vendor-package name — set `MACHINO_DEPENDENCIES`/`MACHINO_IMP_LIB`
> for your SDK before building in your own Buildroot.

## Adding it to a Buildroot tree

1. Copy `package/machino` into your Buildroot `package/` directory (OpenIPC
   firmware or thingino).
2. Add it to `package/Config.in`:

   ```
   source "package/machino/Config.in"
   ```

3. Point the vendor libraries at your SDK. In `machino.mk`:
   - uncomment and set `MACHINO_DEPENDENCIES` to the package that provides
     `libimp.a` / `libalog.a` / `libsysutils.a` for your SoC;
   - set `MACHINO_IMP_LIB` to where those archives are staged
     (default: `$(STAGING_DIR)/usr/lib`);
   - set `MACHINO_SOC` / `MACHINO_SDK` if not `T40` / `1.3.1`.

   The IMP **headers** ship with Machino (the `include/` git submodule), so
   `MACHINO_IMP_INC` needs no external SDK.

4. `make menuconfig` -> enable **machino** (needs a musl mipsel/xburst2
   toolchain), then `make`.

## What it installs

| Path | From |
|------|------|
| `/usr/bin/machino` | the daemon (`machinod`, installed as `machino`) |
| `/usr/sbin/streamerctl` | boot-time streamer selection |
| `/usr/sbin/machino-manager` | idempotent install/uninstall/status control surface |
| `/etc/init.d/machino`, `/etc/init.d/S95streamer` | service + boot selector |
| `/var/www/cgi-bin/machino.cgi` | WebUI switch page |
| `/etc/machino/machino.conf` | default config (canonical path; persisted edits belong on the overlay) |

## Upgrade safety

Config is **additive and forward-compatible**: unknown keys are warned about and
ignored, missing keys take defaults, and `config.revision` is preserved. A newer
Machino therefore reads an older `machino.conf` unchanged, and an older Machino
tolerates a newer file. The runtime installer never overwrites an existing
`machino.conf` (it keeps the shipped one as `machino.conf.default`); on a
read-only image, persist `/etc/machino/machino.conf` via your overlay as usual.
