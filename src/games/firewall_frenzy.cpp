#include "firewall_frenzy.h"

#include "../hal/tft_compat.h"

#include <stdio.h>

// ── HexHound - Firewall Frenzy Implementation ───────────────────
//
// Same discipline as packet_chase.cpp and signal_memory.cpp:
//
//  * The deck is allocated once in begin() and freed in end(). update() never
//    touches the heap, and every string it draws is either a literal from the
//    table below or a fixed stack buffer.
//  * Nothing clears the whole panel during play. The card region is cleared
//    once per packet (roughly every two seconds, which reads as a new packet
//    arriving rather than as a flicker), the chips repaint their own rects and
//    the deadline bar repaints only the pixels that just expired.
//  * update() early-returns until FRAME_MS has elapsed, so being called every
//    main-loop iteration costs one subtraction.

namespace {

constexpr uint32_t FRAME_MS     = 33;
constexpr uint32_t INTRO_MS     = 1400;
constexpr uint32_t VERDICT_MS   = 850;
constexpr uint32_t OVER_HOLD_MS = 1600;

constexpr uint16_t COL_TITLE  = TFT_ORANGE;
constexpr uint16_t COL_ALLOW  = TFT_GREEN;
constexpr uint16_t COL_BLOCK  = TFT_RED;

// ── The deck ──────────────────────────────────────────────────────────────
//
// Every field is at most 14 characters, which is what makes the card fit the
// 160x80 panel at text size 1 and the other two families at size 2 without any
// per-panel special casing.
//
// `hard` marks a packet whose verdict needs all three lines. Those are dealt
// more often as the round goes on; the obvious ones exist so the rules are
// learnable before they are tested. Order here does not matter: begin() sorts
// the deck into an obvious partition and an ambiguous one by this flag.

struct PacketDef {
    const char* proto;
    const char* origin;
    const char* note;
    // WHY the right call is right, in one line, shown when the player gets it
    // wrong. The code already showed which chip was correct and said nothing
    // about the reason, which is fine for the obvious packets and useless for
    // the ambiguous ones: being told you were wrong about SSH from an unknown
    // source teaches nothing unless you are also told that a matching host key
    // is what settles it. Reported from the board as two calls that were
    // marked wrong when they felt right.
    const char* why;
    uint8_t     block;   // 1 when the correct call is BLOCK
    uint8_t     hard;    // 1 when the verdict needs all three lines
};

// THE RULE THIS GAME TEACHES: the third line decides.
//
// The origin sets the suspicion and the third line settles it, in both
// directions. That rule only works if the third line is genuinely decisive, and
// two rows here were not - they were marked ambiguous and then answered on
// information the player was never shown:
//
//   UDP 5353 MDNS / SRC LAN / "BURST x40"   answered ALLOW
//     A burst of forty is what a scan looks like. Nothing on the card said
//     this was ordinary name discovery, so BLOCK was the better reading of
//     what was actually on screen. Now "NAME QUERY", which is decisive.
//   TCP 3389 RDP / SRC VPN / "USER ADMIN"   answered ALLOW
//     Administrative RDP is exactly what a careful person blocks, and "over a
//     VPN" is not on its own a reason to allow it. Now "MFA PASSED", which is.
//
// Both were reported from the board as calls that were marked wrong when they
// felt right, and on the cards as printed the player was right and the game
// was wrong. Fixing the cards rather than the answers keeps the rule intact.
const PacketDef PACKETS[] = {
    // Obvious hostile.
    { "TCP 4444",      "SRC SPOOFED",  "REV SHELL",    "4444 IS A SHELL PORT",    1, 0 },
    { "TCP 23 TELNET", "SRC UNKNOWN",  "PLAINTEXT PW", "TELNET SENDS CLEAR PW",   1, 0 },
    { "TCP 445 SMB",   "SRC WAN",      "EXPLOIT SIG",  "SMB MUST NOT FACE WAN",   1, 0 },
    { "UDP 53 DNS",    "SRC UNKNOWN",  "TUNNEL 4KB",   "4KB IN DNS IS A TUNNEL",  1, 0 },
    { "TCP 80 HTTP",   "SRC BOTNET",   "SYN x9000",    "SYN FLOOD",               1, 0 },
    { "ICMP ECHO",     "SRC SPOOFED",  "PAYLOAD 64KB", "PING CARRIES NO PAYLOAD", 1, 0 },
    // Obvious benign.
    { "TCP 443 TLS",   "SRC KNOWN",    "CERT VALID",   "KNOWN HOST VALID CERT",   0, 0 },
    { "UDP 123 NTP",   "SRC GATEWAY",  "SIGNED OK",    "SIGNED TIME FROM GW",     0, 0 },
    { "TCP 22 SSH",    "SRC LAPTOP",   "KEY MATCHED",  "KNOWN HOST KNOWN KEY",    0, 0 },
    { "UDP 53 DNS",    "SRC RESOLVER", "REPLY OK",     "NORMAL DNS REPLY",        0, 0 },
    { "TCP 631 IPP",   "SRC PRINTER",  "LAN ONLY",     "PRINTING STAYS ON LAN",   0, 0 },
    { "ARP REPLY",     "SRC GATEWAY",  "MAC MATCHED",  "MAC IS THE KNOWN ONE",    0, 0 },
    // Ambiguous: the third line decides, and it cuts both ways.
    { "TCP 22 SSH",    "SRC UNKNOWN",  "KEY MATCHED",  "THE HOST KEY SETTLES IT", 0, 1 },
    { "TCP 22 SSH",    "SRC UNKNOWN",  "KEY CHANGED",  "CHANGED KEY IS A MITM",   1, 1 },
    { "TCP 443 TLS",   "SRC UNKNOWN",  "CERT EXPIRED", "EXPIRED CERT PROVES NIL", 1, 1 },
    { "TCP 443 TLS",   "SRC NEW HOST", "CERT VALID",   "NEW IS NOT UNTRUSTED",    0, 1 },
    { "ARP REPLY",     "SRC UNKNOWN",  "MAC CHANGED",  "ARP SPOOF",               1, 1 },
    { "UDP 5353 MDNS", "SRC LAN",      "NAME QUERY",   "MDNS IS LAN DISCOVERY",   0, 1 },
    { "TCP 3389 RDP",  "SRC VPN",      "MFA PASSED",   "MFA OVER VPN IS ENOUGH",  0, 1 },
    { "TCP 3389 RDP",  "SRC WAN",      "USER ADMIN",   "ADMIN RDP OFF THE WAN",   1, 1 },
    { "UDP 53 DNS",    "SRC LAN",      "TXT 900B",     "900B TXT IS EXFIL",       1, 1 },
    { "TCP 80 HTTP",   "SRC LAN",      "POST 12MB",    "12MB OUT IS EXFIL",       1, 1 },
    { "TCP 8080",      "SRC DEV BOX",  "SIGNED BUILD", "SIGNED BUILD FROM DEV",   0, 1 },
    { "TCP 25 SMTP",   "SRC PRINTER",  "RELAY 200",    "A PRINTER IS NO RELAY",   1, 1 }
};

constexpr int PACKET_N = (int)(sizeof(PACKETS) / sizeof(PACKETS[0]));

// Longest field above, which is what the card is sized for.
constexpr int CARD_COLS = 14;

const char* const CHIP_LABEL[2] = { "ALLOW", "BLOCK" };

}  // namespace

