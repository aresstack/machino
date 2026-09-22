# AP11 — ONVIF minimal stack

## What the WebUI actually contributes

The unmodified majestic-webui has **no ONVIF protocol contract at all** — only
configuration. Read out of the upstream clone:

| Key | Type | Upstream default | Upstream hint |
|---|---|---|---|
| `onvif.enabled` | bool | `true` | — |
| `onvif.username` | string | `root` | "Matched against the cleartext password below when set; otherwise falls back to the `/etc/shadow` lookup." |
| `onvif.password` | string | `""` | "Opt-in, default empty. When set, unlocks WSSE PasswordDigest and HTTP Digest auth so legacy clients (ODM v2.2.x) and digest-only ones (tinyCam Monitor) work. Stored as cleartext in this config file — only enable if you accept that." |

Two further constraints come from elsewhere in the clone:

- **Unclaimed cameras**: "it streams nothing, answers RTSP and ONVIF with 401".
- **`system.unsafe`**: "overrides everything, unclaimed cameras included".

Everything else here comes from the ONVIF specification, not from the WebUI.

## Scope — and what is deliberately absent

Implemented: the operations without which no client works at all.

| Service | Operations |
|---|---|
| Device | `GetSystemDateAndTime`, `GetDeviceInformation`, `GetCapabilities`, `GetServices`, `GetScopes` |
| Media | `GetProfiles`, `GetStreamUri`, `GetVideoSources`, `GetVideoEncoderConfigurations` |

**Not implemented, and answering a proper `ter:ActionNotSupported` fault rather
than a wrong answer:** PTZ, Events, Imaging, Recording, Analytics, and
`GetSnapshotUri` — there is no JPEG path, by mandate, so a snapshot URI would
be a URL that does not work.

`GetCapabilities` advertises **only** Device and Media. Advertising Events or
PTZ would have clients call operations that fault.

**WS-Discovery (UDP 3702) is not implemented.** A client must therefore be
pointed at the camera by address rather than finding it by probing. This is the
single largest functional gap and it is called out again at the bottom.

## Architecture

```
POST /onvif/{device,media}_service
        ↓
  soap.cpp      scanner: operation + WS-Security token
        ↓
  onvif_service.cpp   authenticate → dispatch → SOAP response
```

Both units are pure and host-tested: no sockets, no clock of their own, no
system access. The credential check, the time and the stream facts are
injected, so **every** branch — including every way authentication can fail —
is exercised on the host.

The route is handled **before both HTTP gates**, because an ONVIF client speaks
neither the session cookie nor JSON: a 302 to `/login.html` or a JSON 401 would
be unintelligible. The same policy is applied, expressed in SOAP — the service
is handed the claim state and the unsafe flag and enforces them itself.

### The scanner is not a parser, on purpose

It runs on **unauthenticated input**, so it is the attack surface. It is a
bounded, non-recursive scanner with:

- a hard 64 KiB input bound,
- **`<!DOCTYPE` and `<!ENTITY` refused on sight** — entity expansion is the
  whole of the billion-laughs class, and nothing in ONVIF needs either,
- only the five predefined entities decoded; no numeric references, and an
  unknown entity is left as written rather than expanded,
- **comments skipped whole**. Without that, markup written inside a comment
  becomes the operation — a disagreement between this scanner and any real
  parser, which is exactly the confusion to avoid in code that runs before
  authentication. Found while reviewing the first working version, fixed, and
  pinned by a test.

## Authentication

| Credential | With `onvif.password` set | Without |
|---|---|---|
| WSSE **PasswordText** | compared against it | falls back to the `/etc/shadow` check |
| WSSE **PasswordDigest** | recomputed and compared | **`Unverifiable`** |
| HTTP **Basic** | compared against it | falls back to `/etc/shadow` |
| HTTP **Digest** | *not implemented* | *not implemented* |

