# Open work

Tracked, because `tasks/todo.md` is gitignored and the plan that lived there
only existed on one machine. Everything here was found and deliberately NOT
fixed while doing something else; each item says what it is, what it costs, and
what a fix has to prove. Two of them are content decisions rather than defects
and are labelled as such.

Companion to `docs/bugfix_log.md`, which records what WAS fixed and why.

Ordered by whether a wearer can see it.

# Open after the sleep and missions work (2026-09-01)

Found while doing something else and deliberately NOT fixed. Each says what it
is and what a fix has to prove.

## A. Two parallel stage-name tables

`STAGE_NAMES` (config.h, uppercase) feeds the evolution cutscene, the boot
detail line and the journal. `PetCore::stageName()` (pet_core.cpp, mixed case)
feeds the home screen and the patrol HUD. They are two sources of truth for the
same five strings, and that is exactly why a 203-file company-name scrub fixed
one and left a company-initialled name sitting on the most-viewed screen in the product for
two days.

- [ ] Consolidating them changes strings on every board and is a design call
      about capitalisation, not a cleanup. The defect it caused is fixed; the
      structure that caused it is not.

## B. Anything that changes a string changes ~1 KB of every image

Measured twice, independently, on different strings. Shortening a company-initialled name
to "Sentinel" moved 944 to 1049 bytes on all seven images with the total size
unchanged. Rewording one CDC serial line by FOUR characters moved 130044 bytes
across 17924 runs in the T-Display S3 image.

A string literal in an early-linked translation unit shifts every rodata
address after it, and every pointer and literal pool referencing them moves.

- [ ] This is not a defect and there is nothing to fix. It is written down
      because the 65-byte byte-identity invariant is a genuinely good tool that
      has caught several real regressions, and anyone who changes a user-facing
      string and then runs it will see ~1 KB of movement on boards they never
      touched. Measure against a base that already carries the string change.

## C. Deep-sleep current is unmeasured

The three boards that can now sleep go genuinely dark and wake reliably, which
was the contract. Nothing was done about draw: the 1.28's QMI8658 IMU and the
Touch 1.47's AXS5106L are left in their default states.

- [ ] Needs a meter on the battery lead, and a decision about whether a
      multi-day standby is a product requirement or not.

## D. The missions screen still promises what a CDC image cannot do

`ui_missions.cpp` draws `hold=exec` in the mission-brief footer whenever the
host is connected. On a non-HID image that is a promise the build cannot keep.
The alert now tells the truth AFTER the press, so this is cosmetic.

- [ ] Fixing it means a second edit to an all-boards file for a benefit the
      alert already delivers. Left as a decision, not an oversight.

## E. CLOSED. `USBModule::update()` hardcoded `_connected = true`

The alert was unreachable and the board could not tell a plugged host from an
unplugged one.

- [x] Done. `update()` now reads the HID endpoint through `UsbHostPresence`
      (`src/modules/usb_presence.h`) on a HID image. See J for the CDC half,
      which is deliberately different rather than merely unfinished.

## F. A dead capability macro, and a stale comment

- [ ] `HEXHOUND_BL_PULSE_DIMMER` (board_profile.h) is defined and read by
      NOTHING. `hexhoundSetBacklight()` drives GPIO46 with a plain
      digitalWrite, which happens to work because LOW to HIGH is the dimmer's
      full-brightness edge. Same class as the flourish enum that nothing read:
      a capability with no consumer is invisible. `src/power/soft_sleep.h`
      exists partly to make this class of mistake a compile error.
- [ ] The `[SLEEP]` row comment in `ui_config.cpp` still says "the only panel
      that compiles this". It is three panels now.

## G. PARTLY WITHDRAWN. The stray `?` is a simulator artifact, not a defect

This item claimed two 320x172 config-screen defects, "confirmed against an
unmodified 147B render". A render is the SIMULATOR, and that is the flaw in the
confirmation.

**The stray `?` before "SSID" is not a defect and there is nothing to fix.**
`ui_config.cpp` prints `" "` immediately before the SSID tab. `` is the
CP437 diamond, and it is a house convention used in the headers of the menu,
the kit, the journal, the games list and this screen; one call site comments it
as "diamond char". The simulator cannot draw it, because `hal_sim.cpp` reduces
its font to printable ASCII:

    if (c < 32 || c > 126) c = '?';

