# AP10 — `/setup` and the unclaimed / first-run flow

Contract taken from the upstream oracle clone (`www/setup.html`,
`tests/setup-page.test.js`, `CLAUDE.md` § *Authentication* → *Unclaimed cameras
(first boot)*). Nothing here is inferred from majestic's observed behaviour.

## State machine

```
UNCLAIMED  ──  POST /setup, password set AND verified  ──▶  CLAIMED
```

**The state is not stored anywhere of ours.** Upstream is explicit: the claim
state *is* the emptiness of root's hash field in `/etc/shadow` — "nothing else
records the state, so whichever door sets the password, the other sees it
immediately". The other door is SSH, gated firmware-side by `openipc-claim` as
root's login shell. A second state file here would be a second truth that can
disagree with the one the firmware and SSH already share, so there is none.

`claim_state()` reads that one record and **fails closed**: if `/etc/shadow`
cannot be read it reports CLAIMED, so a camera that cannot answer the question
never opens an unauthenticated page that sets the root password.

There is **no un-claim** in this package. The only way back is the firmware's
own mechanism, which AP10 does not touch.

### What each state serves

| | UNCLAIMED | CLAIMED |
|---|---|---|
| `POST /setup` | handled | **403** |
| `GET /setup.html` | relayed (the stock page) | **404** |
| `GET /eula.<lang>.txt`, `/favicon.ico` | allowed | normal rules |
| every other path, browser navigation | **302 → `/setup.html`** | normal rules |
| every other path, `fetch`/CLI/WS | **401** | normal rules |

"An unauthenticated page that sets the root password must not outlive the state
that justifies it" — hence the 404 and the 403 once claimed.

The claim gate runs **before** the session gate, because on an unclaimed camera
there is no credential that could satisfy it.

## `POST /setup`

`application/x-www-form-urlencoded`, exactly the fields `setup.html` sends:

| Field | Required | Rule |
|---|---|---|
| `password` | yes | 8..128 characters |
| `confirm` | yes | must equal `password` |
| `eula` | when a licence document is on the image | must be `accepted` |
| `eula_lang` | with `eula` | recorded, not validated |
| `sshkey` | no | one line, public key of a known type |

Responses follow the page's own reading of them:

- **200, empty body** — claimed and signed in; the page navigates to `live.cgi`.
- **200, non-empty body** — claimed, *but something was left undone*. The page
  stays put and shows the text. This is the channel for "the key was not
  installed"; it must never be a 500, because the camera really is set up.
- **non-2xx, plain-text body** — refused. The page prints what the camera said
  rather than inventing a message from the status code.

Validation, in order, all refusing before anything is written:

- empty → too short (< 8) → too long (> 128) → mismatch,
- **a colon or a line break is refused, not escaped**: the password is handed to
  the system as `root:<password>` on one line, so either would be a second
  record rather than a weak password,
- the EULA parameter is checked here too, even though the page already checked,
- the optional key is validated **before** the claim, so a refused key costs a
  400 on a camera that is *still unclaimed* — a form that can be corrected.
  `/setup` is gone afterwards, so there is no second chance through this flow.
  A private key is refused outright, and a multi-line value is refused because
  `authorized_keys` is line-oriented and the second line would be unreviewed.

After the write, the new password is **re-authenticated**. Upstream does this
and the reason is worth keeping: an exit status is not proof the hash was
written. A write that silently did not land answers 500 and says the camera is
still unclaimed — it does not report success and does not mint a session.

**Credentials are never logged.** The only line written is
`setup -> <status>` plus whether a session was minted.

## Persistence

`root:<password>` is piped to **`chpasswd`**, which is what the stock flow does.
That keeps the on-disk write — temp file plus rename, inside `chpasswd` —out of
this process, so there is no half-written `/etc/shadow` of our making and the
file's existing ownership and mode are preserved. The password reaches no
command line, no environment variable and no log; only that pipe. `SIGPIPE` is
blocked process-wide, so a `chpasswd` that dies early surfaces as `EPIPE`
instead of killing the daemon.

