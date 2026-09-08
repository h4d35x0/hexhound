# P3-W2: OTA update engine, wiring and operation

How firmware gets replaced on a HexHound, what each piece is responsible for,
and the things that will bite whoever touches this next.

`src/ota/ota_image.h` points here for the save-compatibility warning. It is in
"What the owner can lose" below.

## The rule everything else serves

HexHound is a giveaway device. No support channel, no RMA, and the person
holding it is not going to open a serial console. A device that will not boot
is not a bug report, it is electronic waste with someone's pet on it.

So every ambiguous decision in this subsystem resolves the same way: **do
nothing and keep running the firmware that already works.** A health check that
is too strict costs the owner an update. A health check that is too lax costs
them the device. Those are not close.

## Pieces

| File | Responsibility | Testable off-target |
|---|---|---|
| `ota_image.*` | The signed 192-byte envelope. Parse, bounds, Ed25519. | Yes, fully |
| `ota_session.*` | Order of operations and every bound. The state machine. | Yes, fully |
| `ota_target.*` | The seam to flash. `EspOtaTarget` on device, `MemoryOtaTarget` in tests. | The memory one is real, not a mock |
| `ota_health.*` | Post-boot trial, confirm or roll back. `OtaHealthPolicy` is the portable decision logic. | Policy yes, ESP layer no |
| `ota_identity.h` | What this build is: board id, version, slot size. | n/a |
| `ota_pubkey.h` | Generated. The trusted signing keys. | n/a |
| `ota_serial.*` | The USB serial wire, and the one owner of the session. `OtaFrameReader` is the byte state machine. | The reader and the CRC yes, `Serial` no |
| `ui/ui_update.*` | The owner-facing screen. The ONLY caller of `arm()`. | Renders in the simulator |
| `scripts/sign_ota_image.py` | Sign and verify on the host. | n/a |
| `scripts/push_ota.py` | Verify locally, then push over serial. | n/a |

The split exists so that every bound and every ordering rule lives in ordinary
portable C++ that a desktop test can drive through all of its failure paths,
instead of only being exercisable by flashing a real device and hoping.

## Keys

**Firmware and content use separate signing keys.** That was a deliberate call
and the reasoning is worth keeping: a leaked content key lets somebody write
dialogue, a leaked firmware key lets somebody own the device. Different blast
radius means different custody and different rotation.

The signature is additionally domain-separated (`HEXHOUND-FW-v1`), so a content
pack signature could never be replayed as a firmware signature even if the keys
were shared by mistake.

`keys/ota-dev-signing.key` is gitignored and is a DEVELOPMENT key. It must be
replaced before any build reaches anybody. An OTA key authorises replacing the
firmware; it is the most valuable key in this project.

Adding a trusted key is a firmware change, deliberately. Who may replace this
firmware is not data.

## Building and signing an image

```
pio run -e waveshare-esp32-s3-lcd-147b
python scripts/sign_ota_image.py --sign .pio/build/waveshare-esp32-s3-lcd-147b/firmware.bin \
    --board waveshare-esp32-s3-lcd-147b \
    --fw-version 0.5.0 --build 41 \
    --out hexhound-147b-0.5.0.hexfw
```

`--board` and `--fw-version` have no defaults on purpose. The board id decides
which hardware may install the image, and a version scraped from the tree you
happen to be standing in can silently disagree with the binary you are signing.

Always check what you produced:

```
python scripts/sign_ota_image.py --verify hexhound-147b-0.5.0.hexfw
```

With no `--pubkey` it reads the trusted list out of `src/ota/ota_pubkey.h`, so
it answers "would a device running this firmware install this", not merely "is
this signature valid". That is the question you actually care about.

## The install flow, and why it is in this order

