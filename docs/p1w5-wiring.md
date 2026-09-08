# P1-W5 Minigames - Integration Notes

Two more games under `src/games/`: **Firewall Frenzy** and **Cipher Sprint**.
Both implement `Minigame` and both are in the registry, so on this branch they
are already reachable. This file is the handover anyway, because the cost, the
scoring shape and the one behaviour change on the select screen all belong
somewhere written down.

## Files added

| File | What it is |
|------|-----------|
| `src/games/firewall_frenzy.h` / `.cpp` | One-button allow/block judgement game. Implements `Minigame`. |
| `src/games/cipher_sprint.h` / `.cpp` | One-button substitution/pattern puzzles. Implements `Minigame`. |
| `src/games/game_widgets.h` | Header-only widgets both new games use: `ChipRow` (the sweep selector) and `DrainBar` (the visible deadline). Allocates nothing. |

One existing file changed: `src/games/game_registry.cpp`, two lines in the
table. Nothing else in the tree was touched. `build_src_filter` is `+<*>` on
every firmware and simulator environment, so the new translation units are
picked up with no `platformio.ini` change.

`src/games/game_layout.h` was **not** modified. It stays the only place that
knows a panel dimension; both games derive everything from `games::playArea()`,
`games::status()` and `games::chromeInit()`, and `game_widgets.h` takes a
`games::Rect` from the caller rather than looking at `SCREEN_W` itself.

## Wiring required: none

`main.cpp` already runs any registered game generically (`SCREEN_GAME_PLAY`,
`g_activeGame`, `endActiveGame()`), and `UIGames` already builds its list from
`games::playableAt()`. Both new games declare `requiredCaps() == 0`, so they are
offered on every board. Stable ids for a future high-score store:

```
firewall_frenzy
cipher_sprint
```

### One behaviour change to be aware of

`GAMES_PER_PAGE` is 4 on the 160x80 panel and the list is now `BACK` plus four
games, so **the game select screen paginates on the T-Dongle for the first
time** (it shows `1/2` in the header and scrolls). That path already exists and
was built for it, but this is the first build where it actually happens.

## The games

### Firewall Frenzy (`FIREWALL`)

Packets arrive one at a time as three lines - protocol, origin, one observation
- and the player allows or blocks. The cursor alternates between an `ALLOW`
chip and a `BLOCK` chip and a short press commits whichever it is on, the same
sweep-and-commit `signal_memory` uses. A bar above the card drains over the
packet's deadline and turns orange then red, so the clock is always readable.

It is a decision, not a reflex test. The deck has 24 packets in two halves:

* 12 whose answer is obvious from one line (`SRC SPOOFED / REV SHELL`).
* 12 where the third line decides and cuts both ways:

  | Packet | Call | Why |
  |--------|------|-----|
  | `TCP 22 SSH / SRC UNKNOWN / KEY MATCHED` | allow | the key is the identity, not the address |
  | `TCP 22 SSH / SRC UNKNOWN / KEY CHANGED` | block | that is the whole attack |
  | `TCP 3389 RDP / SRC VPN / USER ADMIN` | allow | admin over the VPN is work |
  | `TCP 3389 RDP / SRC WAN / USER ADMIN` | block | admin off the WAN is not |
  | `UDP 5353 MDNS / SRC LAN / BURST x40` | allow | mDNS bursts are normal on a LAN |
  | `UDP 53 DNS / SRC LAN / TXT 900B` | block | that is exfiltration, not lookup |

  Seven of the twelve are blocks and five are allows, deliberately: if the
  ambiguous half were one-sided, "always block" would win and the game would be
  a reflex test again.

The share of ambiguous packets rises from 25% to 70% across the round, so the
first packets teach the rules and the last ones test them. Three lives; a wrong
call costs one and so does an expired deadline, because an unexamined packet is
one that got through. Thirty packets is a full round, about 90 seconds.

**Score:** 10 per correct call, 20 when the packet was an ambiguous one. A
perfect 30-packet round scores 400 to 520 depending on the deal.

### Cipher Sprint (`CIPHER`)

Eight short puzzles against the clock. Each shows a rule demonstrated once and
then asks for one application of it:

```
NET->OFU        the rule, worked
KEY->???        the question
[LFZ] [MGA] [YEK]   three answers, cursor sweeping
```

Three families: a Caesar shift (+/-1 to 4), a reversal, and an arithmetic
sequence (`3 6 9 12` / `NEXT ?`). Everything is three letters or two digits
wide, because this is a two-minute loop on a 160x80 panel and not a crossword.

The generator **checks that the example identifies exactly one rule** before it
uses it: an example that fits both a shift and a reversal would leave the
challenge with two defensible answers, and a player who picked the other one
would be right to call the game broken. If sixteen draws somehow fail that
check it falls back to a pair that is provably unambiguous.

Distractors are the other rule applied to the same word, then neighbouring
shifts, so a player who mis-read the example finds their mistake on offer
rather than a random string.

Three lives, eight puzzles. Wrong answer or expired deadline costs one.

