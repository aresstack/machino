# machino-manager

One idempotent control surface for tools (the Cam-Tool) and humans. It hides
which files Machino owns behind three operations and reports a **live** state
that is derived from the camera, never from a cached value.

```
machino-manager install   [--owner NAME] [--platform P]
machino-manager uninstall [--owner NAME] [--force]
machino-manager status
```

`status` prints one line of JSON to stdout, e.g.:

```json
{"schema":1,"product":"machino","state":"ON","version":"0.11.0","platform":"t40nn",
 "managedBy":"cam-tool","selected":"machino","running":true,"majesticWasEnabled":true,
 "components":{"binary":true,"version":true,"init":true,"config":true,"manifest":true}}
```

## The four states

Determined live from the binary, `machino --version`, the init script, the
config, and a versioned ownership manifest (`/etc/machino/install-state.json`):

| state | meaning | toggle shows |
|-------|---------|--------------|
| `OFF` | nothing installed | Off |
| `ON` | our manifest **and** all components healthy **and** `selected == machino` **and** `running == true` | On |
| `BROKEN` | our manifest present but any ON condition fails (missing binary, not selected, not running, …) | On + error badge — never a false Off |
| `EXTERNAL` | Machino present but **not** installed by us (no manifest) | toggle stays usable: switching ON **takes it over** (installs our bundle on top, writes the manifest); only uninstall refuses it |

`ON` means **truly active**, not merely "files present": Machino must be the
selected streamer and actually running. `install` verifies this and returns a
non-zero exit with `state:BROKEN` if activation did not take — so the toggle can
never show a false green. `BROKEN` also catches a crashed/half-installed Machino
(never a misleading `OFF`).

`install` is valid from `OFF`, `BROKEN` **and** `EXTERNAL` alike: it stops a
running daemon (a running ELF cannot be overwritten), re-installs the bundle
(an existing `machino.conf` is never clobbered), activates, and writes the
ownership manifest — the install is ours from then on. Ownership gates only
the destructive direction: `uninstall` refuses an installation it does not own.

A caller (the Cam-Tool) adds one more state of its own: **`UNKNOWN`** when
`machino-manager status` could not be run at all (no shell, timeout) — the
toggle is then disabled, never shown as Off.

## Ownership

`install` writes the manifest with `managedBy` set to `--owner` (default
`cam-tool`). `uninstall` refuses to remove an installation owned by someone else
(or one with no manifest at all) unless `--force` is given. This is what makes
the Cam-Tool toggle safe: turning it off removes **only** what the tool
installed, and restores Majestic to exactly its pre-install state (recorded at
install time, not guessed).

## How the Cam-Tool uses it

- **Detect**: run `machino-manager status` on the camera and map `state` to the
  toggle. Live detection always wins over any stored UI value.
- **Turn ON**: deploy the bundle, run `machino-manager install --owner cam-tool
  --platform t40nn`, reboot if required, then `status` again — only report
  success once `state` is `ON`.
- **Turn OFF**: run `machino-manager uninstall --owner cam-tool`, reboot if
  required, then `status` again — only report success once `state` is `OFF`.

`status` and `uninstall` also work standalone on the camera after install
(installed to `/usr/sbin/machino-manager`, with a stored `uninstall.sh` under
`/etc/machino`); `install` must be run from the deployment bundle.

## Upgrade safety

`machino.conf` is never overwritten on reinstall/upgrade (the shipped default is
kept as `machino.conf.default`). On a fresh install, an existing
`majestic.yaml` is imported once (see [majestic-compat.md](majestic-compat.md)).
The config format is additive: unknown keys are ignored, missing keys default,
`config.revision` is preserved — so a new binary reads an old config and vice
versa.
