# HexPass threat model and protocol specification

**Status: DRAFT, still awaiting overall sign-off. No BLE advertising code exists
yet.**

**2026-08-03: the two decisions that were blocking the radio are settled.**
The maintainer decided residual risk 3 (drop badge and greeting from the
passive broadcast) and residual risk 4 (accept and disclose the plaintext
secret). Both
are recorded in full under "Residual risks", and 3 has been applied to the
payload spec.

That is a decision on those two items, NOT a sign-off on this document as a
whole. The rest of the model has not been reviewed, so the "no radio code until
reviewed" bar still stands.

**Revision 2.** The non-radio implementation was built strictly to revision 1
and the team building it was told to report anything it thought was wrong
rather than quietly work around it. It found six problems, one of which made a
headline feature impossible as specified. All six are corrected below and
marked where they were wrong, rather than silently edited, because the reasoning
matters more than the conclusion:

1. FriendID could not support repeat-encounter friendships at all
2. "the epoch key" was undefined and could not mean what it implied
3. the cosmetic payload is an 11 bit fingerprint, which is a number, not a
   judgement call
4. "at most N per day" never defined N
5. the secret at rest was not discussed
6. the epoch encoding was unspecified

That the implementation was built to a flawed spec and reported the flaws is
the process working. Had it silently "fixed" them, the spec and the code would
now disagree and nobody would know which was intended.

HexPass lets two HexHounds that pass near each other exchange a small public
pet card, so carrying the device to a conference, a class or an office is worth
something. It is also, structurally, a device that broadcasts an identifier in
public on a repeating basis. That is the same shape as a tracking beacon, and
the only thing separating the two is design discipline.

This is a security product. A HexHound that can be followed across a venue would
be a worse outcome than never shipping HexPass at all.

## What is being protected, and from whom

| Asset | Threat | Why it matters |
|---|---|---|
| The owner's location over time | Passive sniffer correlating sightings | Following a person around a venue, or across days |
| The owner's identity | Linking a HexHound to a name | The pet name is user-chosen and may be their handle |
| The owner's network history | Anything derived from `seenWifi` / `seenBle` | This is reconnaissance data about THEIR environment |
| Other people's devices | Rebroadcast of scanned MACs or SSIDs | HexHound would become a distributed tracker of third parties |
| The encounter record | Tampering, flooding, forgery | Fake friendships, inflated stats, denial of service |

The adversary to design against is not sophisticated: it is one person with a
laptop, a cheap BLE dongle and a directory of the venue. Anything that beats
that also beats a bored attendee, which is the realistic case at a conference.

## Never transmitted, under any circumstance

This list is not advisory. Anything on it appearing in a payload is a defect of
the highest severity, not a feature request to reconsider.

- The device MAC address, or any value derived from it
- Any SSID, BSSID or BLE address the device has scanned
- Any location, coordinate, or venue identifier
- Any free-form text the user can author
- Raw `seenWifi` / `seenBle` capture sets, or counts precise enough to
  fingerprint an environment
- Battery level, uptime, or anything else that adds entropy to a fingerprint
- Any persistent identifier at all

## Identity: rotating ephemeral identifiers

Each device holds a 32-byte `hexpassSecret`, generated once from `esp_random()`
and never transmitted. The broadcast identifier is derived:

    EID(epoch) = HMAC-SHA256(hexpassSecret, "hexpass-eid" || epoch)[0..15]

`epoch` increments on a fixed interval (proposed: 15 minutes). Only 16 bytes are
broadcast. The secret never leaves the device, so an observer cannot compute
past or future identifiers from a captured one.

Two devices that have met derive a shared value:

    FriendID = HMAC-SHA256(sort(EID_a, EID_b), "hexpass-friend")[0..7]

Sorted so both sides compute the same value.

### CORRECTION: FriendID cannot support repeat-encounter friendships

The first draft of this document claimed FriendID is "what makes *you have met
R00tPup three times* work". **That was wrong, and it was wrong in a way that
made a headline feature impossible.**

Both devices' EIDs rotate independently every epoch, so the same pair of
devices derives a COMPLETELY DIFFERENT FriendID in every 15 minute window.
Recognition works only within a single epoch. Across days it is impossible by
construction, not merely difficult.