So every one of those headers renders a `?` in the simulator and a diamond on
glass. The same substitution also affects ``, the right arrow used as a
check mark in this file.

- [x] Withdrawn. Anyone re-reporting a stray `?` from a simulator capture
      should check that line first. The hardware behaviour is inferred from the
      convention shipping unremarked on five screens, not measured on glass;
      one photograph would settle it for good.
- [ ] **The footer clipping is untouched by this and may still be real:** the
      footer rendered as `scroll ... hold=tab` with `press=` cut off the left
      edge. That is a layout question, not a glyph one, so the simulator is a
      fair witness for it. Fixing it moves a frozen board, so it needs to be a
      deliberate decision to spend that.

## H. MOSTLY CLOSED. The two `Win` missions sat under the four safe ones

Raised by the owner after the 2026-09-01 incident, and it is a real design
defect rather than a preference. On the round boards a hold in a sub-list moves
the cursor AND opens the row, so a hold that lands one row off opens a different
mission than the one being looked at. `Lock Screen` (4) and `Map Link` (6) are
the only two missions that press a modifier, and they sit immediately under the
four that only type text.

A second inconsistency in the same table: `Lock Screen` is
`MISSION_HIGH_MISCHIEF` and `Map Link` is `MISSION_NORMAL`, even though both
press `Win`. The flag currently means "raises the mischief stat", not "touches
the host's window manager", so there is no field that marks the dangerous two.

- [x] Done, by appending a flag rather than reordering. `MISSION_GUI_KEYS`
      (`usb_module.h`) marks the two missions that press the host's GUI
      modifier. It is a SEPARATE bit from `MISSION_HIGH_MISCHIEF`, because the
      two sets do not coincide: mission 7 is high mischief and only types text,
      mission 6 presses Win and is not high mischief. Reusing the display flag
      would have made it load-bearing for safety. Nothing was reordered, so no
      journal entry was renamed.

      A flagged mission now takes TWO holds. The first arms it and repaints the
      brief with a warning naming the actual combination and saying in plain
      words what it does to the host; the second runs it. The arm expires after
      10 seconds, is consumed by one run, and is stored as a mission INDEX
      rather than a bool, so arming Lock Screen cannot confirm Map Link. Every
      way out of the screen disarms. The seven text-only missions are untouched
      on every path.

      Two independent mechanisms, guarding two different failures. The gate
      (`UIMissions::longPressShouldExecute()`) does the asking. The backstop
      (`executeMission()` taking a `GuiKeyConsent` with no default) does the
      refusing, and exists because the gate is one line in `main.cpp` whose
      deletion would otherwise be silent: green build, green tests, both Win
      missions back to a single hold. Omitting consent is now a compile error.
      Covered by `test/test_mission_confirm/`, 29 cases, mutation verified.

      **Correction, 2026-09-08.** That coverage claim was true locally and
      false in CI for the whole time it stood there. The suite included the two
      headers but not the implementations, and `[env:native]` compiles no
      project sources, so it failed to LINK on every CI run and its cases never
      executed there. Fixed, and the local runner's special case for it removed
      so the two cannot diverge again. See `docs/bugfix_log.md`, 2026-09-08.

- [x] **Confirmed on glass, 2026-09-04.** Mission 4 was run on the T-RGB from
      the `lilygo-t-rgb-hid` image and takes TWO holds. The first arms and
      repaints; it does not reach the host. That is the defect closed, on the
      board the 2026-09-01 incident happened on.

## I. The 2026-09-01 mechanism was never reproduced

Recorded so that nobody later reads the fix as a diagnosis. The guard in
`usb_module.cpp` is built to hold whatever the cause was, and
`docs/bugfix_log.md` sets out why the obvious story does not fully close: every
HID report carries the complete modifier byte, so a delivered keystroke report
with `modifier=0` should already clear a stale modifier on the host.

- [ ] Reproducing it needs a USB analyser or a host-side HID logger against the
      pre-fix image. Worth doing only if the symptom ever returns, and the
      instrumented-build control in `docs/hardware_checklist.md` section 10 is
      the cheaper thing to run first.

## J. Item E is now load-bearing, and still open

`USBModule::update()` hardcodes `_connected = true` (see E above), so both the
`isConnected()` gate in `main.cpp` and the `if (!_connected)` guard at the top
of `executeMission()` are dead checks. The new `clearAllKeys()` incidentally
covers the safety half of this - an absent or unready host makes `SendReport()`
fail, so the mission refuses rather than typing into nothing - but it covers it
by accident.