**Score:** 100 per solve plus up to 60 for speed, so answering fast is worth
something but guessing fast is not. A clean 8/8 run scores 1150 to 1250.

Score scales are not comparable between games and are not meant to be, exactly
as `docs/w6-wiring.md` says for the first two.

## Panel families

Nothing in either game contains a coordinate. Both lay out the same three
bands inside `games::playArea()`: deadline bar, card, chip row, with the chip
row a third of the play height and the card text sized to the longest string it
will ever hold.

| Family | Play rect | Card text | Chips |
|--------|-----------|-----------|-------|
| 160x80 | 154x50 at (3,15) | size 1 | 2 or 3 across 154 px |
| 320x172 | 308x110 at (6,34) | size 2 | same, roomier |
| 240x240 round | 184x128 at (28,58) | size 2 | same |

Titles are shortened on the round panel only (`FIREWALL`, `CIPHER`), because
the title row there is a 128 px chord. Folded at compile time, costs nothing
elsewhere. Rendering was checked by driving both games through the SDL sim on
all three families and capturing frames.

## Memory

Measured on `lilygo-t-dongle-s3-vendor-app` (the no-PSRAM design target), by
building the tree with and without these files rather than by estimating. Both
games are reachable in both builds, so this is the real delivered cost, not a
garbage-collected zero.

| Build | RAM | Flash |
|-------|-----|-------|
| Without Firewall Frenzy and Cipher Sprint | 78,916 | 1,165,941 |
| With them | 79,300 | 1,174,801 |
| **Cost** | **+384 (24.1% -> 24.2%)** | **+8,860** |

On `waveshare-esp32-s3-lcd-128` (round): RAM 76,784 -> 77,168 (**+384**), flash
1,499,549 -> 1,508,877 (**+9,328**).

`xtensa-esp32s3-elf-nm -S` accounts for every one of those 384 bytes:

```
0000b4 b FirewallFrenzy::instance()::g     180 B
0000ac b CipherSprint::instance()::g       172 B
000008 b guard variable for each            16 B
                       registry table       16 B  (two more pointers)
                                          -----
                                            384 B
```

For comparison the two existing games are 108 B (`PacketChase`, `.data`) and
136 B (`SignalMemory`, `.bss`). The new pair is larger because each carries its
`ChipRow` (about 60 B: three chip rects, labels, colours) and its `DrainBar`.
`ChipRow::MAX_CHIPS` is 3, which is what these two games need; raising it costs
about 22 bytes of `.bss` per slot per game and should be a deliberate choice.

**Heap while a round is running:** Firewall Frenzy 24 bytes (the deck of table
indices), Cipher Sprint 52 bytes (one puzzle). One game at a time, allocated in
`begin()`, freed in `end()`, and `update()` never touches the heap.

## Verified

| Environment | Result |
|-------------|--------|
| `lilygo-t-dongle-s3-vendor-app` | SUCCESS, RAM 79,300 / flash 1,174,801 |
| `waveshare-esp32-s3-lcd-128` (round) | SUCCESS, RAM 77,168 / flash 1,508,877 |
| `desktop-sim` (160x80) | SUCCESS |
| `desktop-sim-round` (240x240) | SUCCESS |
| `desktop-sim-wave` (320x172) | SUCCESS |
| `pio test -e native` | 6/6 suites PASSED |

### Behavioural verification

Compiling proves nothing about a game loop, so both games were driven through
complete rounds by a throwaway harness (stub HAL, virtual clock, global
`new`/`delete` counters, bounds-checking display), built for all three panel
families. 203 rounds per family, 609 in total, five input policies: never press
the button, mash it every frame, press randomly, play the correct answer, and
play the wrong answer on purpose.

```
allocations in update()   0
frees in update()         0
net live allocations      0     (everything begin() took, end() gave back)
rounds that failed to end 0     (longest 59.0 s, budget 800 s)
fillScreen calls          0
draws outside the panel   0
draws behind the bezel    0     (round build, checked against the glass circle)
max draw ops per frame    18 rect panels, 130 on the round game-over frame
```

The harness reads the games' own rendering rather than their private state, so
these are checks a person in front of the panel could make:

* **Every Cipher Sprint puzzle has exactly one right answer.** The harness
  parses the rule off the card, derives the answer itself, and asserts one and
  only one chip carries it. A player following that answer finishes `8/8` and
  the end screen says `DECRYPTED`.
* **Firewall Frenzy is winnable and losable.** A player following the verdict
  table finishes `30/30` with `SHIFT OVER`; a player who inverts every call
  reaches `BREACHED` with all three lives gone. The harness holds its own copy
  of the verdict table, so if the game's table changes and the two disagree,
  the perfect run stops being perfect and it says so.
* **`end()` is idempotent.** Every round calls it twice and asserts the second
  call frees nothing more and does not restate `result()`. The abandon-before-
  first-update path (`begin()` then `end()` then `end()`) is checked separately
  and leaks nothing, and `update()` after `end()` reports the round over rather
  than crashing.

The harness is not in the tree.