```
IDLE
  Refuses every update verb. This is where the device sits.

ARMED                    <- requires the OWNER, on the device
  Nothing arms itself, nothing arms on a timer, no host command can reach
  this state. The window expires on its own. This is the whole
  implementation of "no automatic updates and no silent ones": the update
  cannot even BEGIN without a person, not merely that it asks first.

HEADER REJECTED          <- terminal, and NOTHING was written
  The 192-byte header is parsed, bounds-checked and Ed25519-verified before
  the target's begin() is ever called. So the passive slot is not even
  erased. The claim here is not "verify before commit", it is "verify
  before WRITE".

RECEIVING
  Chunks bounded against HEXHOUND_OTA_CHUNK_MAX and against the remaining
  count from the SIGNED length. A host that sends one byte too many is
  refused outright rather than truncated, because a host that is wrong
  about the length has already sent bytes that cannot be trusted.

VERIFYING
  The image is read BACK OUT of flash and hashed. Nothing that arrived on
  the wire is trusted. Hashing the incoming stream would attest to a copy
  that no longer exists; the thing that will boot is the thing in the
  partition.

READY
  setBootPartition() has marked the new slot PENDING. The device still
  boots the OLD firmware until it restarts, and the restart is the owner's.

FAILED
  Terminal for this attempt. Target aborted, device unchanged.
```

`setBootPartition()` is the single irreversible step and it is deliberately
last. Everything before it can be abandoned with no consequence.

## The trial window, which is the other half

Installing a good image is only half of never bricking. The other half is that
an image which installs fine but does not actually work must not become
permanent.

1. `esp_ota_set_boot_partition()` marks the new slot `NEW`.
2. The bootloader promotes it to `PENDING_VERIFY` and boots it.
3. The app must call `esp_ota_mark_app_valid_cancel_rollback()` to make it
   permanent.
4. If the device reboots while still `PENDING_VERIFY`, for **any** reason, the
   bootloader marks that slot `ABORTED` and boots the previous one.

**This requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.** Verified present in
the ESP32-S3 Arduino SDK this project builds against
(`framework-arduinoespressif32/tools/sdk/esp32s3/sdkconfig`, and `=1` in every
generated `sdkconfig.h` variant). Without it every rollback call is a no-op and
this entire section is decoration. It is a load-bearing dependency, not an
incidental one. Re-check it after any platform bump.

### The trap in step 4

Rollback is triggered by a REBOOT, not by unhappiness. An image that crashes,
panics or watchdogs is rolled back automatically because those all end in a
reset. An image that boots and then **wedges forever without resetting** is
never rolled back. It sits there pending, bricked in every way that matters to
the person holding it.

That is what the trial watchdog is for: it converts a hang into a reset. It is
armed only during the pending window and disarmed the moment the image is
confirmed, so the ordinary long-running firmware with its blocking patrol scans
is never subject to it.

### Milestones

A new image must reach all of these to be confirmed:

| Milestone | Reported from `main.cpp` at |
|---|---|
| `DISPLAY` | after the splash is drawn and the backlight is on |
| `STORAGE` | after `StorageModule::init()` |
| `PET` | after `loadPetState()` |
| `CRYPTO` | the known-answer tests, evaluated only while pending |
| `LOOP` | first `loop()` iteration |

`DISPLAY` is reported after pixels, not after `tft->init()` returns, and the
reason is specific: **this product has already shipped a build that ran
perfectly with nothing on the screen.** The HID firmware sat on the TFT_eSPI
backend that black-screens the T-Dongle S3, and because HID mode has no serial
console the failure was completely invisible. A liveness-only health check
would have confirmed that image as healthy. The pet being visible is the entire
product.

`CRYPTO` gates on `ContentCrypto::selfTest()` as well, because an image that
cannot verify a signature is an image that could never accept its own
successor. Confirming that as permanent strands the device on firmware that can
no longer be updated, which here is the same as bricked. It is evaluated only
while pending because it costs a real Ed25519 verify.

**If you add a milestone, you must add the call site in the same commit.** A
required milestone that nothing reports means every update rolls back after 20
seconds and OTA looks completely broken, and the cause will not be obvious.

### What the deadline actually means

`OTA_HEALTH_DEADLINE_MS` is the point by which every required milestone must
have been REACHED, not the point by which the image must have confirmed.
`evaluate()` tests CONFIRM before ROLLBACK, so an image that reaches all five
is confirmed even if it got there slowly, and only an image still missing one
at the deadline is rolled back.