- [x] Done, and the CDC answer is NOT the obvious one. Reporting "not
      connected" on a CDC image was considered and REJECTED with a hard reason:
      `EVENT_USB_CONNECTED` is the only thing that increments `usbConnects`,
      and `usbConnects >= EGG_USB_CONNECTS` is a hatch requirement
      (`pet_core.cpp`). Six of the eight images are CDC, so their eggs would
      never have hatched. A third state exists instead, `USB_HOST_UNKNOWN`,
      returned by the new `hostState()`; `isConnected()` keeps its signature and
      keeps returning true on CDC, so all five of its call sites are unchanged.
      The truthful CDC message stays `USB_MISSION_FAILED_MSG`, which says the
      build has no keyboard, rather than "plug into a computer" on a board that
      already is.

      Because presence can now change, `EVENT_USB_DISCONNECTED` fires for the
      first time. The falling edge is debounced by 1500 ms on purpose:
      `ready()` is also false for the milliseconds a report is in flight, so an
      undebounced read would publish a disconnect/connect pair for every
      character typed and pay the pet for phantom plug-ins. Covered by
      `test/test_usb_presence/`, 9 cases, mutation verified.

# Open after the USB reset-loop hunt (2026-09-02)

## K. `HEXHOUND_DEMO_STAGE` does not set `hatched`, unlike its sibling

`HEXHOUND_FORCE_RUNTIME_STAGE` (main.cpp:710-721) sets the stage AND
`hatched = true` AND clears `dirty`. `HEXHOUND_DEMO_STAGE` (main.cpp:843-866)
sets only the stage. On a board with no save, or an unhatched egg, that gives
`stage == STAGE_GREMLIN` with `hatched == false`, and `PetCore::checkEvolution()`
takes the `!hatched` branch: on meeting the egg thresholds it sets
`stage = STAGE_PACKET_PUP`, a DEMOTION from 4 to 2, and publishes
`EVENT_STAGE_EVOLVED` - an egg-hatch cutscene on a Gremlin.

- [ ] Cannot have happened on the bench T-RGB, whose pet is hatched. It is a
      wrong picture, not a reset, and it bites only a board with no save. One
      line to fix; left because the demo env may be retired instead (see M).

## L. `board_build.cdc_on_boot` is dead config on espressif32@6.12.0

Nothing in the platform builder reads that key - `grep -rni "cdc" builder/`
returns zero hits. What decides is the `-D` macro, and
`boards/lilygo_t_rgb.json` hardcodes `-DARDUINO_USB_MODE=1` and
`-DARDUINO_USB_CDC_ON_BOOT=1` in `build.extra_flags`, which env `build_flags`
land after and override (656 redefinition warnings per build, last flag wins).

So `[env:lilygo-t-rgb-hid]`'s comment - "Set here as well as via the -D flag so
the board config and the macros cannot disagree" - describes a no-op. The same
dead line and the same false reassurance are in `[env:t-dongle-s3-hid]`.

- [ ] Harmless, but it is a false reassurance in the one file where mistakes are
      invisible. Fixing it is a comment edit on a frozen board's env.

## M. The demo env is nearly obsolete, and it is the image that misbehaved

`[env:lilygo-t-rgb-hid-demo]` exists only because MISSIONS is gated at stage 4
(900 XP) and the bench pet could not reach the screen being filmed. That pet is
now stage 3 with 868 XP, so it is 32 XP from opening MISSIONS on the plain
`[env:lilygo-t-rgb-hid]`.

- [ ] Once it crosses 900, the forced-stage / no-persist pair can be deleted
      outright rather than fixed (see K). Note the env comment at
      platformio.ini:958 still says "a Packet Pup with 241 XP" and is stale.

## N. `USBCDC::write()` can still stall a boot, by a different route

The 443-second boot stall documented at main.cpp:322-372 was `Serial.flush()` on
`HWCDC`. `USBCDC` - what `Serial` becomes in the composite diag env - is not
gentler: `write()` busy-spins on `tud_cdc_n_write_flush()` for as long as the
host stays connected without reading, and `tx_timeout_ms` bounds only acquiring
the TX lock, not that spin. It also returns 0 when no host has the port open, so
pre-attach output is dropped rather than buffered.