The error was trying to extract a persistent identity from a system whose
entire purpose is to destroy persistent identity. Those two goals cannot both
be served by the same mechanism, and no amount of care in the derivation
changes that.

The knock-on is also real: the encounter ring stores one entry per
(peer, epoch), so somebody sitting near you all day consumes about four of the
sixteen slots per hour rather than one slot total.

**The resolution is to split passive passing from deliberate pairing**, which
is both correct and better for privacy:

- **Passive passing stays anonymous.** You learn a count and earn materials:
  "twelve hounds passed you today". No identity, no linkage, nothing to follow.
  This is what the rotation is FOR, and it works perfectly for this.
- **Named, repeat friendships require a deliberate pairing** where both owners
  choose to exchange a persistent friend key. Linkability between two devices
  is exactly what both parties are consenting to at that moment, so a stable
  identifier is appropriate there and nowhere else.

This is the same boundary the document already draws for pet names. Names and
persistent friendships belong on the same side of it.

### The mistake this design exists to avoid

**Rotating the payload identifier while the BLE MAC stays fixed accomplishes
nothing.** The MAC becomes the tracking identifier and every clever thing above
is decoration. ESP32 BLE defaults to a static public address.

Therefore: the BLE address MUST be a non-resolvable private address, and it MUST
rotate **in the same instant** as the EID. If the two rotate on different
schedules, the overlap window links old identifier to new, and an observer
rebuilds the whole chain. A test must assert that address rotation and EID
rotation share one trigger, not two timers that happen to have equal periods.

This is the single most likely way to get HexPass wrong, and it is easy to get
wrong while every unit test passes.

## Payload

Fixed size, fixed shape, no optional fields. A variable-length payload leaks
information in its length alone.

There are TWO payloads, and the split is deliberate: see decision 3 under
residual risks. The passive broadcast carries the minimum that makes a passing
encounter mean anything. Everything that identifies a specific owner is held
back for the deliberate pairing exchange, where the owner has consented.

### Passive broadcast

| Field | Bytes | Notes |
|---|---|---|
| version | 1 | Reject unknown versions rather than guessing |
| EID | 16 | Rotating, as above |
| stage | 1 | Age axis, 1 to 5 |
| form | 1 | Behavioural form, 0 to 6 |
| counter | 4 | Monotonic per epoch, for replay detection |
| tag | 8 | Integrity check. See below: this does NOT prove origin |

Total 31 bytes, inside one BLE advertisement. `epoch` is encoded little-endian
32-bit in the EID derivation message; leaving that unspecified would have been
a live interop bug the moment a second implementation existed.

`stage x form` is 35 combinations, about 5 bits. In a hall of 300 units that is
roughly 8 or 9 units sharing each profile, which is a crowd rather than an
identifier.

### Paired exchange only

| Field | Bytes | Notes |
|---|---|---|
| badge | 1 | ONE selected achievement id, chosen by the owner |
| greeting | 1 | Index into a FIXED preset table. Never free text |

These are NOT advertised. They are sent only during a pairing both owners
initiated, so the personality still shows for the people you actually paired
with, and the identifying bits sit behind consent. An implementation that puts
either field back into the advertisement has reintroduced the 11 bit
fingerprint and defeated rotation; treat that as a defect, not a tradeoff.

### The tag is an integrity check, not a signature

The first draft said "HMAC over the above with the epoch key" without defining
the epoch key, which invited a reading it cannot support. A receiver has no
access to the sender's secret, so a secret-keyed tag would be unverifiable.

The tag is therefore keyed from the EID, which is public in the payload. It
detects corruption and casual editing. **Anyone can forge a well-formed card
from scratch**, and nothing in this design prevents that.

That is acceptable, but only because of what the payload contains: cosmetic
stats with no authority attached. Forging one buys an attacker a fake encounter
in somebody's counter, bounded by the rate limits below. It must never be built
on for anything that matters. If HexPass ever carries something that does
matter, it needs real signatures and a key distribution story, which is a
different project.

The pet NAME is deliberately absent. A user-chosen name is frequently a handle,
and a handle is an identity. Names are exchanged only after both sides have
opted into a deliberate pairing, which is a separate flow from passive passing.

## Replay and flooding

- `counter` is monotonic within an epoch. A repeat of `(EID, counter)` is
  discarded.
- `tag` is an HMAC under the epoch key, so a payload cannot be edited without
  detection. It does not prove identity to a stranger, and is not claimed to.