That is the single place in this subsystem where an ambiguous-looking case
resolves toward CONFIRM rather than ROLLBACK, and it is worth knowing it was a
decision rather than an accident. "Display up, storage mounted, pet loaded,
crypto passed, loop running" is a working device by every measure this firmware
has; rolling it back for being slow would cost the owner a functioning update
and buy no safety. Slow boots are real here, since a SPIFFS format or a save
migration can add seconds.

A genuinely wedged image is not covered by this in either direction: it never
reaches `loop()`, so nothing ever calls `evaluate()`. The trial watchdog is
what catches that.

## Adding a board

`HEXHOUND_OTA_BOARD_ID` in `ota_identity.h`. It identifies HARDWARE, not a
build variant: the HID and vendor-TFT builds of the T-Dongle S3 share an id
because either one boots on that board. Choosing between variants is the
owner's business. Choosing between BOARDS is a safety property, because five of
the seven targets are ESP32-S3 and a chip-family check would happily pass a
T-Dongle image onto a Waveshare, where the panel pins, the PSRAM mode and the
backlight GPIO are all different. That is a dead screen, permanently, in
someone's hand.

The id strings match the platformio env names, so the thing a developer types
to build is the thing the signing script stamps in. Do not derive them from
`HEXHOUND_BOARD_NAME`: that is a display string and is allowed to be reworded,
which would silently invalidate every image ever signed for that board.

Note `waveshare-esp32-s3-touch-lcd-147` is exactly 32 characters and therefore
fills the board id field with **zero** NUL padding. This is safe only because
`parse()` copies into a 33-byte buffer it zeroed first. Do not "tighten" that
field to require room for a terminator; it would make that board unsignable.

## What the owner can lose

**The pet save survives an OTA.** Writes go exclusively through
`esp_ota_write()` on a handle bound to the passive app partition; ESP-IDF
clamps every write to that partition and OTA does not rewrite the partition
table. The app slots and SPIFFS are disjoint and not adjacent:

```
app0    0x010000  0x640000
app1    0x650000  0x640000
spiffs  0xC90000  0x360000   <- /pet_state_a.json, /pet_state_b.json
```

That is an argument, though, and arguments about data loss are worth checking
rather than believing. `selectTarget()` refuses to proceed at runtime if the
partition it resolved overlaps the filesystem, so the claim is enforced on the
device and not merely asserted here.

**The save-compatibility warning.** There is deliberately no minimum-version
field and no downgrade block, because refusing older firmware would remove the
owner's only escape route from a bad release on a device with no support
channel. The owner is allowed to go backwards.

The cost of that is real: **the save schema only migrates forward.** A save
written by schema v3 firmware and then read by v2 firmware is not something v2
knows how to interpret. Going backwards across a schema bump can therefore cost
the pet even though the firmware itself is fine. Nothing on the device can
detect this before the fact, so surfacing it is the companion app's job: when
it offers an image whose version is older than what is installed, it has to say
so in those terms, not as a generic "are you sure".

## The serial transport

`src/ota/ota_serial.{h,cpp}` is the wire. `src/ui/ui_update.{h,cpp}` is the
screen that arms it. `scripts/push_ota.py` is the host end.

Serial is full duplex, and this matters more than it looks. The image travels
host to device; the firmware's logging travels device to host. **They do not
collide.** So the host-to-device direction is pure framed binary with no
escaping and no base64, and the only thing the host has to cope with is finding
its response frames amongst ordinary log lines in the other direction. That is
solved by giving the device-to-host magic a non-ASCII first byte, so no log line
the firmware can print can contain it.

Native USB CDC (`board_build.cdc_on_boot = 1`) runs at USB speed regardless of
the nominal baud rate, so a 1.2 MB image is not a throughput problem and there
is no reason to trade correctness for density.

### Frame layout, both directions

```
off  len  field
0    4    magic
4    1    type
5    1    flags, reserved, must be 0
6    2    payload length, little-endian, <= 4096 (HEXHOUND_OTA_CHUNK_MAX)
8    N    payload
8+N  4    CRC32, little-endian, over bytes [4, 8+N)
```

Twelve bytes of overhead on a 4096-byte chunk, which is 0.3%.

