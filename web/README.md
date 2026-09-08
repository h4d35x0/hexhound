# HexHound Web Flasher

Browser-based flashing via [ESP Web Tools](https://esphome.github.io/esp-web-tools/):
pick a board, plug it in over USB, hit install. Flash progress drives the pet
through its five evolution stages, using the same HD art the firmware ships.

## Build and serve it locally

The page is committed; the binaries are not (they are build output, 1.3 MB to
4 MB each). Stage them from a real firmware build:

```bash
python scripts/build_flashes.py 0.2.0-beta2      # builds every board image
python scripts/stage_web_flasher.py 0.2.0-beta2  # copies them + writes manifests
python -m http.server -d web 8000
```

Then open <http://localhost:8000> in Chrome or Edge.

`localhost` counts as a secure context, so Web Serial works locally without
HTTPS. Everything else needs real HTTPS.

## What is generated vs committed

| Path | Committed? | Notes |
|------|-----------|-------|
| `index.html`, `img/` | yes | the page and the five stage portraits |
| `manifests/*.json` | no | generated per board by `stage_web_flasher.py` |
| `firmware/*.bin` | no | copied from `firmware/HexHound-Firmware-v<version>/` |
| `boards.json` | no | drives the board picker, so adding a board to a firmware build needs no HTML edit |

Regenerating is always safe: the manifests are derived from the binaries that
are actually present, so they cannot drift.

## Known constraints

- **Chrome or Edge on desktop only.** Web Serial does not exist in Firefox or
  Safari, or on mobile. The page detects this and points at the esptool route.
- **Every ESP32-S3 image reports the same chip family**, so ESP Web Tools
  cannot tell a T-Dongle from a Waveshare from a T-RGB. That is why the page
  has its own board picker rather than relying on chip detection - picking the
  wrong board writes a working image with the wrong display pins, which looks
  like a dead screen. The display names in `stage_web_flasher.BOARDS` are the
  only disambiguator the user gets, so they have to name the physical board.
- **Some boards need a note of their own.** `stage_web_flasher.POST_FLASH_NOTES`
  maps a board id to guidance that lands in `boards.json` as an optional `note`
  field; the page shows it when that board is picked and again when the flash
  finishes. The T-RGB has one because it parks in ROM download mode after every
  write and stays dark until it is reset by hand - the same symptom as flashing
  the wrong image, so it has to be called out or it reads as a failure.
- **ESP32-C5 support is unverified.** The C5 manifest declares
  `chipFamily: "ESP32-C5"`; if esptool-js in the pinned ESP Web Tools release
  does not know that chip, the button will error. That is web tooling, not the
  image - the C5 binary flashes fine with esptool.
- **Flashing wipes the pet.** The images are full merged flash writes.

## OPEN: the T-RGB has no image producer yet

`stage_web_flasher.BOARDS` lists the T-RGB and the page renders it, but
`scripts/build_flashes.py` does **not** build it: its `MODELS` list has no
`lilygo-t-rgb` entry, so no `hexhound-t-rgb-merged.bin` is ever produced and
the staging step reports the board as NOT STAGED.

Because `deploy_web_flasher.py` imports `BOARDS` as its single source of truth
and refuses an incomplete board set, **the next deploy will be refused** until
that entry exists. That refusal is correct behaviour, not a bug: it is the
`--only` trap firing on a board whose image was never built.

The missing line, in `MODELS` in `scripts/build_flashes.py`:

```python
("lilygo-t-rgb", "t-rgb", "LilyGo T-RGB 2.1in Round", None),
```

`None` for the core dir is right: the T-RGB pins `espressif32@6.12.0`, the
stock platform, so it does not need the C5's quarantined PlatformIO home. The
basename `t-rgb` is what `stage_web_flasher.BOARDS` already expects.

## Hosting

**LIVE: https://h4d35x0.github.io/hexhound-flasher/**

Deployed 2026-08-03 with firmware v0.4.2, all seven boards.

GitHub Pages needs a public repo (or a paid plan for private Pages). This repo is
private, so the deployment is a **separate public repo** containing only this
directory's contents plus the staged binaries and no source:
[h4d35x0/hexhound-flasher](https://github.com/h4d35x0/hexhound-flasher).
That decouples handing strangers a flasher from publishing the firmware source.

### Redeploying after a firmware change

**Re-staging locally is NOT deploying.** `web/firmware/`, `web/manifests/` and
`web/boards.json` are gitignored, so they exist only in your working tree.
Nothing you do in THIS repo changes what the live page serves, and merging a PR
here does not either. Forgetting the second half is how a stale pre-OTA C5 image
sat on the flasher for a day.

Three commands:

```bash
python scripts/build_flashes.py <version>        # build every board image
python scripts/stage_web_flasher.py <version>    # stage + regenerate manifests
python scripts/deploy_web_flasher.py <version>   # publish, then verify live
```

`deploy_web_flasher.py` copies the payload into the public flasher repo,
regenerates the version and hash table in its README, pushes, waits for Pages,
and then **re-downloads every image from the live URL** to confirm the served
bytes match what was built. Add `--dry-run` to see the diff without publishing,
or `--verify-only` to check the live site without touching it. Re-running when
nothing changed is a no-op.

It is a gate rather than a copier, and refuses to publish when:

- the staged board set is **incomplete** (the `--only` trap below)
- `boards.json` is not the version you asked for
- an image's bytes do not match its recorded sha256 or size
- an image has **no `HEXHOTA1` magic**, meaning no OTA receiver, so it could
  never be updated over the air. This is exactly the stale-C5 defect.
- **private signing key material appears in any image.** A leaked OTA signing
  key lets anyone sign firmware every unit in the field will accept.
- it **cannot run** that secret check, or cannot show the check works. The byte
  search is validated by confirming it DOES find the public keys, which are
  compiled in and must be there, because "I did not look" must never read as
  "it is clean".

Every one of those refusals has a test, because a gate that has never fired is
not known to work:

```bash
python scripts/test_deploy_gates.py     # 12 checks, no network, no pushing
```

**Do not run `stage_web_flasher.py` after a partial build.** It writes
`boards.json` from only the images present, so a `--only` build silently drops
every other board from the live picker while leaving their `.bin` files behind.
The deploy script refuses this rather than shipping it, but the staging step will
still have overwritten your `boards.json`.
