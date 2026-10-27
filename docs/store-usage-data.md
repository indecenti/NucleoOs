# Store: private usage data (design note)

The Cardputer app store must let the maintainer **release new apps, update existing ones, and — if it can be
done privately — see useful usage data**. The first two already work; the third is a design, not yet built.
This note records what exists and how private usage data could be added without breaking the store's
guarantees. It is a requirement/design note, written to be handed to whoever owns the store code — it does
not change the store.

## What already works (release + update)

These are documented and shipping (see `docs/app-store.md`, `docs/native-apps.md`, and the store repo's own
`README.md` / `docs/rules.md` / `docs/format.md`):

- **Release a new app.** Add a folder under `apps/<channel>/<id>/` (a `manifest.json` + the built files:
  a `.nap` for the Native C channel, the web bundle for the Web channel), then
  `python tools/store.py publish` in the store repo: it builds `public/`, signs each package offline
  (ECDSA P-256, keys in `~/.nucleo`), and verifies. Pages serves the signed catalog.
- **Update an existing app.** Raise its `version_code` (a published `version_code` is immutable — the device
  refuses a changed package at the same code), rebuild/republish the same way. The device offers the update;
  the install keeps the id, so recents / pins / saves carry over. The catalog must be re-signed at least
  every 120 days (`store.py sign`) or it expires.
- **No surprises on OS update** (`docs/app-store.md`, upgrade guarantees): an app leaves the firmware image
  only one release *after* the store client shipped, the SD zip of each release also carries the store
  packages (so a device can install with no network), and each release is verified as an upgrade on both
  Cardputers before it is published.

So the store already satisfies "release new apps" and "update existing ones". Only usage data is missing.

## Why usage data is not there yet

The store is a **static, signed catalog on GitHub Pages** with **no backend**: the device fetches signed
files over HTTPS and installs locally. Nothing reports back, by design — it is what makes the store simple,
offline-capable and verifiable. So "track useful data" cannot be a side effect of the current store; it needs
a new, deliberate channel, and it must stay private (the user asked for non-public data) and respect the
project's privacy rule (no personal data leaves the device without cause).

## A private-usage-data design (if we build it)

Goals: a few **useful, aggregate** signals (which apps get installed / opened, which OS version, board type),
**private** (only the maintainer sees them), **opt-in**, and **no personal data** (no SSIDs, no IPs stored,
no file names, no content).

Options, cheapest first:

1. **On-device only (no network).** The device keeps local counters (installs, launches per app) viewable in
   the Store app itself. Zero privacy risk, zero infrastructure — but the maintainer only sees a device they
   hold. Good first step.
2. **Opt-in private ping to a maintainer endpoint.** With the user's consent, the device POSTs a tiny,
   anonymous payload (random install-id, OS version, board, app id + event = install/open, coarse count) to a
   **private** collector the maintainer runs (not Pages, not public). The collector stores aggregates only.
   Constraints: off by default; a clear toggle in Settings; no PII; batched and rate-limited; the endpoint and
   its data are private (auth on write, never served publicly); the device must work fully with it disabled.
3. **Private analytics repo (no server).** The device writes nothing; instead the maintainer, from their own
   devices, exports the local counters (option 1) into a **private** git repo. No third-party, no live
   endpoint, but only covers devices the maintainer syncs.

Recommended path: build **option 1** now (local counters, useful immediately, no privacy cost), and keep
**option 2** as an opt-in addition if real fleet data is needed — designed with the privacy rule up front.

## Open decisions (for the store owner)

- Which signals are actually useful enough to justify collecting them?
- Option 1 only, or also the opt-in ping (option 2)? If 2: where does the private collector live, and who
  holds its data?
- Consent UX: where the toggle lives and its default (must be off / opt-in).