| Direction | Magic | Bytes |
|---|---|---|
| host to device | `\xA5HXD` | `A5 48 58 44` |
| device to host | `\x5AHXH` | `5A 48 58 48` |

**Two magics, not one.** If both directions shared a magic then a loopback
adapter, an echoing terminal or a host that opened its own output would look
exactly like a peer, and the failure would present as a device that
mysteriously answers itself. The lead bytes are `0xA5` and `0x5A`: non-ASCII, so
neither can occur in log text, and complements of each other, so a stuck or
inverted line does not turn one into the other.

**The CRC covers from the type byte, not from the magic.** The magic is matched
exactly by the reader before anything else is read, so it is already proven;
what the CRC protects is the fields the reader is about to trust. It is standard
reflected CRC-32 (poly `0xEDB88320`, init and final xor `0xFFFFFFFF`), which is
what zlib and Python's `binascii.crc32` compute, so the host side is a library
call and not a second implementation that could drift. The device uses a
16-entry nibble table: 64 bytes of flash instead of 1 KB, on a check that is
nowhere near the bottleneck.

**The CRC is not security. The signature is.** It exists so a desynchronised
stream fails loudly and immediately, at the frame that went wrong, instead of
being fed to the digest check a megabyte and several seconds later where the
reported cause would be "the image did not arrive intact" and the actual cause
was a framing bug three thousand chunks earlier.

### Frame types

| Value | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | `HELLO` | empty | What are you, what state are you in? Legal in every state, changes none. |
| `0x02` | `BEGIN` | 192 bytes | The signed header. Drives `offerHeader()`. |
| `0x03` | `DATA` | <= 4096 | Image bytes, in order. Drives `offerChunk()`. |
| `0x04` | `COMMIT` | empty | Drives `finish()`. |
| `0x05` | `ABORT` | empty | Drives `cancel()`. |
| `0x81` | `IDENT` | 48 bytes | Identity and state. Answers `HELLO`, and sent unsolicited on every state change. |
| `0x82` | `PROGRESS` | 10 bytes | Per accepted chunk. Fixed-size and string-free; this one is sent three hundred times. |
| `0x83` | `RESULT` | variable | End of an attempt, with the verdict and the owner-facing help text. |

Host-to-device values are `0x00..0x7F` and device-to-host values have the high
bit set, so a frame that arrived on the wrong wire is refused by its type as
well as by its magic.

**There is no ARM verb, and that is the point.** Every host-to-device type is in
that table and none of them arms the session. `arm()` is reachable only from the
update screen, in response to a person holding the button. A host can ask what
state the device is in and it can send an image once a person has already said
yes; it has no way to say yes on their behalf. "No automatic updates and no
silent ones" is implemented by that absence, so **anything added to that table
has to be checked against it.**

### Payloads

`IDENT`, 48 bytes:

```
0   1   wire version (currently 1)
1   1   session state (OtaSession::State)
2   1   verdict of the last real decision
3   1   flags: bit0 running image on trial, bit1 booted after rollback,
          bit2 a flash target is actually attached
4   4   this firmware's version, packed major<<16|minor<<8|patch
8   4   OTA slot size, read from the REAL partition table
12  2   maximum chunk this device accepts
14  2   reserved, zero
16  32  board id, NUL-padded ASCII
```

`PROGRESS`, 10 bytes: state, verdict, bytes written (u32), image length (u32).

`RESULT`: the same 10 fixed bytes, then three NUL-terminated strings:
`verdictName()`, `verdictHelp()`, `failReason()`.

The help string travels on the wire rather than being looked up host-side on
purpose. It is written for the person holding the device and it lives in
`ota_image.cpp`; a host that kept its own copy would drift, and the two would
eventually describe the same refusal differently. **The device is the authority
on why it said no.**

### The device-side reader

`OtaFrameReader` is a strict byte-at-a-time state machine. It never blocks,
never allocates, and owns exactly one 4 KB payload buffer that is reused for
every frame, so "never hold more than one chunk" is a property of the type
rather than something a caller has to remember. It is portable, with no Arduino
dependency, so its resynchronisation and its CRC can be driven from a desktop
instead of by unplugging a cable at carefully chosen moments.