FirewallFrenzy& FirewallFrenzy::instance() {
    static FirewallFrenzy g;
    return g;
}

// ── Cadence ───────────────────────────────────────────────────────────────
//
// Both timings tighten with the round and then floor, so late packets are
// brisk without ever becoming faster than a person can read three lines.

// Packets per level. The ramp used to be per PACKET, 80 ms at a time, which
// meant no two decisions ever had the same budget and there was nothing to
// notice getting harder - the difficulty was real but invisible.
static const uint8_t PACKETS_PER_LEVEL = 5;

uint8_t FirewallFrenzy::level() const {
    return (uint8_t)(_judged / PACKETS_PER_LEVEL + 1);
}

uint32_t FirewallFrenzy::deadlineMs() const {
    // 6000 ms at level 1, 700 off each level, floor 3200.
    //
    // It was 4600 falling to 2400, and 2400 ms is not enough to read a
    // three-line packet card and decide - reported from the board as wanting
    // "a couple more seconds". The floor matters more than the start: the
    // hardest level has to stay playable, not merely reachable.
    const int32_t v = 6000 - (int32_t)(level() - 1) * 700;
    return (uint32_t)(v < 3200 ? 3200 : v);
}

uint32_t FirewallFrenzy::dwellMs() const {
    // How long the sweeping cursor rests on each chip. Only the one-button
    // boards live or die by this; a touch board picks its chip directly now.
    // Still slower than it was (640 down to 400 per packet), because the sweep
    // is also what a touch player sees while deciding.
    const int32_t v = 800 - (int32_t)(level() - 1) * 60;
    return (uint32_t)(v < 500 ? 500 : v);
}