- [ ] Only affects the diagnostic env, and its documented workflow is to open
      the monitor first. Recorded so nobody re-derives the 443-second bug from
      the other end. `setTxTimeoutMs(0)` is NOT the answer here either - it does
      not bound the spin, and it drops lines.


# Open after the quest-payout test work (2026-09-03)

## O. A real-time scanner can block every native test binary on Windows

Originally recorded as one suite's executable being quarantined under a generic
packer heuristic. It is broader than that, and the mechanism was measured on
2026-09-04 rather than inferred:

Two byte-identical hello-world executables were linked from the same source in
the same directory. One was left alone and one was executed.

    linked, never executed   -> intact 20 seconds later
    linked, execution tried  -> refused "Permission denied", then deleted

So **linking is not blocked and building is not blocked. Launching is.** The
scanner refuses to start a freshly linked unsigned image and quarantines it on
that attempt, which is why the failure looks like a missing file rather than a
denied one, and why it can appear to strike at random: a binary only dies once
something tries to run it.

Two consequences worth knowing before diagnosing anything else:

- A build that reports success and then cannot run its own output is this, not
  a link error and not a code regression. If a test sweep reports 17 of 18 with
  no compiler error, check the quarantine before checking the source.
- Loading the same code as a shared library from an already-trusted host
  process is not blocked, so building a suite as a DLL and driving it from a
  script is a working way to run tests while the block is in place. It is a
  workaround, not a fix: it changes how the code is loaded, so it is not
  evidence that the executable path would have worked.

- [x] **There is now a working way to run the suites here anyway.**
      `scripts/run_native_suites.sh` builds each suite as a shared library and
      drives it from Python, so the code is loaded by an already-trusted
      process instead of started as a new image. Verified 2026-09-04: all 20
      suites, 439 cases, 0 failed, INCLUDING the one that could not be launched
      as an executable at all.

      It is a workaround, not a fix. It changes how the code is loaded, so a
      green run through it is not evidence that the executable path is sound.

- [ ] The real fix is still an exclusion for the build output directory, which
      needs the scanner's own interface and is the maintainer's call. Until
      then, use the script above locally; CI on Linux runs the suites normally
      and is unaffected.


# Open before the repository is published

## R. The signing keys are DEVELOPMENT keys, and images built on them shipped

`src/ota/ota_pubkey.h` and `src/content/content_pubkey.h` both carry keys whose
private halves were minted onto a developer machine and live in gitignored
files there. The headers have always said so, in a comment. Images built on
them have been served publicly from the web flasher, because a comment is not a
control and nothing was checking.

The OTA key is the one that matters. Anyone who obtains its private half can
sign firmware that every device in the field accepts as genuine, which is the
entire thing OTA signing exists to prevent. The content key is a smaller blast
radius: it lets somebody write dialogue.

**The mechanism is now fixed and the key itself is not.**

- [x] `deploy_web_flasher.py` REFUSES to publish images that trust a
      development key. Provenance is machine-readable
      (`*_KEY_PROVENANCE_DEVELOPMENT` in each generated header), an absent
      marker counts as development so an old or hand-edited header fails
      closed, and `--allow-dev-keys` makes shipping one a deliberate, logged
      act instead of a silent one. Four cases in `scripts/test_deploy_gates.py`
      prove all of that fires.
- [x] Both signing scripts gained `--set-release-pubkey`, which installs a
      release PUBLIC key and writes no private key at all. That is the point:
      `--gen-key` mints a private key onto whatever machine runs it, which is
      what makes its output a development key however carefully it is handled
      afterwards.
- [ ] **Generate the real keypairs somewhere that is not a developer laptop,
      and decide where the private halves live.** This is a custody decision,
      not a code change, and it cannot be done from inside this repository:
      generating them here would reproduce exactly the weakness being fixed.
      Until it is made, every publish needs `--allow-dev-keys` and every unit
      flashed from the web page trusts a key that is not fit for the job.
- [ ] Units already in the field trust the current OTA key. Replacing it is a
      firmware change, so the rotation has to reach them before the old key is
      considered retired, or they can never be updated again.

## T. The C5 image cannot be built here, so a full eight-board stage is blocked

Verified 2026-09-04, and it is an environment fault rather than a code one: the
C5 build dies compiling `FS.cpp.o`, an Arduino framework file, before it reaches
any HexHound source. Nothing in this project can have caused it.