Three things in it are worth not undoing:

- **Resync re-tests the current byte.** The naive version drops back to "matched
  zero" on a mismatch, which loses a magic that began inside the garbage: feed
  it `A5 A5 48 58 44` and it never sees the frame. This is exactly KMP, and it is
  complete rather than merely better because all four magic bytes are distinct,
  so the failure function is zero everywhere. **Keep the magic bytes distinct.**
- **An impossible length resynchronises instead of skipping the payload.** A
  length over 4096 did not come from a peer that meant it, so those two bytes
  are not a length; trusting them enough to skip that many bytes would eat up to
  64 KB and the real frame boundary is inside what was discarded.
- **A corrupt frame fails the transfer, it does not carry on.** Only a transfer
  in progress is torn down; garbage arriving at an IDLE device is just garbage
  on a serial port, which is normal, and must not be reportable as a failed
  update.

### Pumping it

`OtaService::pump()` is called every loop iteration and is bounded to 16 KB per
call, so a flooding host cannot hold the loop hostage while the screen is trying
to draw a progress bar out of the same loop. Sixteen kilobytes is four full
chunks, far more than the flash write behind it can absorb, so the bound never
limits a well-behaved transfer.

`finish()` is the one blocking call: it reads the whole image back off flash and
hashes it, which is seconds, not milliseconds. That is acceptable exactly there
and nowhere else, because the alternative is a resumable hash spread across loop
iterations that could be interrupted halfway and would then be attesting to a
partition somebody else had touched.

### Two lines in `main.cpp` that are load-bearing

```cpp
OtaService::setRunningImageOnTrial(OtaHealth::isPending());
OtaService::pump(now);
```

**Without the first, the guard against erasing the last known-good image is
present in the code and absent on the device.** `OtaSession` refuses to arm and
refuses to write while the running image is itself on trial, because the slot an
update would erase is not spare space then: it holds the previous firmware and
is the only thing there is to roll back to. The session has no ESP dependency
and no way to discover this, so it is injected. It is read *after*
`OtaHealth::update()`, which is what confirms a trial image; reading before it
would carry a stale "still on trial" for one more iteration, which is harmless
(a stale value can only refuse an update that would have been fine) but there is
no reason to be wrong on purpose.

**Without the second, the arm window and the stall timeout never expire.** A
device that stays ARMED because nobody ticked it is a device quietly accepting
firmware on a desk, which is exactly what the window exists to prevent.

## Pushing an image

```
python scripts/push_ota.py dist/hexhound-147b-0.5.0.hexfw --port COM14
```

`--list` shows likely ports. The script:

1. **Verifies the file locally before sending a byte**, using
   `sign_ota_image.py`'s own `verify()`, imported rather than reimplemented. Not
   a sanity check: the same gate in the same order. If this passes and the
   device still refuses, that disagreement is itself the bug.
2. `HELLO`s the device and checks the board id and the slot size against what
   the device actually reports, not against a compiled-in guess.
3. **Waits for you to arm it by hand.** MENU -> UPDATE -> START UPDATE. There is
   no flag that does this; `--skip-verify` does not exist either, and says so
   rather than being silently ignored.
4. Streams, checking the device's byte count against its own after every chunk
   and stopping the moment the two disagree.
5. Reports the device's verdict, including `verdictHelp()` in the device's own
   words.

`--show-log` interleaves the firmware's log lines, which during a failed update
are usually the most useful thing on the screen.

### The screen on each panel family

The update screen respects all three families with compile-time branches only,
so adding it changed no byte of any existing screen on any board.

- **320x172** and **240x240 round**: everything fits. Version, status, progress
  (a bar on the rectangle, the rim ring on the circle), the full refusal text,
  and the action rows.
- **160x80**: 56 pixels of body, which is six size-1 lines, and a refused update
  wants eight. So on this panel only, and only while refused, the installed
  version line is dropped, and text that still does not fit is truncated **with
  an ellipsis**. That marker is not cosmetic: most `verdictHelp()` strings end in
  "Nothing was changed", which is the single fact the owner wants after a failed
  update, and a cut that reads as a finished sentence would say close to the
  opposite. Typical refusals (46 to 58 characters) fit whole; only the four
  longest truncate.

