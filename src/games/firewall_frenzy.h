#pragma once

#include "../content/minigame.h"
#include "game_layout.h"
#include "game_widgets.h"

// ── HexHound - Firewall Frenzy ──────────────────────────────────
//
// You are the rule engine. Packets arrive one at a time, each described by
// three lines, and you either let it through or drop it.
//
//   short press  commit whichever of ALLOW / BLOCK the cursor is on
//   long press   exit (handled by the caller, which calls end())
//
// Selection is the same sweep signal_memory uses: the cursor alternates
// between the two chips and a press commits the one it is sitting on. That is
// the only way one button can express a choice, and it costs at most one dwell
// of waiting, which the deadline is sized around.
//
// It is a decision, not a reflex test. Every packet shows protocol, origin and
// one observation, and the verdict lives in the combination:
//
//   TCP 22 SSH  / SRC UNKNOWN / KEY MATCHED   allow: the key is the identity
//   TCP 22 SSH  / SRC UNKNOWN / KEY CHANGED   block: that is the whole attack
//   TCP 3389 RDP/ SRC VPN     / USER ADMIN    allow: admin over the VPN is work
//   TCP 3389 RDP/ SRC WAN     / USER ADMIN    block: admin off the WAN is not
//
// The deck opens on packets whose answer is obvious from one line and shifts
// towards ones that need all three as the round goes on, so a new player
// learns the rules before being asked to apply them.
//
// Three lives. A wrong call costs one; so does letting the deadline expire,
// because an unexamined packet is one that got through. Score is 10 per
// correct call and 20 for a correct call on an ambiguous packet. The round
// ends when the lives are gone or the deck limit is reached.

class FirewallFrenzy : public Minigame {
public:
    static FirewallFrenzy& instance();

    const char* id() const override { return "firewall_frenzy"; }
    const char* name() const override { return "FIREWALL"; }
    uint16_t requiredCaps() const override { return 0; }   // any board

    void begin(TFT_eSPI* tft) override;
    bool update(uint32_t nowMs) override;
    void onPress() override;
#if HEXHOUND_HAS_TOUCH
    // A tap picks the chip it landed on. See ChipRow::chipAtXY().
    void onPressAt(int x, int y) override;
#endif
    void end() override;
    MinigameResult result() const override { return _result; }

private:
    FirewallFrenzy() = default;

    enum Phase : uint8_t {
        PH_INTRO = 0,
        PH_DECIDE,      // cursor sweeping, deadline draining
        PH_VERDICT,     // the call is shown before the next packet arrives
        PH_OVER
    };

    enum Choice : uint8_t {
        CH_ALLOW = 0,
        CH_BLOCK = 1
    };

    // How many packets a full round is. At roughly 2.5 s each this is a
    // two-minute session, which is the loop these games are built for.
    static const int PACKET_LIMIT = 30;
    static const int START_LIVES  = 3;

    void computeGeometry();
    void buildDeck();
    void shuffleRange(int lo, int hi);
    uint8_t drawFromDeck();
    void showPacket(uint32_t nowMs);
    void judge(uint8_t choice, uint32_t nowMs);
    void drawCardLines(const char* a, const char* b, const char* c,
                       uint16_t color);
    void clearCard();
    void drawStatus(const char* text, uint16_t color);
    void drawOver();
    uint32_t deadlineMs() const;
    // 1-based, rises every PACKETS_PER_LEVEL judged packets.
    uint8_t  level() const;
    uint32_t dwellMs() const;

    TFT_eSPI* _tft  = nullptr;
    uint8_t*  _deck = nullptr;      // shuffled table indices, owned begin/end

    games::Rect     _card;
    games::ChipRow  _chips;
    games::DrainBar _clock;
    int _cardTextSize = 1;
    int _cardLineH    = 8;

    games::Rng _rng;
    uint32_t _startMs   = 0;
    uint32_t _lastMs    = 0;
    uint32_t _phaseMs   = 0;
    uint32_t _sweepMs   = 0;        // start of the current cursor dwell
    uint32_t _score     = 0;
    uint16_t _easyCut   = 0;        // deck index where ambiguous packets begin
    uint16_t _easyNext  = 0;        // cursor into the obvious partition
    uint16_t _hardNext  = 0;        // cursor into the ambiguous partition
    uint8_t  _current   = 0;        // table index of the packet on screen
    uint8_t  _judged    = 0;        // packets resolved this round
    uint8_t  _correct   = 0;
    uint8_t  _pressedChip = 0;
    uint8_t  _phase     = PH_INTRO;
    int8_t   _lives     = 0;
    bool     _pressPending = false;
    bool     _completed = false;
    bool     _ended     = false;

    MinigameResult _result;
};
