#pragma once

#include "../content/minigame.h"
#include "game_layout.h"
#include "game_widgets.h"

// ── HexHound - Cipher Sprint ────────────────────────────────────
//
// Eight tiny puzzles against the clock. Each one shows a worked example of a
// rule and then asks you to apply it once:
//
//     NET->OFU          the rule, demonstrated
//     KEY->???          the question
//     [LFZ] [MGA] [YEK] three answers, one of them right
//
//   short press  commit whichever answer the cursor is on
//   long press   exit (handled by the caller, which calls end())
//
// One button again, so the answer is chosen by sweep: the cursor walks the
// three chips and a press takes the one it is on. Three families of rule ship
// today - a Caesar shift, a reversal, and an arithmetic sequence - and the
// example always identifies exactly one of them. That is checked when the
// puzzle is generated, not assumed: a puzzle whose example fits two rules has
// no correct answer, and the player would be right to call it broken.
//
// Deliberately short. This is a two-minute loop on a 160x80 panel, so a puzzle
// is three letters wide and readable in a glance, not a crossword.
//
// Three lives, eight puzzles. A wrong answer or an expired deadline costs one.
// Score is 100 per solve plus up to 60 for speed, so answering fast is worth
// something but guessing fast is not.

class CipherSprint : public Minigame {
public:
    static CipherSprint& instance();

    const char* id() const override { return "cipher_sprint"; }
    const char* name() const override { return "CIPHER"; }
    uint16_t requiredCaps() const override { return 0; }   // any board

    void begin(TFT_eSPI* tft) override;
    bool update(uint32_t nowMs) override;
    void onPress() override;
#if HEXHOUND_HAS_TOUCH
    // A tap picks the answer it landed on. See ChipRow::chipAtXY().
    void onPressAt(int x, int y) override;
#endif
    void end() override;
    MinigameResult result() const override { return _result; }

private:
    CipherSprint() = default;

    enum Phase : uint8_t {
        PH_INTRO = 0,
        PH_SOLVE,       // cursor sweeping, deadline draining
        PH_MARK,        // the answer is shown before the next puzzle
        PH_OVER
    };

    static const int OPTIONS      = 3;
    static const int PUZZLE_LIMIT = 8;
    static const int START_LIVES  = 3;
    static const int LINE_LEN     = 16;   // characters per card line, with NUL
    static const int OPT_LEN      = 6;    // characters per answer, with NUL

    // One puzzle, allocated in begin() and freed in end(). The chips hold
    // pointers into opt[], so this must outlive every draw in the round.
    struct Puzzle {
        char    line1[LINE_LEN];
        char    line2[LINE_LEN];
        char    opt[OPTIONS][OPT_LEN];
        uint8_t answer;
        uint8_t family;
    };

    void computeGeometry();
    void makePuzzle();
    void showPuzzle(uint32_t nowMs);
    void mark(uint8_t choice, uint32_t nowMs);
    void drawCardLines(const char* a, const char* b, uint16_t color);
    void clearCard();
    void drawStatus(const char* text, uint16_t color);
    void drawOver();
    uint32_t deadlineMs() const;
    uint32_t dwellMs() const;

    TFT_eSPI* _tft = nullptr;
    Puzzle*   _pz  = nullptr;       // owned between begin() and end()

    games::Rect     _card;
    games::ChipRow  _chips;
    games::DrainBar _clock;
    int _cardTextSize = 1;
    int _cardLineH    = 8;

    games::Rng _rng;
    uint32_t _startMs  = 0;
    uint32_t _lastMs   = 0;
    uint32_t _phaseMs  = 0;
    uint32_t _sweepMs  = 0;
    uint32_t _score    = 0;
    uint8_t  _index    = 0;         // puzzles presented so far
    uint8_t  _solved   = 0;
    uint8_t  _pressedChip = 0;
    uint8_t  _phase    = PH_INTRO;
    int8_t   _lives    = 0;
    bool     _pressPending = false;
    bool     _completed = false;
    bool     _ended     = false;

    MinigameResult _result;
};