The screen also surfaces `OtaHealth::bootedAfterRollback()`, because otherwise a
failed update looks to the owner exactly like nothing happened.

**Nothing reboots itself.** Reaching READY means the image is verified and staged
and the device is still running the old firmware. The restart is offered and the
owner takes it.

### What the screen may say in READY, and what it may not

READY is a one-way door, and the screen has to respect that. `setBootPartition()`
has run and otadata already names the new slot, so **the update will be applied
at the next boot whatever the owner does.** Powering off does not cancel it, it
defers it. `cancel()` refuses from READY for exactly this reason, so:

- **No cancel or discard row is drawn once the state is READY.** The one control
  the owner was given would not do what its label says.
- The status reads "Update ready", not "Update installed". It is staged and
  verified; the old firmware is still the one running, and "installed" would
  claim something that only becomes true after a restart that has not happened.
- The sentence states the outcome as a fact and pairs it with the reassurance
  that makes it an easy one to hear: *"Installs at the next restart. It rolls
  back if it fails."* That second half is not a consolation, it is precisely
  what the trial window guarantees.
- The two rows are RESTART NOW and RESTART LATER. Both are honest; neither
  pretends the update can be called off.

Before READY, cancel works normally and is offered.

`arm()` refuses too, from READY and while the running image is on trial, and it
returns `false` in both cases. The screen does not assume success: a refused arm
leaves the session carrying the verdict that says why, and an IDLE session with
a non-ACCEPTED verdict renders `verdictHelp()` rather than "Up to date". Without
that, pressing START UPDATE during a trial window would do nothing visible at
all, and a control that appears to do nothing is indistinguishable from a broken
one.

### The trusted-key seam is not reachable from here

`OtaSession::setTrustedKeys()` exists only under `UNIT_TEST` or
`SIMULATOR_BUILD`. Neither the transport nor the screen calls it, and neither
ever should: nothing on a device gets to choose which keys it trusts. If a
change to this area makes the hardware build fail on that symbol, the guard is
working, and the fix is to stop calling it rather than to widen it.

## Chicken and egg, and it is worth stating plainly

**Every device already in the field must be updated over USB the first time,
because no firmware anybody is holding contains the OTA receiver.** OTA becomes
useful for the update after that one. There is no way around this and it is not
a defect; it is just worth knowing before someone promises a field update to a
unit that shipped before this branch.

## Verified on hardware, 2026-08-03

Waveshare 1.47B, MAC `44:1b:f6:xx:xx:xx`, PSRAM 8MB. Identified by MAC and
PSRAM before anything was written, and a full SPIFFS image taken first.

The device was USB-flashed with 0.4.1, then updated to 0.4.2 entirely over the
serial transport.

| | before | after |
|---|---|---|
| firmware | 0.4.1 | **0.4.2** |
| otadata state | `-1` UNDEFINED (USB flashed) | **`2` VALID (confirmed)** |
| pet | stage=2 xp=87 schema 3 | **stage=2 xp=87 schema 3** |

1,363,392 bytes at about 64 KiB/s, read back off flash and hashed before the
slot was staged, restarted by the owner, and confirmed by the health check.

The state going from UNDEFINED to VALID is the part worth noticing: it is the
health check actually running, not merely the image booting. A USB-flashed
image has no otadata entry and stays UNDEFINED forever; only an OTA image goes
NEW, then PENDING_VERIFY, then VALID, and only the milestone checks move it
that last step.

Requirement met and observed rather than argued: **the pet survived the
update**, same stage, same XP, same schema, saves still incrementing.

### Two bugs this found that no test could

Both are recorded because the shape of them matters more than the fix.

**The serial receive buffer was never sized.** Arduino defaults to 256 bytes; a
data frame is 4108. Everything beyond 256 bytes arriving between two polls was
dropped by the driver, and when the dropped run contained the 4-byte magic the
reader never started a frame. The failure was TOTAL SILENCE: the device sat
healthy in RECEIVING with zero bytes written, the host waited, and the session
died on its own stall timeout with nothing anywhere naming the cause. It read
like a protocol bug, a crash, or bad flash. It was none of those.

