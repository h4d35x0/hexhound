#pragma once

#include "../content/minigame.h"
#include "game_layout.h"

// ── HexHound - Signal Memory ────────────────────────────────────
//
// Simon, rebuilt for a device with exactly one button.
//
// The device plays back a growing sequence of four protocol tiles. The hard
// part of porting Simon to one button is selection, not memory: you cannot
// point at a tile. So input is a sweep. A cursor walks the four tiles at a
// fixed cadence and the player presses when it is sitting on the one they
// want. That turns "which tile" into "when", which one button can express,
// and it adds a timing layer that makes later levels tense without making
// them unfair - the cadence is always visible as a draining bar under the
// cursor.
//
//   short press  select the tile the cursor is on right now
//   long press   exit (handled by the caller, which calls end())
//
// One wrong tile ends the round. So does letting the signal go quiet for too
// long. Score is the longest sequence reproduced correctly.

class SignalMemory : public Minigame {
public:
    static SignalMemory& instance();

    const char* id() const override { return "signal_memory"; }
    const char* name() const override { return "SIG MEMORY"; }
    uint16_t requiredCaps() const override { return 0; }   // any board

    void begin(TFT_eSPI* tft) override;
    bool update(uint32_t nowMs) override;
    void onPress() override;
    void end() override;
    MinigameResult result() const override { return _result; }

private:
    SignalMemory() = default;

    enum Phase : uint8_t {
        PH_INTRO = 0,
        PH_SHOW,        // a tile is lit
        PH_SHOW_GAP,    // dark beat between tiles
        PH_INPUT,       // cursor sweeping, waiting for a press
        PH_GOOD,        // whole sequence reproduced
        PH_BAD,         // wrong tile, or the signal went quiet
        PH_OVER
    };

    enum TileState : uint8_t {
        TS_IDLE = 0,
        TS_LIT,
        TS_CURSOR,
        TS_WRONG,
        TS_UNDRAWN = 0xFF
    };

    static const int TILES   = 4;
    static const int MAX_SEQ = 32;   // 32 correct in a row is a finished round

    void computeGeometry();
    void appendSymbol();
    void drawTile(int i, uint8_t state);
    void setTile(int i, uint8_t state);
    void drawAllTiles(uint8_t state);
    void drawCursorBar(uint32_t nowMs);
    void clearCursorBar();
    void drawStatus(const char* what, uint16_t color);
    void drawOver();
    uint32_t litMs()   const;
    uint32_t gapMs()   const;
    uint32_t dwellMs() const;

    TFT_eSPI* _tft = nullptr;
    uint8_t*  _seq = nullptr;        // MAX_SEQ entries, owned between begin/end

    games::Rect _tiles[TILES];
    uint8_t     _tileState[TILES] = { TS_UNDRAWN, TS_UNDRAWN,
                                      TS_UNDRAWN, TS_UNDRAWN };
    int _labelSize = 1;
    int _barH      = 2;
    int _barW      = -1;

    games::Rng _rng;
    uint32_t _startMs  = 0;
    uint32_t _lastMs   = 0;
    uint32_t _phaseMs  = 0;      // start of the current beat (or cursor step)
    uint32_t _inputMs  = 0;      // start of the current input attempt
    uint32_t _score    = 0;
    uint8_t  _len      = 0;          // current sequence length
    uint8_t  _showIdx  = 0;
    uint8_t  _inputIdx = 0;
    uint8_t  _cursor   = 0;
    uint8_t  _pressedTile = 0;
    uint8_t  _phase    = PH_INTRO;
    bool     _pressPending = false;
    bool     _timedOut = false;
    bool     _completed = false;

    MinigameResult _result;
};