Two separate causes were found and only the first is fixed:

- **Git Bash cannot build this target at all.** `idf_tools.py` refuses to run
  when `MSYSTEM` is set, and the MSys runtime re-injects that variable into
  every child process, so it cannot be unset from there. Build it from
  PowerShell or cmd. Written up in `docs/build-and-bench-notes.md`, and the
  misleading comment in `scripts/build_flashes.py` is corrected.
- **Unfixed: the RISC-V compiler is not on PATH.** From a native shell the
  build gets further and then fails with `'riscv32-esp-elf-g++' is not
  recognized`. The compiler IS present in the isolated core directory under
  `packages/riscv32-esp-elf/bin/`, so this is a registration problem, probably
  left behind by the earlier failed `idf_tools.py` runs.

- [ ] Repairing it likely means letting the platform reinstall into a clean
      isolated core directory, which is a large download and has historically
      been the step that fails behind TLS interception on this machine (the old
      item E2). Worth attempting on a machine that is not this one before
      spending the download here.
- [ ] **Consequence: the flasher cannot be re-staged with a complete board set
      until this is fixed**, because the publish gate correctly refuses a
      partial picker. The other eight images all build and link.

## S. The staged web-flasher payload is stale and lacks the T-RGB

`web/boards.json` is version 0.4.4 and lists SEVEN boards under ids that no
longer match `stage_web_flasher.BOARDS`: it still says `t-dongle-s3`,
`waveshare-147b`, `waveshare-128-round` where the code now says
`t-dongle-s3-vendor-app`, `waveshare-esp32-s3-lcd-147b`,
`waveshare-esp32-s3-lcd-128`. `t-rgb` is absent altogether.

So the live flasher does not serve the board this branch exists to add, and the
publish gate refuses the staged payload as an incomplete board set. That gate
is working; the payload is what is wrong.

- [ ] Re-stage after a full eight-image build. Note the two failures in
      `scripts/test_deploy_gates.py` ("clean payload passes every gate" and
      "--allow-missing-keys permits it, loudly") are BOTH this, not a defect in
      the gates: they call `preflight()`, which refuses the stale payload
      before reaching the case under test. They will pass once staging is
      current.


## P. HoundLink carries three obligations that no document records

`web/houndlink/index.html` is a browser tool that talks to a board. Three
things in it were written expecting a wiring document that was never actually
authored, so the references pointed at a file that has never existed on any
branch. They now point here instead.

- [ ] **The firmware constants are mirrored by hand and can drift.** `FW` in
      that file duplicates values from `src/config.h`, `src/pet/pet_core.h` and
      `src/social/hexpass_types.h`, because a browser app cannot include a C
      header. Any change on the firmware side has to be repeated there, and
      nothing enforces it. The validator is only as good as those numbers. A
      real fix generates the block from the headers at build time.
- [ ] **The content-pack size cap is ambiguous and the tool guesses safely.**
      The spec says "6 KB of recipes" without saying whether that bounds the
      CBOR body or the whole file; the two differ by the 80 byte envelope. The
      tool checks the body against the cap and warns near the boundary rather
      than silently picking a reading. Someone has to decide which it is.
- [ ] **The serial transfer protocol is PROPOSED, not agreed.** No firmware in
      this tree implements it and there is no serial command handler at all
      today. The design in that file should not be read as a specification
      until it is.

## Q. DECIDED, ACCEPTED. A pre-rewrite commit is retrievable by hash

Attribution trailers were stripped from this branch's history and the branch
was force-pushed. Verified: no trailer survives in any commit reachable from
any remote ref, and as of 2026-09-04 none is reachable from any LOCAL ref
either. But a force-push does not purge the old objects, and the pre-rewrite
commit is still served by the host when asked for by hash. It becomes
world-readable when the repository is published.

**Decision: accept the residual.** Recorded here rather than left open, because
an undecided risk is one nobody re-examines.

The reasoning, so it can be re-opened on its merits rather than re-argued from
scratch:

- The exposed content is two lines of authorship metadata and a session URL.
  No credentials, no personal data, nothing about the product.
- Reaching it requires knowing a 40 character hash. The free half of the fix is
  done: no tracked file contains that hash any more, so the repository does not
  publish the way in.
- The only guaranteed removal is deleting and recreating the repository, which
  would destroy seven open pull requests and their review history. That is
  plainly disproportionate to two lines of metadata.