No native test could have caught it. `MemoryOtaTarget` is handed complete
frames by the harness, so the wire has no buffer to overflow. This one needed
real silicon, a real USB stack, and a loop with a screen to repaint.

**A log line could kill an update.** `push_ota.py` drains the device log from
inside the frame reader, so a `UnicodeEncodeError` out of `print()` unwound the
entire push: a cp1252 console plus one arrow in an unrelated boot message was
enough, with the device armed and waiting. Diagnostics do not get to fail the
operation they are describing. Fixed at the host, and the arrows are gone from
the firmware's serial strings, which should have been ASCII anyway.

## Verification standard

Before calling anything here done:

- `pio run -e lilygo-t-dongle-s3-vendor-app`. The no-PSRAM board is the real
  test. Watch the RAM number and treat a jump as a defect.
- `pio run -e waveshare-esp32-s3-lcd-147b -e waveshare-esp32-s3-lcd-128`
- `pio test -e native`, all suites green.
- **Measure cost with a link probe, never the headline build number.** Until
  something references it, the whole of `src/ota` is garbage-collected at link
  time and reads as free. Build the same target with and without the
  integration and diff the two numbers.

  Measured on `lilygo-t-dongle-s3-vendor-app`, the no-PSRAM board, three points
  on one tree:

  | | RAM | Flash |
  |---|---|---|
  | A: `ota_serial.cpp` / `ui_update.cpp` absent | 81,884 | 1,210,761 |
  | B: both compiled, nothing references them | 86,364 | 1,211,553 |
  | C: wired into `main.cpp` and the menu | 86,404 | 1,223,437 |
  | **link probe (B to C)** | **+40** | **+11,884** |
  | **whole feature (A to C)** | **+4,520** | **+12,676** |

  **Flash behaves the way the warning above predicts and RAM does not.** Between
  A and B the flash barely moves (+792): unreferenced code really is collected
  at link time, so measuring only the headline number would have reported this
  feature as nearly free. But the RAM is already fully charged at B, before a
  single line references any of it, because `--gc-sections` does not remove the
  `.bss` for a file-scope static in a translation unit that is being compiled.

  So the number that matters for RAM is the A-to-C one, and it is 4,520 bytes:
  almost entirely the single 4 KB `OtaFrameReader` payload buffer, which is the
  "never hold more than one chunk" rule made structural, plus about 350 bytes of
  `OtaSession`. It is static rather than heap on purpose: a 4 KB allocation that
  can fail on a fragmented no-PSRAM heap, at the moment an owner has just asked
  for an update, is a worse failure than 4 KB permanently spent.

  The practical warning for whoever measures next: **do the probe by removing
  the FILES, not by removing the calls.** Removing the calls understates a
  static buffer by its entire size.
- **Build a clean checkout**, not just the working tree:
  `git worktree add --detach <tmp> HEAD` and build there.

  This one is not theoretical. `.gitignore` carried `content/` with no leading
  slash, meant for the top-level directory of signed `.hcp` build output. Git
  matches a bare directory pattern at any depth, so it also matched
  `src/content/` and silently untracked seven files including
  `content_crypto.{h,cpp}`, the only Ed25519 and SHA-512 implementation in the
  tree. Every local build stayed green because the files were on disk, and
  P3-W1 was pushed looking fine. No clean clone could compile it, which
  included every board target, because `build_src_filter = +<*>` compiles
  `src/ota` and `ota_image.cpp` includes that header. Fixed in `0f20c70`.

  `git status` is silent about ignored files and `git commit -a` will not
  mention them, so a clean-checkout build is the only thing that catches this.
  Ninety seconds, and it is the difference between a rollback point and a
  commit that merely looks like one.
- Flash hardware WITHOUT erasing, so the save exercises migration, and
  **identify the board first by MAC and PSRAM before writing to it.** Every
  ESP32-S3 image reports the same chip family, so a wrong image flashes fine
  and leaves a dark screen that reads as a hardware fault.
