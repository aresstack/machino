# AP3 — RTSP authentication prepared

## What the oracle actually proves

`www/cgi-bin/stream-urls.cgi` in the unmodified webui states it outright:

> "These endpoints authenticate as user `root` with the same password you use
> for this WebUI. Players such as VLC ask for it when you open a bare URL."

and, for the other branch:

> "Authentication is switched off for every endpoint (`system.unsafe`) — anyone
> who can reach the camera can open these URLs."

`main.js` picks between the two notes on `system.unsafe` from
`/api/v1/config.json`.

So, **proven**:

- There is **no separate RTSP account**. The credential is the *system*
  account (`root`), the same one the WebUI login uses - which in Machino is
  already `shadow_check` (crypt(3) against `/etc/shadow`).
- The kill switch is the **global** `system.unsafe`, not an rtsp-specific
  flag, and it disables auth for *every* endpoint, not just RTSP.
- Default is authenticated (`unsafe` off).

## What follows from that (derived, not guessed)

Digest needs `HA1 = MD5(user:realm:password)`, i.e. the plaintext password or
a stored HA1. `crypt(3)` against `/etc/shadow` is one-way: it can only answer
*"is this password correct?"*. A camera whose RTSP credential **is** the
system account therefore **cannot serve Digest from /etc/shadow** - Basic is
the only scheme that works with this credential source.

Machino follows that: `offer_basic` defaults on, `offer_digest` defaults off
and is only usable when an explicit `Ha1Fn` (a stored secret) is supplied.
The Digest verifier is fully implemented and tested against that provider, so
if the capture shows Majestic serves Digest from some other secret, only the
provider has to be written - no protocol work.

## BLOCKED_T31_CAPTURE

Still unknown, deliberately not invented:

1. **Which challenge Majestic actually emits** - Basic only, or Digest too
   (which would imply a stored secret we have not found).
2. **The realm string** it uses (Machino uses `Machino`).
3. Whether it sends `stale=true` on nonce expiry, and its nonce lifetime.
4. Whether `OPTIONS` is challenged or answered unauthenticated (Machino
   currently answers `OPTIONS` without auth and gates
   `DESCRIBE`/`SETUP`/`PLAY`, which is the common reading of RFC 2326 but is
   not proven for Majestic).

**What the capture must show**: one full `DESCRIBE` exchange against a
Majestic camera with auth on - the `401` response headers verbatim (every
`WWW-Authenticate` line) and the client's follow-up `Authorization` header.
That single exchange answers 1-3; a bare `OPTIONS` answers 4.

## Implemented

- `src/app/rtsp/rtsp_auth.{hpp,cpp}`: the whole decision, isolated from the
  parser and the socket code. `RtspAuth::Ctx` is **per connection** (nonce +
  authorised flag) and dies with the socket.
- `RtspAuth::CheckFn` is the same `(user, password) -> bool` the WebUI session
  gate uses; `main.cpp` passes `shadow_check`. Machino stores **no RTSP
  password of its own** anywhere - not in config, not in diagnostics.
- Basic (RFC 7617) verified against that callback; Digest (RFC 7616, MD5,
  qop=auth) verified against an optional HA1 provider, with nonce lifetime and
  `stale=true`. Digest comparison is length-checked and constant-time-ish.
- `rtsp.auth` config key, default **false**: AP3 prepares, it does not flip
  the switch. Full drop-in parity means auth-on-by-default driven by
  `system.unsafe`, which belongs to the `/setup` + `system.unsafe` package.
- Server wiring: the check runs in `handle_request` right after the mount is
  resolved and **before any path that can take demand or subscribe a sink**,
  so an unauthenticated client can never start the sensor. `401` carries one
  `WWW-Authenticate` line per offered scheme and keeps the connection open for
  the retry. Identical for main and sub - the mount does not change the rule.
- Nothing logs a credential: a refusal logs peer, method and unit only.

## Tests (host, 1247/0)

`test_rtsp_auth.cpp`: auth-off never even calls the credential source;
missing header -> `Missing` + a Basic challenge; wrong user, wrong password,
empty password, non-base64, no colon, unknown scheme -> all `Bad`; correct
credential -> `Ok` and the context becomes authorised; the same credential
works across `DESCRIBE`/`SETUP`/`PLAY` and across `/ch0`, `/ch1`, `/stream=0`,
`/stream=1`; two contexts are independent and get different nonces; a missing
credential source refuses rather than failing open; with an HA1 provider a
correct Digest response is accepted while a wrong password, an unknown user,
a foreign nonce and an expired nonce are refused (and `stale=true` is
offered); header parameter parsing incl. a lookalike name; the digest formula
is deterministic, password-sensitive and qop-aware.

## Not in AP3

The capture itself, new codecs, audio/backchannel, ONVIF, TLS, WebUI changes.

## Open (NEEDS_HUMAN)

- The capture above (`BLOCKED_T31_CAPTURE`).
- Hardware acceptance of the wiring with `rtsp.auth=true` (VLC prompt, wrong
  password refused, no encoder start before authorisation). Not run tonight.