- Asking the host to garbage-collect unreachable objects is the middle option
  and remains available at any time. It needs an account holder to raise it.

- [x] Accepted. Reversible right up until publication, and the middle option
      above costs nothing but a support request if the view changes.

**What was cleaned up on 2026-09-04, so it is not repeated:** the three backup
refs from the rewrite were deleted, and 35 leftover parallel-worker branches
were removed after checking each one. Four of them appeared to hold unmerged
work and did not: three contained only pre-rewrite duplicates, identical in
tree and subject to commits already on the branch and differing solely by the
stripped trailer, and the fourth added an environment that is already present.
A history rewrite makes finished work look unique, so compare trees and
subjects rather than trusting a commit count.

---

## Everything below was CLOSED on 2026-08-30

Kept as a record of what each item turned out to be, because three of the five
were not what this document said they were.

### A. Flourishes - DONE, and a repeat of the original defect was caught

`PetState::flourishSlot` (schema 4 -> 5), rules in `src/pet/pet_flourish.h`,
renderer in `src/ui/ui_flourish.*`, art from `scripts/gen_flourish_art.py`.
Motes orbit the pet on the home screen, on an ellipse tuned by sweeping the
radius against captured home screens from all four panel families.

**The first delivery shipped a flourish system nothing could select.**
`flourishNext()` and `flourishSet()` existed and were tested, but no screen
called them, which is exactly the defect `COSMETIC_WORN` sat in for months and
the reason this whole workstream started. Closed by adding an FX row to the
closet, which then exposed a real layout bug: on the 160x80 panel the third row
printed through the footer rule with its name on top of `hold=back`, because
the row gap was a constant that happened to suit two rows. It is derived now.

### B. Worn overlays on the animated pixel sprite - CLOSED, premise corrected

Confirmed by rendering, with a control (same flags, resting pet, cosmetics
clearly visible) so the negative result could not be a harness failure.

A second anchor table measured against the pixel art was tried and REJECTED
with evidence: the pixel sprite is not a small copy of the HD art, and its
silhouette MOVES between frames (`gremlin_laugh` and `pup_hungry` sit a whole
pixel below their own idle frames), so no static table can be right. Do not
reopen this without reading the header of `scripts/gen_wear_anchors.py`.

The remedy was to stop drawing the pixel sprite where the pet must be dressed.
The patrol HUD corner draws the HD still on every panel now, and both
rectangular boards got SMALLER for it.

**The gap was never rectangular-only.** `drawRoundPet()` has the same
`if (resting)` fall-through, so a hungry pet on the 480 lost its cosmetics too,
and `ANIM_HUNGRY` is a steady state rather than a timed override. It has its own
HD portrait now (`scripts/gen_hd_hungry.py`). Only the two TIMED animations
(HAPPY 2 to 5 s, ALERT 4 s) are still on the pixel path, deliberately.

### C. Game footer ghost text - DONE, and it was not the games

**No game ever drew it.** Identical white pixel runs at identical x positions
appeared in three different games at once, which ruled the games out before any
drawing code was read. It was the HOME SCREEN's stat line and subtitle, never
cleared: `playArea()` is inset from the chrome rules so a sprite never sits on
one, and that inset was also serving as the clearing boundary, so four thin
bands were owned by nothing. Each one hid the next until the one in front was
cleared. `chromeInit()` blanks the whole band now.

Two further defects found in the same audit: `ChipRow::layout()` sized labels
from real pixels with an assumed 6x8 glyph, so at 480 firewall's ALLOW/BLOCK
were drawn 180 px wide in a 175 px chip and merged into `ALLOWBLOCK`; and
`signal_memory`'s tile label was centred with a half-width formula.

`uiround::header()` never cleared its band either, which affected EVERY round
screen and not just the games. Fixed at source.

The 240-relative size CEILINGS in the games are now scaled too, so the 480
panel stops binding every chip, cell and bar at half the size it was authored
at. That was raised as a design call rather than decided unilaterally, which
was the right way to raise it.

### D. Den slots - CLOSED as correct behaviour, not a defect

Three more den cosmetics were authored, so there are now 8 pieces for 8
brackets and a maxed pet can fill the room.