// ── Geometry ──────────────────────────────────────────────────────────────
//
// Top to bottom inside the play rectangle: the deadline bar, the three-line
// packet card, then the two chips. Every extent is a fraction of the play
// rectangle, so the proportions are identical on all three panel families and
// nothing here knows a panel dimension.

void FirewallFrenzy::computeGeometry() {
    const games::Rect p = games::playArea();
    const int gap = games::panel() == games::PANEL_COMPACT ? 3 : 5;

    const int barH  = games::clampInt(p.h / 16, games::scaled(2), games::scaled(5));
    const int chipH = games::clampInt(p.h / 3, games::scaled(12), games::scaled(34));

    _clock.layout(p.x + 2, p.y, p.w - 4, barH);

    _card = games::Rect(p.x, p.y + barH + 2, p.w,
                        p.h - chipH - gap - barH - 2);
    if (_card.h < 12) _card.h = 12;

    const games::Rect chipArea(p.x, p.y + p.h - chipH, p.w, chipH);
    _chips.layout(chipArea, 2, 5);              // "ALLOW" / "BLOCK"
    _chips.setLabel(0, CHIP_LABEL[CH_ALLOW], COL_ALLOW);
    _chips.setLabel(1, CHIP_LABEL[CH_BLOCK], COL_BLOCK);

    // Largest size at which the longest field still fits the card width and
    // three lines still fit its height. Capped at 2 so the big panel matches
    // the rest of the firmware rather than becoming a billboard.
    // realTextSize(1) / lineHeight(1) in the divisors because _card is in REAL
    // pixels while _cardTextSize is 240-relative: see ChipRow::layout(). The
    // clamp to 2 absorbs the error at all four panels today, so this picks the
    // same size everywhere and no board's image moves. Corrected regardless -
    // drawCardLines() clamps x to the card's left edge but does not clip its
    // right, and clearCard() only ever clears _card.w, so a size chosen one
    // step too large would overrun the card and leave the overrun uncleared.
    const int byW = _card.w / (CARD_COLS * 6 * games::realTextSize(1));
    const int byH = (_card.h / 3) / games::lineHeight(1);
    _cardTextSize = games::clampInt(byW < byH ? byW : byH, 1, 2);
    _cardLineH    = _card.h / 3;
}

// ── Deck ──────────────────────────────────────────────────────────────────

void FirewallFrenzy::shuffleRange(int lo, int hi) {
    for (int i = hi - 1; i > lo; i--) {
        const int j = lo + _rng.range(i - lo + 1);
        const uint8_t t = _deck[i];
        _deck[i] = _deck[j];
        _deck[j] = t;
    }
}