Persistence across restarts needs nothing extra, because the state is the
shadow record itself. The host tests simulate a restart by building a second
gate over the same camera state.

## `system.unsafe`

Upstream schema: `boolean`, default `false`, title *"Disable authentication"*.
`CLAUDE.md` gives its reach: it "overrides everything, unclaimed cameras
included — that, not a blank password, is how a deliberately-open camera is
configured".

Implemented exactly that way and no further: when `system.unsafe` is set, both
the session gate **and** the unclaimed redirect are skipped. `POST /login` and
`POST /logout` stay wired even then, because they are routes the stock UI calls
and relaying them upstream would be a different answer rather than an absent
one. No other meaning is invented for the flag.

## Auth / session integration

One credential store, the system account, as before. The setup flow does not
get its own.

`SessionGate::mint()` was added: a session **without** a credential check. Its
one caller is the setup flow, which has just set the password and then
re-authenticated against it — the check has already happened, and repeating it
would mean handing the plaintext around a second time. The cookie is the same
one `login()` issues (`session`, `HttpOnly`, `SameSite=Strict`), so everything
downstream is unchanged, and the tests assert the minted token is one the gate
actually accepts.

## Tests — host, 1641/0 (was 1498)

`tests/test_setup.cpp`: fresh UNCLAIMED state and the exact set of paths it
serves (including that `/api/v1/*`, `/login`, `/login.html` and `/ws/video` are
*not* among them, and that an `eula` path cannot climb out of the web root);
claim with valid data → CLAIMED, empty body, session minted; a second setup →
403 that runs no work and leaves the original password; seven bad-input cases
plus the exactly-8 boundary; oversized password and oversized body; four
passwords that would forge a second shadow record; the EULA gate both present
and absent on the image; a write that reports success and silently did not land
(→ 500, still unclaimed); the helper simply failing; six refused SSH keys, each
proving the camera was **not** claimed first; a good key installed; the
installer failing *after* the claim (→ 200 with a message, claim stands); no
installer wired (→ 200 with a message, key never silently dropped); restart
persistence; login through the real `SessionGate` before and after the claim,
and that a minted cookie is accepted; `system.unsafe` parsing including that an
unparseable value never opens it.

## Remaining differences from Majestic / OpenIPC

1. **No SSH key installer is wired.** A key offered during setup is reported
   back as "claimed, but the key was not installed" rather than silently
   dropped. Writing `authorized_keys` is deliberately left out of this package.
2. **The no-script form post gets the same status as the `fetch` path**, not
   the **303** majestic answers there. The scripted path — which is what the
   page uses whenever JavaScript runs — is exact; the no-script fallback would
   land on a blank page with the text instead of being redirected.
3. ~~RTSP and ONVIF are not gated on the claim state.~~ **CLOSED.** ONVIF was
   gated in AP11, and RTSP in a follow-up commit: `RtspAuth` now takes a claim
   predicate, `required()` is true whenever the camera is unclaimed *regardless
   of `rtsp.auth`*, and `check()` refuses every credential outright in that
   state rather than relying on `/etc/shadow` happening to reject them.
   `system.unsafe` outranks both, as upstream specifies. The predicate is
   consulted per request, not cached, so a camera claimed over SSH starts
   serving without a daemon restart — which is what keeps the two doors in
   agreement.
4. **Camera-local callers are still waved through while unclaimed**, matching
   the existing Majestic local-trust rule that the CGIs depend on. On an
   unclaimed camera that means a local process has full access — defensible,
   since a local process is already root-equivalent, but it is a deviation from
   "every HTTP path but the claim flow gets 401" and is recorded as one.
5. **`eula_lang` is recorded, not acted on.** Nothing here selects or stores a
   language; the parameter is accepted so the stock form is not rejected.
6. **No hardware acceptance.** Nothing was deployed and no camera was claimed:
   doing so would set a real root password on the test camera. The flow has
   only ever run against the host fakes. **`NEEDS_HARDWARE_ACCEPTANCE`**, and
   it needs a camera that can be re-flashed, not the working one.