`PasswordDigest` is `Base64(SHA1(nonce ‖ created ‖ password))`, reusing the
SHA-1 and Base64 already in the tree for the WebSocket handshake. It needs the
**cleartext** to recompute, which `/etc/shadow` cannot supply — so with no
configured password the answer is `Unverifiable`, not `Bad`. That distinction
is deliberate: the credential may well be correct and we simply cannot judge
it, and the fault text says so ("PasswordDigest needs onvif.password to be set
on this camera") instead of sending an operator hunting for a wrong password.

Replay defence:

- `Created` must parse as **plain UTC** and be within **±300 s**. An offset
  form, a space instead of `T`, a missing `Z` or an impossible month is refused
  rather than guessed at — a misparsed timestamp would silently widen the
  window.
- Nonces are remembered for the length of that window, capped at 256 entries so
  a flood of distinct nonces cannot grow the cache without bound.
- **The digest is compared before the nonce is recorded.** Otherwise anyone
  could burn a nonce by guessing first and lock out the real client. Pinned by
  a test.

`GetSystemDateAndTime` is unauthenticated, by spec and by necessity: a client
needs the camera's clock before it can build a `Created` that will pass the
freshness check. It is the *only* unauthenticated operation, and it still
answers on an unclaimed camera — the clock is not a secret, and a client that
cannot read it cannot even report why it failed.

## Deviation from upstream: default off

`onvif.enabled` defaults to **`false`** here, where upstream defaults to
`true`. This stack has never met a real ONVIF client, and a camera that
half-answers on the network is worse than one that stays silent — a client can
latch onto it and then fail in ways that are hard to attribute. When disabled,
`/onvif/*` answers a plain **404**, never a partial SOAP envelope. The default
flips to `true` once it is hardware-accepted.

## Tests — host, 1748 → 1750/0 (suite was 1641 before AP11)

`tests/test_onvif.cpp`, weighted towards the parts that can hurt: the scanner's
refusals (empty, oversized, DOCTYPE, ENTITY in either case); operation
extraction including prefix-agnosticism, empty bodies, non-XML, and markup
hidden in a comment; entity decoding and the refusal to expand an unknown one;
Base64 rejecting non-alphabet bytes and data after padding; XML escaping
including dropping control characters; the digest itself against independently
varied inputs; the UTC parser accepting exactly the UTC form and refusing five
near-misses including a leap day check; `GetSystemDateAndTime` answering
unauthenticated while everything else 401s; PasswordText against both credential
sources; PasswordDigest accepted, replayed, wrong-then-right (the nonce-burn
case), both ends of the skew window, the exact boundary, and missing
nonce/created/base64; HTTP Basic good, bad, malformed and absent; unclaimed
refusing every credential while the clock stays readable; `system.unsafe`
overriding both; disabled answering 404 with no envelope; all nine operations
including per-profile stream URIs, the absent-token default, an unknown token
faulting rather than guessing at another stream, and two unimplemented
operations faulting correctly.

## Remaining gaps — `NEEDS_HARDWARE_ACCEPTANCE`

1. ~~No WS-Discovery.~~ **Added** in a follow-up commit
   (`src/app/onvif/discovery.*`): Probe is parsed with the same scanner —
   a Probe arrives over UDP from anyone on the segment, so if anything it
   deserves more suspicion than the SOAP endpoint — and answered with a
   `ProbeMatches` that RelatesTo the Probe's MessageID. A Probe naming a type
   this camera is not is **ignored**, because answering would put it in a list
   it does not belong in. The endpoint `urn:uuid` is derived from durable
   device facts rather than stored, so it survives a restart without another
   state file that could disagree with reality. `Hello`/`Bye` are built too.

   The responder has its **own socket and its own thread**, not another fd in
   the HTTP poll loop: that loop carries the live media path for `/ws/video`
   and `/ws/webrtc`, and discovery is not worth any risk to it. The reply is
   unicast back to the sender, per spec, and the `XAddrs` it hands back uses
   the local address the kernel picks *for that peer* — the only answer that is
   right on a camera with more than one interface. Still unverified against a
   real client (gap 3).
2. **No HTTP Digest**, which the upstream hint mentions alongside WSSE
   PasswordDigest. Digest-only clients (tinyCam Monitor is named upstream) will
   not authenticate. The WSSE half is implemented.
3. **Never tested against a real client.** No ODM, no tinyCam, no VLC-via-ONVIF
   run has happened. Every response above is built to the specification and
   checked by unit tests, which is not the same thing as a client accepting it.
4. **Default off** until 1–3 are addressed, as described above.
5. **The cleartext password is stored in `machino.conf`.** That is upstream's
   own design and its own warning, repeated here rather than silently improved:
   turning it on is a decision with a cost.
6. ~~RTSP is still not gated on the claim state.~~ **CLOSED** in a follow-up
   commit, so the stream this service points at is now refused on an unclaimed
   camera too — ONVIF and RTSP finally say the same thing. See the AP10 note.
