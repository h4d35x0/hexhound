# W2 Content Engine - Integrator Wiring

Everything W2 built compiles and is unit-tested, but nothing calls it yet. This
file is the handover: what to call, in what order, and the decisions that were
deliberately left to the integrator.

The files that must be edited to finish the wiring (`src/main.cpp`,
`src/pet/pet_core.*`, `src/ui/ui_menu.*`, `src/config.h`) are owned by other
work, so W2 did not touch them.

## What was added

| File | Purpose |
|------|---------|
| `src/content/content_store.h/.cpp` | Loads and owns the dialogue and quest tables. Compiled-in baseline plus optional SPIFFS pack. |
| `src/content/content_baseline.h` | The shipped content, as data. 24 dialogue lines, 13 quest definitions. |
| `src/content/dialogue_engine.h/.cpp` | Picks a line for a context, expands `{n}` from a caller-supplied accessor. |
| `src/content/quest_engine.h/.cpp` | Rolls, tracks, rerolls and completes the daily quests. |
| `test/test_content/test_content.cpp` | 28 assertions across all three. Runs in `pio test -e native`. |

No existing file was modified.

## Boot order

`ContentStore::begin()` reads from SPIFFS, so it must run **after**
`StorageModule::init()` has mounted it. If it runs earlier it simply finds no
pack and uses the baseline, which is correct but throws away any content
update the device was shipped.

```cpp
StorageModule::instance().init();

ContentStore::instance().begin();
DialogueEngine::instance().begin(seed);
DialogueEngine::instance().setMemoryAccessor(petMemoryValue, nullptr);
QuestEngine::instance().begin(daySeed);
```

`seed` only needs to vary between boots; `esp_random()` or `millis()` at setup
is plenty. `daySeed` should be **stable for the whole day and different the next
day**, because that is what makes the quest list survive a reboot without being
saved field by field. Until there is a real clock, `PetState::saveSeq / 48` or a
stored day counter both work; anything monotonic per day does.

## The memory accessor is required for some content to appear

A dialogue line that wants to state a number declares a `MemorySlot` and writes
`{n}` in its text. The engine asks the accessor. **Until an accessor is
installed, every line that declares a slot is skipped entirely** rather than
shown with a zero in it, so content can never print a statistic the firmware did
not supply. Six of the 24 baseline lines are in that group.

Write the accessor next to pet state; the engine deliberately knows nothing
about `PetCore`:

```cpp
static uint32_t petMemoryValue(uint8_t slot, void* user) {
    const PetState& s = PetCore::instance().state();
    switch (slot) {
        case MEM_SLOT_VISITS:       return s.interactions;
        case MEM_SLOT_PATROLS:      return s.wifiScans;
        case MEM_SLOT_NEW_NETWORKS: return s.seenWifiCount + s.seenBleCount;
        case MEM_SLOT_THREATS:      return PetCore::instance().totalThreatFindings();
        case MEM_SLOT_QUESTS_DONE:  return /* needs a lifetime counter, see below */ 0;
        case MEM_SLOT_DAYS_AWAY:    return /* needs a clock, see below */ 0;
        case MEM_SLOT_BEST_SCORE:   return /* W3 minigame high score */ 0;
        default:                    return 0;
    }
}
```

Three slots have no source in `PetState` today. **Do not wire them to a
plausible-looking substitute.** Either add the real counter or leave the slot
returning 0; a pet that says "you were gone 0 days" is a smaller problem than a
pet that confidently reports a number nobody measured. `MEM_SLOT_QUESTS_DONE`
wants a lifetime `questsCompleted` field on `PetState` (a new optional field, so
no `PET_SCHEMA_VERSION` bump is needed); `MEM_SLOT_DAYS_AWAY` needs a stored
last-seen timestamp.

## Where to call dialogue

`pick()` returns `nullptr` when nothing matches, and every caller must treat
that as "draw no bubble". It is a normal outcome, not an error.

```cpp
const PetState& s = PetCore::instance().state();
const char* line = DialogueEngine::instance().pick(
    DLG_GREETING, s.traits[0], s.traits[1], s.stage);
if (line) { /* draw it */ }
```

The returned pointer is valid until the next `pick()`. Draw it immediately or
copy it; do not store the pointer.

Suggested call sites, matching the contexts in `content_types.h`:

