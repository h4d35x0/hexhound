#pragma once

#include "../content/minigame.h"
#include "game_layout.h"

// ── HexHound - Packet Chase ─────────────────────────────────────
//
// A one-button endless runner. The pet runs left to right along the wire while
// traffic comes at it. Two lanes: on the wire and above it.
//
//   short press  hop
//   long press   exit (handled by the caller, which calls end())
//
// The single button is genuinely a decision rather than a reflex, because
// hopping is not always right:
//
//   malformed packet ON the wire    hop over it
//   malformed packet ABOVE the wire stay down, do not hop
//   valid packet on the wire        stay down to collect
//   valid packet above the wire     hop to collect
//
// So every spawn asks "up or down?" and the answer changes. That is the whole
// game, and it needs exactly one button.
//
// Three lives. Score is distance travelled (counted in body-widths so it means
// the same thing on a 160x80 panel and on a 320x172 one) plus five per valid
// packet collected.

class PacketChase : public Minigame {
public:
    static PacketChase& instance();

    const char* id() const override { return "packet_chase"; }
    const char* name() const override { return "PKT CHASE"; }
    uint16_t requiredCaps() const override { return 0; }   // any board

    void begin(TFT_eSPI* tft) override;
    bool update(uint32_t nowMs) override;
    void onPress() override;
    void end() override;
    MinigameResult result() const override { return _result; }

private:
    PacketChase() = default;

    enum Phase : uint8_t {
        PH_READY = 0,   // "GET READY" beat before the first spawn
        PH_RUN,
        PH_OVER         // score held on screen until the player presses
    };

    enum EntKind : uint8_t {
        ENT_FREE = 0,
        ENT_HAZARD,     // malformed packet: costs a life
        ENT_PACKET      // valid packet: worth points
    };

    // 8 bytes. The whole pool is 10 of these, allocated once in begin().
    struct Ent {
        int16_t x     = 0;      // left edge this frame
        int16_t drawX = 0;      // left edge as last drawn, for erase-in-place
        uint8_t kind  = ENT_FREE;
        uint8_t lane  = 0;      // 0 = on the wire, 1 = above it
        uint8_t drawn = 0;
        uint8_t pad   = 0;
    };

    static const int MAX_ENT = 10;

    void computeGeometry();
    void drawFrame();
    void drawEntity(const Ent& e, int atX, bool erase);
    void drawPet(int atY, bool erase);
    void drawStatus();
    void drawOver();
    void spawn();
    void stepWorld(uint32_t dtMs);
    int  laneTop(uint8_t lane) const;
    int  petTop() const;         // current pet top edge, from the hop arc
    bool petAirborne() const;

    TFT_eSPI* _tft = nullptr;
    Ent*      _ents = nullptr;   // MAX_ENT entries, owned between begin/end

    // Geometry, all derived from games::playArea() in begin().
    int _left = 0, _right = 0, _groundY = 0, _cell = 0, _hopH = 0, _petX = 0;

    // World state.
    games::Rng _rng;
    float    _dist      = 0.0f;   // pixels travelled this round
    float    _speed     = 0.0f;   // pixels per second
    float    _speedBase = 0.0f;
    float    _hopPhase  = 0.0f;   // 0..1 through the hop arc, 0 when grounded
    uint32_t _hopStart  = 0;
    uint32_t _startMs   = 0;
    uint32_t _lastMs    = 0;
    uint32_t _phaseMs   = 0;
    uint32_t _packets   = 0;
    uint32_t _score     = 0;
    uint32_t _shownScore = 0xFFFFFFFFu;
    int      _lives     = 0;
    int      _shownLives = -1;
    int      _petDrawY  = -1;
    uint8_t  _phase     = PH_READY;
    bool     _hopping   = false;
    bool     _pressPending = false;
    bool     _completed = false;

    MinigameResult _result;
};