The "why does it show 5" question had TWO answers and only one was
reproducible: 4 den items owned (`owned + 1`), rendered and confirmed; or 3
owned with one standing in a high slot index, which is real and pinned by a
test but which the sim cannot construct because `HEXHOUND_SIM_OWN` fills the
first free slot. The footer tells them apart, because it counts ITEMS.

### E. Verification debts - CLOSED

E1 (rectangular patrol showed no materials) and E3b (rectangular closet wasted
half the panel) are both done. E3 was discharged the day it was written and
found nothing.

### One defect this workstream INTRODUCED and caught

Adding `WearAnchor` to `ItemDef` broke `ROW_RE` in `scripts/gen_item_icons.py`,
which required `}` immediately after the colour. Verified against the shipped
table: it matched 16 of 20 rows, and the four it missed were exactly the four
that gained an anchor. `ITEM_ICONS` is indexed by item id, so re-running the
generator would have drawn every cosmetic from id 8 up with the wrong picture,
and the only guard passes happily at 16. Latent until someone regenerated
icons, which is precisely what adding den items required. Pattern fixed and an
independent row count added that fails loudly.

---

# The original plan, kept for reference

## A. Flourishes have no consumer

`COSMETIC_FLOURISH` is now the only cosmetic slot nothing reads, one layer
below the gap the worn items were in until today. Three items - PACKET FLURRY
(Gremlin), SOLDER SPARKS (Gremlin), NOISE AURORA (Sentinel) - are craftable,
one of a kind, and do nothing.

**This is not urgent the way the worn gap was.** All three gate at Gremlin
Mode or Sentinel, so no new owner can reach them; a DEF CON unit at Packet Pup
cannot craft one. The worn gap bit immediately because ANTENNA HAT and SIGNAL
SCARF are two of the five Packet Pup recipes.

Two honest options, and they are not equal:

- [ ] **A1. Implement them.** A flourish is a periodic embellishment, not a
      worn overlay: it belongs on the home screen's idle loop and on a results
      screen, not stuck to a body part. That means a spawn/decay particle path
      with a frame budget - the round 480 panel repaints its pet at ~2 fps on
      the idle loop today, and confetti at that rate is a slideshow. Needs a
      real answer for the compact 160x80 panel, where there is no free space
      at all. Cost: comparable to the worn feature, most of it in the renderer
      rather than in state. `PetState` needs one byte,
      `wornSlots[]`-shaped, plus a schema bump to v5.
- [ ] **A2. Hide the three recipes until A1 exists.** One predicate in
      `UIInventory`, no schema change, no art. It stops the Kit advertising
      something that does nothing. It also removes three of the twelve
      recipes, which is a content cut, and it hides the aspirational
      "there is more at Sentinel" signal the locked rows currently carry.

**Recommendation: neither yet, and do not leave it silent.** The cheap correct
move is a third option: leave the recipes visible and make the Kit say so, the
way it now says `HAVE:DEN` / `HAVE:WORN`. `HAVE:FX` already prints; what is
missing is that FX currently means "and nothing happens". Decide A1 vs A2 when
a pet on this bench actually reaches Gremlin Mode, because until then nobody
can evaluate the result. **This is a content decision, not a defect.**

## B. Worn overlays do not reach the animated pixel sprite

`drawWornOn()` is called from every HD art site: home (all boards), den (all
boards), patrol radar (round panels), and the whole evolution cutscene. It is
NOT called for the 16x16 animated sprite the rectangular boards draw in the
patrol HUD corner, or for the home screen's non-resting animation frames.

- [ ] **B1. Decide whether this matters.** The anchor table is normalised to
      the 400 px HD canvas, and the pixel sprites are hand-drawn 16x16 art that
      is almost certainly cropped tighter. Reusing the table would put a hat
      through the pet's forehead. A fix needs its own anchor set measured
      against the pixel art, which is a second table to keep in step with the
      first for a 16 px corner icon on three boards.

**Recommendation: leave it, and state the limitation in the README.** The
affected surface is a corner sprite during an active scan on the rectangular
boards. Verify the claim first though: confirm on a T-Display S3 that the home
screen's ANIMATED states really do drop the overlay, because the resting state
is the one that was rendered and the animated one was reasoned about.

## C. Ghost text above the game footer rule

Carried forward from the previous pass, unchanged and still not reported by the
owner. Reproduces at BOTH 240 and 480, so it is pre-existing and not a scaling
bug: a partially-cleared status string sits just above the rule during play.