| Context | Call site |
|---------|-----------|
| `DLG_GREETING` | screen wake / home screen entry |
| `DLG_IDLE` | home screen ambient timer |
| `DLG_PATROL_DONE` | `SCREEN_PATROL` entry |
| `DLG_THREAT_FOUND` | `SCREEN_ALERT` entry |
| `DLG_QUEST_DONE` | on a `true` return from `reportProgress` |
| `DLG_GAME_WIN` / `DLG_GAME_LOSE` | W3 minigame result screen |
| `DLG_RETURN_AFTER_ABSENCE` | boot, when the last-seen gap is large |
| `DLG_HIBERNATE_WAKE` | wake from light sleep |
| `DLG_EVOLVED` | `SCREEN_EVOLVE` |

## Where to report quest progress

`PetRules` already handles the events these map onto, so its handlers are the
natural place:

```cpp
if (QuestEngine::instance().reportProgress(QUEST_CYBER, 1)) {
    // at least one quest just completed: pay the reward, show DLG_QUEST_DONE
}
```

Rewards are not paid by the engine; it only tracks. On a `true` return, walk the
slots, and for each `QUEST_COMPLETE` quest not yet paid, read
`QuestEngine::instance().definitionFor(i)` for `rewardXP` and `rewardBond` and
apply them through `PetCore`. Keeping the reward out of the engine is what stops
a content pack from writing its own XP grant.

Mapping from existing events:

| Event | Call |
|-------|------|
| interaction / feed | `reportProgress(QUEST_CARE)` |
| WiFi or BLE scan completed | `reportProgress(QUEST_CYBER)` |
| new capture seen | `reportProgress(QUEST_EXPLORE)` |
| threat opened / tracker found | `reportProgressById("cyber.triage")` / `("cyber.tracker")` |
| IMU step or shake | `reportProgress(QUEST_EXPLORE)` on IMU boards only |
| `QUEST_LIFE` | user-confirmed from the quest screen; nothing else can observe it |

## Quest screen (`SCREEN_QUESTS`)

The enum value already exists in `config.h`. The screen needs:
`activeCount()`, `quest(i)` for progress and status, `definitionFor(i)->text`
for the label, and a reroll action gated on `rerollsLeft()`.

`activeCount()` can legitimately be **less than** `QUEST_MAX_ACTIVE`, or zero, if
a pack leaves this board with too few eligible definitions. The screen must
render that, not assume four rows.

## Not done, and deliberately

* **Quest persistence.** Nothing saves the day's quests or their progress; a
  reboot rerolls the day. Fixing it properly means adding a `quests` object to
  the pet save, which is `pet_core.cpp` territory. The engine is ready for it:
  a `QuestInstance` is 32 bytes of plain data and `rollDaily(seed)` is
  reproducible, so a save needs only the seed plus each slot's progress and
  status.
* **Rewards.** See above; by design.
* **`ui_menu` entry.** The menu owner adds the `SCREEN_QUESTS` row.

## Content pack format

Optional, read from SPIFFS at `/content/dialogue.json` and `/content/quests.json`
(and from `content/*.json` beside the executable in the desktop sim, so content
can be reviewed without hardware). A pack replaces the matching baseline table
only if it parses and yields at least one usable row; anything else leaves the
baseline in place. Each file is capped at 6 KB and truncated at 40 lines /
20 quests.

```json
{ "lines": [
  { "text": "You again. That is {n} check-ins now.",
    "context": "greeting", "trait": "curious", "stage": 2, "memory": "visits" }
]}
```

```json
{ "quests": [
  { "id": "explore.steps", "text": "Walk 200 steps together.",
    "kind": "explore", "target": 200, "xp": 30, "bond": 4, "requires": ["imu"] }
]}
```

`context`, `trait`, `kind`, `memory` and `requires` take names or raw ordinals;
the accepted spellings are the name tables at the top of `content_store.cpp`.
`stage` is the numeric `PetStage`. Omitted `trait`/`stage`/`memory` mean "any" /
"no substitution".

`requires` is enforced: a quest naming a capability this board lacks is never
offered. On the T-Dongle S3 that removes the two IMU quests from the baseline,
leaving 11 of 13.

## Cost

Measured on `lilygo-t-dongle-s3-vendor-app`:

* RAM: **+6,872 bytes** (71,636 -> 78,508 of 327,680; 21.9% -> 24.0%) once the
  singletons are referenced. `ContentStore` is 6,606 of that, being the two
  fixed tables.
* Flash: +14,112 bytes.

The tables land in `.data` rather than `.bss` because `DialogueLine` and
`QuestDef` have non-zero default members, so the baseline costs its size in the
flash image as well. That is 0.2% of the partition and was not worth contorting
the shared structs to avoid.