// Obvious entries first, ambiguous after, each partition shuffled. Built from
// the `hard` flag rather than from the table's order, so reordering the table
// above can never silently break the difficulty ramp.
void FirewallFrenzy::buildDeck() {
    int n = 0;
    for (int i = 0; i < PACKET_N; i++) {
        if (!PACKETS[i].hard) _deck[n++] = (uint8_t)i;
    }
    _easyCut = (uint16_t)n;
    for (int i = 0; i < PACKET_N; i++) {
        if (PACKETS[i].hard) _deck[n++] = (uint8_t)i;
    }

    shuffleRange(0, _easyCut);
    shuffleRange(_easyCut, PACKET_N);
    _easyNext = 0;
    _hardNext = _easyCut;
}

uint8_t FirewallFrenzy::drawFromDeck() {
    // Ambiguous share climbs across the round, so the first packets teach and
    // the last ones test.
    int hardPct = 25 + (int)_judged * 2;
    if (hardPct > 70) hardPct = 70;

    bool hard = (_easyCut < (uint16_t)PACKET_N) && (_rng.range(100) < hardPct);
    if (_easyCut == 0) hard = true;
    if (_easyCut >= (uint16_t)PACKET_N) hard = false;

    const int lo = hard ? (int)_easyCut : 0;
    const int hi = hard ? PACKET_N : (int)_easyCut;

    uint16_t& cur = hard ? _hardNext : _easyNext;
    if ((int)cur < lo || (int)cur >= hi) {
        // Partition exhausted: reshuffle it so the repeat order differs. This
        // is an in-place swap loop, not an allocation.
        shuffleRange(lo, hi);
        cur = (uint16_t)lo;
    }
    return _deck[cur++];
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void FirewallFrenzy::begin(TFT_eSPI* tft) {
    _tft = tft;

    if (!_deck) {
        _deck = new uint8_t[PACKET_N];
    }
    for (int i = 0; i < PACKET_N; i++) _deck[i] = 0;

    computeGeometry();

    _rng.seed(millis() * 1103515245u + 0x12345677u);
    buildDeck();

    _score     = 0;
    _judged    = 0;
    _correct   = 0;
    _current   = 0;
    _lives     = START_LIVES;
    _completed = false;
    _ended     = false;
    _pressPending = false;
    _pressedChip  = 0;
    _phase     = PH_INTRO;
    _startMs   = millis();
    _lastMs    = _startMs;
    _phaseMs   = _startMs;
    _sweepMs   = _startMs;
    _result    = MinigameResult();

    _chips.forget();
    _clock.forget();

    if (!_tft) return;

    // No clearPlay() here: chromeInit() blanks the whole band between the two
    // rules, which is a superset of the play area, and a clearPlay() first only
    // fills part of it twice.
    //
    // The round panel's title row is a 128px chord, so anything longer than
    // this would run under the bezel. Folded at compile time.
    games::chromeInit(*_tft,
                      games::panel() == games::PANEL_ROUND ? "FIREWALL"
                                                           : "FIREWALL FRENZY",
                      COL_TITLE);
    _chips.drawAll(*_tft, games::CHIP_IDLE);
    drawCardLines("ALLOW OR BLOCK", "READ ALL 3", nullptr, TFT_YELLOW);
    drawStatus("INSPECT TRAFFIC", TFT_CYAN);
}

void FirewallFrenzy::end() {
    if (_deck) {
        delete[] _deck;
        _deck = nullptr;
    }
    // Latched rather than recomputed: the abandon path can reach end() twice,
    // and a second pass must not stretch durationMs or restate the result.
    if (!_ended) {
        _ended = true;
        _result.score      = _score;
        _result.durationMs = games::clampDuration(millis() - _startMs);
        _result.completed  = _completed;
        _result.newBest    = false;   // owned by the caller's high-score store
    }
    _tft = nullptr;
}

void FirewallFrenzy::onPress() {
    // Latch WHICH chip was under the cursor at the instant of the press, not
    // whichever chip the sweep has reached by the time update() runs.
    _pressedChip  = (uint8_t)_chips.cursor();
    _pressPending = true;
}

#if HEXHOUND_HAS_TOUCH
void FirewallFrenzy::onPressAt(int x, int y) {
    // A finger already knows which call it is making, so take the chip it
    // landed on rather than the one the sweep happens to be sitting on. The
    // sweep stays for the one-button boards and for a tap that misses.
    const int hit = _chips.chipAtXY(x, y);
    if (hit >= 0) {
        _pressedChip = (uint8_t)hit;
        // Move the visible cursor too, or the chip that lights up as chosen is
        // not the chip that was touched, which is its own kind of lie.
        if (_tft) _chips.moveCursor(*_tft, hit);
    } else {
        _pressedChip = (uint8_t)_chips.cursor();
    }
    _pressPending = true;
}
#endif

// ── Drawing ───────────────────────────────────────────────────────────────

void FirewallFrenzy::clearCard() {
    if (!_tft) return;
    _tft->fillRect(_card.x, _card.y, _card.w, _card.h, TFT_BLACK);
}

// Up to three centered lines inside the card. Called once per packet, never
// per frame, so clearing the card first is a packet arriving and not a strobe.
void FirewallFrenzy::drawCardLines(const char* a, const char* b, const char* c,
                                   uint16_t color) {
    if (!_tft) return;
    clearCard();

    const char* line[3] = { a, b, c };
    int n = 0;
    for (int i = 0; i < 3; i++) if (line[i]) n++;
    if (n == 0) return;

    int y = _card.y + (_card.h - n * _cardLineH) / 2;
    if (y < _card.y) y = _card.y;

    const int textOff = (_cardLineH - games::lineHeight(_cardTextSize)) / 2;
    _tft->setTextSize(games::realTextSize(_cardTextSize));
    _tft->setTextColor(color, TFT_BLACK);

    for (int i = 0; i < 3; i++) {
        if (!line[i]) continue;
        const int w = games::textWidth(line[i], _cardTextSize);
        int x = _card.x + (_card.w - w) / 2;
        if (x < _card.x) x = _card.x;
        _tft->setCursor(x, y + (textOff > 0 ? textOff : 0));
        _tft->print(line[i]);
        y += _cardLineH;
    }
}

void FirewallFrenzy::drawStatus(const char* text, uint16_t color) {
    if (!_tft) return;
    games::status(*_tft, text, color);
}

void FirewallFrenzy::drawOver() {
    if (!_tft) return;
    games::clearPlay(*_tft);
    _chips.forget();
    _clock.forget();

    const games::Rect p = games::playArea();
    const int size = games::bodyTextSize();
    const int lineH = games::linePitch(size);
    int y = p.y + (p.h - lineH * 2) / 2;
    if (y < p.y) y = p.y;

    games::centerText(*_tft, y, _lives > 0 ? "SHIFT OVER" : "BREACHED",
                      size, _lives > 0 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    y += lineH;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu PTS  %d/%d",
             (unsigned long)_score, (int)_correct, (int)_judged);
    games::centerText(*_tft, y, buf, size, TFT_WHITE, TFT_BLACK);
}

// ── Round flow ────────────────────────────────────────────────────────────

void FirewallFrenzy::showPacket(uint32_t nowMs) {
    _current = drawFromDeck();
    const PacketDef& d = PACKETS[_current];

    if (_tft) {
        drawCardLines(d.proto, d.origin, d.note, TFT_WHITE);
        _chips.drawAll(*_tft, games::CHIP_IDLE);
        _chips.moveCursor(*_tft, 0);

        char buf[40];
        // The level is shown because it is the thing that changes the game.
        // Ramping the deadline invisibly per packet meant the difficulty was
        // real and unannounced; a player who is given less time deserves to be
        // told, both so a bad round has an explanation and so surviving a
        // faster level feels like the achievement it is.
        snprintf(buf, sizeof(buf), "L%u  %d/%d  SCR %lu  LIFE %d",
                 (unsigned)level(), (int)_judged + 1, PACKET_LIMIT,
                 (unsigned long)_score, _lives < 0 ? 0 : (int)_lives);
        drawStatus(buf, _lives > 1 ? TFT_GREEN : TFT_ORANGE);
    }

    _phase   = PH_DECIDE;
    _phaseMs = nowMs;
    _sweepMs = nowMs;
}

void FirewallFrenzy::judge(uint8_t choice, uint32_t nowMs) {
    const PacketDef& d = PACKETS[_current];
    const uint8_t rightChip = d.block ? (uint8_t)CH_BLOCK : (uint8_t)CH_ALLOW;
    const bool timedOut = (choice > CH_BLOCK);
    const bool right    = !timedOut && (choice == rightChip);

    if (_tft) _clock.clear(*_tft);

    if (right) {
        _correct++;
        _score += d.hard ? 20u : 10u;
        if (_tft) {
            _chips.draw(*_tft, choice, games::CHIP_GOOD);
            drawStatus(d.block ? "DROPPED" : "PASSED", TFT_GREEN);
        }
    } else {
        _lives--;
        if (_tft) {
            // Show the call that was right as well as the one that was made:
            // a rule you are never shown is one you cannot learn.
            if (!timedOut) _chips.draw(*_tft, choice, games::CHIP_BAD);
            _chips.draw(*_tft, rightChip, games::CHIP_GOOD);
            // And WHY. Showing the correct chip says what the answer was;
            // this says what the rule is, which is the only part worth
            // carrying to the next packet.
            drawCardLines(d.proto, rightChip == CH_BLOCK ? "-> BLOCK" : "-> ALLOW",
                          d.why, TFT_YELLOW);
            drawStatus(timedOut ? "LEAKED"
                                : (d.block ? "BREACH" : "OVERBLOCK"),
                       TFT_RED);
        }
    }

    _judged++;
    _phase   = PH_VERDICT;
    _phaseMs = nowMs;
}

// ── Main update ───────────────────────────────────────────────────────────

bool FirewallFrenzy::update(uint32_t nowMs) {
    if (!_deck) return false;

    if (nowMs - _lastMs < FRAME_MS) return true;
    _lastMs = nowMs;

    switch (_phase) {
    case PH_INTRO:
        _pressPending = false;
        if (nowMs - _phaseMs >= INTRO_MS) {
            showPacket(nowMs);
        }
        return true;

    case PH_DECIDE: {
        if (_pressPending) {
            _pressPending = false;
            judge(_pressedChip <= CH_BLOCK ? _pressedChip : (uint8_t)CH_ALLOW,
                  nowMs);
            return true;
        }

        const uint32_t elapsed = nowMs - _phaseMs;
        const uint32_t limit   = deadlineMs();
        if (elapsed >= limit) {
            // An unexamined packet is one that got through. Costs a life, and
            // it is what lets an abandoned round finish on its own.
            judge(0xFF, nowMs);
            return true;
        }

        if (_tft) {
            const uint32_t left = limit - elapsed;
            const uint16_t col = (left * 2 > limit) ? TFT_GREEN
                               : (left * 4 > limit) ? TFT_ORANGE : TFT_RED;
            _clock.draw(*_tft, elapsed, limit, col);
        }

        if (nowMs - _sweepMs >= dwellMs()) {
            _sweepMs = nowMs;
            if (_tft) _chips.step(*_tft);
        }
        return true;
    }

    case PH_VERDICT:
        _pressPending = false;
        if (nowMs - _phaseMs >= VERDICT_MS) {
            if (_lives <= 0 || _judged >= PACKET_LIMIT) {
                _completed = true;      // played out, not abandoned
                _result.score      = _score;
                _result.durationMs = games::clampDuration(nowMs - _startMs);
                _result.completed  = true;
                drawOver();
                _phase   = PH_OVER;
                _phaseMs = nowMs;
            } else {
                showPacket(nowMs);
            }
        }
        return true;

    case PH_OVER:
    default:
        if (nowMs - _phaseMs >= OVER_HOLD_MS) {
            if (_pressPending) {
                _pressPending = false;
                return false;
            }
            // Auto-close after a generous hold so an unattended device does
            // not sit on a dead screen forever.
            if (nowMs - _phaseMs >= OVER_HOLD_MS * 4) return false;
        } else {
            _pressPending = false;   // swallow the press that ended the round
        }
        return true;
    }
}