- [ ] **C1. Find the write that is not cleared.** Likely one games/ screen
      clearing a shorter rect than it prints. Cheap to bisect with
      `HEXHOUND_SIM_FORCE_SCREEN=game` and `HEXHOUND_SIM_GAME=<n>`, which now
      exists; it did not when this was found.

## D. The den has more slots than the game has furniture

Reduced but not closed. The room now grows with the collection, so nobody sees
dead brackets - but `DEN_SLOT_COUNT` is 8 and there are five den cosmetics, so
a fully-kitted Sentinel still tops out at 5 of a possible 8.

- [ ] **D1. Author three more den cosmetics, or lower the ceiling.** Lowering
      it is NOT free: `denSlots[]` is persisted at `DEN_SLOT_COUNT`, so
      shrinking the array is a schema change, and a save from a wider build
      would need its extra slots read and discarded rather than truncated
      blind. Authoring three more items is append-only and cannot break a save.

**Recommendation: author three.** It is the cheaper change AND the better game.
Requires icons from `gen_item_icons.py` and three recipes; no schema change.

## E. Verification debts

- [ ] **E1. The rectangular patrol layout still does not show materials.**
      Known since the 480 pass. Round-only today.
- [ ] **E2. `lilygo-t-dongle-c5-vendor-app` is unbuildable on this machine**
      (TLS interception). It is not in the 7-board set that gets built each
      time, so it can rot silently.
- [x] **E3. DONE 2026-08-30. The closet and den render correctly on both
      rectangular panel families.** Rendered on `desktop-sim` (160x80) and
      `desktop-sim-wave` (320x172): header, pet on the left wearing both
      overlays, two labelled rows, footer. No defect. Worth recording that the
      check found nothing, so nobody pays for it twice.
- [ ] **E3b. The rectangular closet wastes the lower half of the panel.**
      Found while doing E3. Two rows sit at the top and the pet sits low-left,
      leaving a dead band across the bottom on both panels. Not a defect and
      not reported; a polish item if that screen ever gets attention.

## Recommended order

1. ~~**E3**~~ - done, no defect found.
2. **B1** - confirm or refute the animated-sprite claim, then write the
   limitation down either way.
3. **D1** - three more den items.
4. **C1** - the ghost text, once someone is in the games code anyway.
5. **A** - flourishes, only once a bench pet reaches Gremlin Mode.

## Team assignment (2026-08-30)

All of the above is being done in parallel. Four worktree-isolated lanes plus
integration. Worktrees are mandatory here, not cosmetic: subagents otherwise
share one `.pio` build tree and corrupt each other's builds.

| Lane | Work | Owns |
|---|---|---|
| A | 3 new den cosmetics (D1); flourishes end to end, schema v5 (A1) | `item_defs.h`, `config.h`, `pet_core.*`, `pet_flourish.h`, `ui_flourish.*`, `item_art.h`, `gen_item_icons.py`, `gen_flourish_art.py`, `test_flourish/`, `test_hexpass.cpp` |
| B | Worn overlays on the animated pixel sprite (B1) | `animator.*`, `ui_worn.*`, `wear_anchors.h`, `gen_wear_anchors.py`, `ui_patrol_hud.*` |
| C | Game footer ghost text (C1) plus an audit for the same bug class | `src/games/**` |
| D | Rectangular patrol materials (E1); rect closet layout (E3b); prove or disprove the den 5-slot hypothesis | `ui_patrol.*`, `ui_closet.*`, `pet_rules.*` |
| me | `ui_home.*` integration, docs, merge, post-merge re-verification | `ui_home.*`, `docs/`, `tasks/`, `README.md`, `CONTRIBUTING.md` |

`ui_home.cpp` is deliberately owned by NOBODY but me: both A and B need a call
site in it, and two lanes editing one file is how a clean merge stops being
clean. Each writes the change it needs into `LANE_<x>_CALLSITE.md` and I apply
it. Same rule for any file outside a lane's list: stop and report, do not edit.

Lane B's first task is to CONFIRM OR REFUTE its own premise before writing
code. The claim that overlays do not reach the animated sprite came from
reading the call sites, not from looking at a render, and a lane that finds its
premise false and says so has done its job.

**After merging, re-measure the byte invariant.** Each lane measures against
its own base and cannot see the others' effect; the last multi-lane run needed
a post-merge check to catch the combined delta.
