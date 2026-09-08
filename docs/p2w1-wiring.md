# P2-W1 wiring notes: HexPass identity and encounter storage

Status: built, tested, **not wired into the running firmware**. Nothing calls
HexPass yet. That is intentional: the UI is P2-W2, and the radio is blocked on
human sign-off of `docs/hexpass-threat-model.md`.

**There is no BLE code in this workstream.** No advertising, no scanning, no
NimBLE, no `esp_ble_*`. See "What the radio layer must do" at the bottom for
the constraints that apply when that work is unblocked.

## What was added

| File | Purpose |
|---|---|
| `src/social/hexpass_types.h` | Persisted structs, sizes, field ranges, hex codec. No project dependencies. |
| `src/social/hexpass_crypto.h/.cpp` | HMAC-SHA256. mbedtls on target, portable SHA-256 off target. |
| `src/social/hexpass.h/.cpp` | Identity, payload build/parse, encounter ring, block list, settings, wipe. |
| `test/test_hexpass/test_hexpass.cpp` | 38 tests. |

Edited: `src/pet/pet_core.h` (schema v3, `HexPassState hexpass` in `PetState`),
`src/pet/pet_core.cpp` (serialise, load with range checks, `fromVersion < 3`
migration step). `src/config.h` needed no change.

`hexpass_types.h` is split from `hexpass.h` on purpose. `PetState` needs the
structs, and `hexpass.cpp` needs `PetState`; only splitting them keeps that from
being a circular include. The hex codec is header-inline in the same file so
`pet_core.cpp` can serialise byte arrays **without gaining a link dependency on
the crypto unit**, which is what keeps the ten pre-existing test suites building
unchanged.

## Measured cost (T-Dongle S3, `lilygo-t-dongle-s3-vendor-app`)

| Build | RAM | Flash |
|---|---|---|
| Baseline at `9e961d6` | 80900 | 1194809 |
| This branch, HexPass compiled but unrouted | 81460 (+560) | 1200797 (+5988) |
| This branch, link probe forcing full linkage | 81868 (+968) | 1205013 (+10204) |

RAM moves even unrouted because `HexPassState` lives inside `PetState`, which is
a static member of the `PetCore` singleton, so the linker cannot strip it. The
+560 bytes is the store itself (32-byte secret, 16-entry encounter ring, 8-entry
block list, settings). The remaining +408 bytes of RAM and +4216 bytes of flash
are the engine, and only appear once something calls it.

The probe was `src/social/hexpass_linkprobe.cpp`, a file-scope
`__attribute__((constructor))` touching every public entry point. A constructor
lands in `.init_array`, which the link script KEEPs, so it is a GC root and
drags in what a real caller would. **The probe was deleted after measuring** and
the tree rebuilds at 81460 / 1200797. Its numbers are therefore an upper bound:
the probe's own body is inside them.

Total worst case is 968 bytes of RAM, 0.3% of the 327680 available. Everything
is fixed size; no path allocates.

## How to wire it (P2-W2)

`main.cpp` is off-limits to this workstream, so this is the handoff.

1. **After the save has loaded**, once, next to the other `begin()` calls:
   ```cpp
   #include "social/hexpass.h"
   ...
   HexPass::instance().begin();
   ```
   Order matters. `begin()` reads the secret out of `PetState`, so it must run
   after `StorageModule` has loaded the pet, or it will mint a second identity
   and orphan the first.

2. **If a save is ever loaded after `begin()`** (slot recovery, import), call
   `HexPass::instance().afterLoad()` to drop volatile state. `begin()` already
   does this; `afterLoad()` exists so a late load does not have to rotate the
   epoch a second time.

3. **Guard the feature on `selfTest()`.** If the crypto backend fails its
   known-answer test, every identifier this build derives is untrustworthy and
   HexPass must stay off rather than broadcast something weak:
   ```cpp
   if (!HexPass::selfTest()) { HexPass::instance().setEnabled(false); }
   ```

4. **Settings screen** needs, at minimum: an opt-in toggle (`setEnabled`, and it
   must read as OFF on a device that has never been asked), a private-mode
   toggle (`setPrivateMode`), badge and greeting pickers bounded by
   `HEXPASS_BADGE_COUNT` / `HEXPASS_GREETING_COUNT`, a block action per
   encounter, and a wipe with a confirmation step.

5. **The "what am I broadcasting" screen is not optional.** The threat model
   says "the device must be able to say plainly what it is broadcasting. A user
   who cannot inspect it cannot consent to it." `describePayloadHex()` returns
   the exact 33 bytes as hex and deliberately does **not** advance the counter,
   so looking is not sending.

6. There is no menu entry, no `Screen` enum slot and no `MENU_ORDER` change in
   this workstream, since `src/ui/ui_menu.*` was off-limits.

## Two things the threat model left unstated, resolved here

Both are flagged rather than silently chosen.

1. **The encoding of `epoch` in the EID message.** The spec writes
   `HMAC-SHA256(secret, "hexpass-eid" || epoch)` without saying how `epoch` is
   serialised. Fixed here as **4-byte little-endian**, and the label is written
   as an explicit byte array with no NUL terminator. Any encoding works as long
   as both ends agree; what would not work is leaving it to whoever writes the
   radio layer.