- Encounters from one EID are rate limited: at most one recorded per epoch,
  **at most 4 per peer per day, and at most 32 recorded per day in total**.
  The first draft said "at most N per day" without ever giving N, and phrased
  it as governing one EID, which cannot survive a day since an EID does not.
  A sniffer replaying a captured card cannot inflate anyone's stats.
- The encounter store is a fixed-size ring. It cannot be grown by an attacker
  into a memory problem, and eviction is oldest-first.

## Consent and control

- **Off by default.** HexPass transmits nothing until the owner explicitly
  enables it. Not opt-out, not on-with-a-notice.
- **Private mode**: a one-action toggle that stops advertising immediately.
- **Block list**: a blocked FriendID is never recorded again.
- **Local wipe**: erase all encounters, friendships and the secret. Erasing the
  secret makes every previously derived identifier unlinkable to the new one,
  so a wipe is a genuine reset rather than a cosmetic one.
- The device must be able to say plainly what it is broadcasting. A user who
  cannot inspect it cannot consent to it.

## Residual risks I cannot resolve alone

These need a human decision. They are listed because they are real, not to
transfer blame.

1. **Presence is inherently observable.** Rotating identifiers stop LINKING
   sightings; they do not hide that a HexHound is present. In a room with one
   HexHound, an observer knows whose it is. This is inherent to any proximity
   feature and cannot be engineered away, only disclosed.
2. **Rotation interval is a tradeoff.** Shorter is more private and makes
   repeat-encounter friendships less reliable. 15 minutes is a proposal, not a
   derived answer.
3. **The payload was an 11 bit fingerprint, and that number defeated rotation.
   DECIDED 2026-08-03 by the maintainer: drop badge and greeting from the passive
   broadcast.**
   The first draft called this "needs a judgement call" without doing the
   arithmetic, which understated it. stage(5) x form(7) x badge(8) x
   greeting(8) = **2240 combinations, about 11 bits**. In a hall of a few
   hundred units that is more than enough to link a device across a rotation
   boundary by its cosmetic profile alone, which defeats the rotation the whole
   design rests on.

   The decision: badge and greeting are sent only in the deliberate pairing
   exchange, never advertised. Residual passive entropy is stage x form, 35
   combinations or about 5 bits. Coarsening form into buckets instead was
   rejected because it only reaches about 10 bits, paying a real cost in lost
   detail without buying the rotation back. Accepting the full payload was
   rejected because this is a security company's giveaway at DEF CON, where
   somebody in the hall is logging BLE, and a rotation that does not rotate is
   worse than no rotation claim at all.

   See the two payload tables above. This was the item that had to be settled
   before the radio was written, and it now is.
4. **The secret is stored in the clear. DECIDED 2026-08-03 by the maintainer: accept
   and disclose.** It is written to `pet_state_{a,b}.json` alongside everything
   else. There is no secure element and no key the firmware could withhold from
   itself, so anyone who reads the flash or the SD card derives every past and
   future EID and rotation stops meaning anything for that attacker. The
   mitigating argument is that such an attacker already has the physical device.

   Accepted as a known property, to be disclosed here and in the tester guide,
   because the attack requires physical possession and HexPass is opt-in and
   defaults off, which already bounds the exposure.

   Both alternatives were rejected on their costs. ESP32-S3 flash encryption
   would genuinely fix it but is irreversible per unit, is a brick risk on a
   giveaway device, and interacts with the OTA path: the whole update flow would
   need re-verifying under encrypted flash. Not persisting the secret removes
   the exposure entirely but resets every friendship on every reboot, which is
   most of what HexPass is for.
5. **Conference density.** Hundreds of units in one hall is a different problem
   from two friends meeting, both for the ring buffer and for whether encounters
   still feel meaningful. Untested, and untestable without the units.

## What is being built before sign-off

Everything that does not transmit: the secret and its generation, EID and
FriendID derivation, the encounter store and its eviction, replay and rate
limiting, the block list, the opt-in setting defaulting to off, local wipe, and
the UI. All of it is unit-testable with no radio.

**BLE advertising and scanning are NOT being written until this document has
been reviewed.** The parts most likely to go wrong are the parts that touch the
air, and they are the ones that cannot be quietly fixed later once units are in
the wild.