2. **What "the epoch key" for the tag is.** The spec says the tag is "an HMAC
   under the epoch key" but never defines that key, and it cannot be
   secret-derived: the receiver does not have the sender's secret and never will
   without a pairing handshake, so a secret-keyed tag would be unverifiable by
   design. Resolved as:
   ```
   tagKey = HMAC-SHA256(EID, "hexpass-tag")
   tag    = HMAC-SHA256(tagKey, payload[0..24])[0..7]
   ```
   The EID is public in the payload, so **anyone can compute a valid tag**. The
   tag detects EDITS to a captured card; it authenticates nothing. That matches
   what the spec already concedes ("It does not prove identity to a stranger,
   and is not claimed to"), but it is worth stating in the code as well, because
   a reader who assumes the tag proves origin will build the wrong thing on top
   of it. Replay detection and rate limiting, not the tag, are what stop a
   forged card mattering.

## Ambiguities resolved toward the stricter reading

1. **"at most one recorded per epoch, and at most N per day."** Read literally,
   both clauses govern "encounters from one EID", but an EID does not survive a
   day, so the second clause cannot mean what it says. Both are enforced: a
   per-friend daily cap (`HEXPASS_MAX_PER_FRIEND_PER_DAY` = 4) **and** a global
   device-wide daily cap (`HEXPASS_MAX_PER_DAY` = 32). Only the global one
   actually bounds the ring against a flood of unfamiliar cards. "Day" is
   `PetState::questDay`, the pet's existing notion of a day; there is no RTC and
   inventing one would be a fabricated fact.

2. **"Private mode: a one-action toggle that stops advertising immediately."**
   Implemented as stopping advertising *and* recording. A device that keeps
   building a social graph while the owner believes it is private is the worse
   of the two readings, so it is not the one that shipped.

3. **"eviction is oldest-first."** Implemented as oldest = **least recently
   seen**, not first-created. Pure FIFO would discard a friend you meet daily
   simply because you met them first. Under an attacker flood the two are
   identical, because flood entries are always the most recent.

4. **Wipe does not change the consent flags.** A wipe is a reset of identity,
   not a withdrawal of consent, and silently switching the feature off would be
   a different action from the one the owner asked for. It does mint a fresh
   secret and keeps the epoch moving forward.

## Schema v3

`PET_SCHEMA_VERSION` is 3. The `fromVersion < 3` step converts nothing: every
`HexPassState` field already holds its correct default. It is explicit about the
two that matter, `enabled = false` and no secret, because an existing pet must
not wake up after a firmware update quietly broadcasting an identifier in
public. An upgrade is not an owner enabling something.

Every loaded field that indexes a table is range-checked: `badge`, `greeting`,
and per encounter `stage`, `form`, `badge`, `greeting`. The asymmetry is
deliberate and tested:

* **A received card is rejected** when a field is out of range. Clamping would
  store a value the sender never sent.
* **A loaded save is clamped**, not dropped. The record already exists and its
  FriendID and meet count carry the meaning; losing a real friendship over one
  corrupt display byte is the worse trade.

A row whose FriendID will not decode is dropped entirely, because it identifies
nobody, and a zero FriendID would collide with every other broken row. A block
entry that will not decode is dropped rather than inserted as zeros, which would
otherwise be a block on the all-zero FriendID rather than on whoever the owner
actually refused. A malformed secret yields **no** secret, not a partial one:
half a secret still derives stable-looking EIDs, which is worse than none
because it looks like it works.

## Known limitation: the secret is stored in the clear

`hexpass.secret` is written to `pet_state_{a,b}.json` as hex. There is nowhere
better on this hardware: no secure element, and no flash-encryption key the
firmware could withhold from itself. **Anyone holding the SD card can derive
this device's past and future EIDs**, which defeats rotation for that attacker.

This is out of scope for P2-W1 but should not be discovered later. Options, none
free: keep the secret in NVS with flash encryption enabled (target-only, and the
simulator then needs a different path), or accept it and say so in the product
documentation. It needs the same human decision as the residual risks.

## What the radio layer must do, when it is unblocked

1. **Rotate through `HexPass::rotate()` and nothing else.** Register the BLE
   address change with `setRotationHook()`. It is called from inside `rotate()`
   after the epoch has advanced, so the address and the EID change in the same
   instant with no window between them. **Do not add a second timer.** The
   threat model calls this the single most likely way to get HexPass wrong, and
   `test_rotation_is_one_trigger_not_two_timers` is what stops it being added
   quietly.
2. **The BLE address must be a non-resolvable private address.** A rotating
   payload identifier behind a static public MAC accomplishes nothing.
3. **Check `broadcasting()` before every transmission**, not once at startup.
4. **Feed received bytes through `parsePayload()` then `record()`.** Do not
   write the encounter ring from anywhere else.
5. Nothing on the never-transmitted list may be added to the payload. The
   payload is a fixed 33 bytes with no optional fields; a variable-length
   payload leaks in its length alone.

## Test coverage

`pio test -e native -f test_hexpass`, 38 tests, all passing. All ten
pre-existing suites still pass.

Covered: RFC 4231 HMAC vectors (cases 1, 2, 4 and 6, so the portable backend is
pinned to the same published answers mbedtls is), EID stable within an epoch and
different across epochs, EID reproducible for a past epoch, rotation as a single
trigger observed by the hook, boot advancing the epoch, FriendID
order-independence, 33-byte round trip, absence of the pet name in the bytes,
counter advance and per-epoch restart, every tag-covered byte tampered one at a
time, unknown versions, wrong lengths, out-of-range fields rejected, all-zero
EID refused, one record per epoch, replay of a captured card and of a rewound
counter, our own card refused across a rotation, ring eviction under a flood
with the count pinned, daily cap and its release on a new day, blocked FriendID
retroactively removed and never re-recorded, bounded block list, opt-in
defaulting off, private mode stopping both directions, bounded badge and
greeting, inspection without transmission, wipe making old identifiers
underivable, save round trip, v2 to v3 migration preserving a full pet, v1
migrating all the way, a hostile save unable to produce an out-of-range index,
oversized arrays unable to overflow, and a save unable to enable the feature by
omission.
