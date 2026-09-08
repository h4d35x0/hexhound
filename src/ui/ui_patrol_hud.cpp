#include "ui_patrol_hud.h"
#include "ui_round.h"
#include "ui_utils.h"
#include "ui_worn.h"
#include "../pet/pet_core.h"
#include "../modules/storage_module.h"
#include "../modules/touch_module.h"
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// ── HexHound - Live Patrol HUD Implementation ──────────────────

// ── Color constants ──────────────────────────────────────────────────────
#define COL_BG         TFT_BLACK
#define COL_HEADER_BG  0x0841   // near-black
#define COL_HEADER_TXT 0x07FF   // cyan
#define COL_RING       0x035F   // electric blue ring outline
#define COL_RING_BLE   0xFD20   // amber ring during BLE phase
#define COL_SWEEP      0x07FF   // cyan sweep head
#define COL_DOT_WIFI   0x07E0   // green - normal WiFi
#define COL_DOT_OPEN   0xFFE0   // yellow - open network
#define COL_DOT_DUPE   0xF800   // red - duplicate SSID
#define COL_DOT_BLE    0xFD20   // amber - BLE device
#define COL_DOT_TRACK  0xF81F   // magenta - tracker candidate
#define COL_COUNTER    TFT_WHITE
#define COL_FLASH      0xFFE0   // yellow counter flash
#define COL_BAR_BG     0x2104   // dark gray bar background
#define COL_BAR_WIFI   0x07FF   // cyan progress bar
#define COL_BAR_BLE    0xFD20   // amber progress bar
#define COL_BAR_DONE   0x07E0   // green completed bar
#define COL_LABEL      0xBDF7   // bright gray label text
#define COL_SCAN_PULSE 0x07FF   // cyan "SCAN" text
#define COL_PHASE_BLE  0x035F   // electric blue "BLE" phase text

// Sweep speed: degrees per second
#define SWEEP_SPEED    180.0f
// Trail segments behind the sweep head
#define TRAIL_SEGMENTS 6
#define TRAIL_SPREAD   8.0f  // degrees between trail lines

// ── Singleton ────────────────────────────────────────────────────────────

UIPatrolHUD& UIPatrolHUD::instance() {
    static UIPatrolHUD hud;
    return hud;
}

void UIPatrolHUD::init(TFT_eSPI* tft) {
    _tft = tft;
}

int UIPatrolHUD::hudWidth() const {
    return _tft ? _tft->width() : ((SCREEN_W < SCREEN_H) ? SCREEN_W : SCREEN_H);
}

int UIPatrolHUD::hudHeight() const {
    return _tft ? _tft->height() : ((SCREEN_W > SCREEN_H) ? SCREEN_W : SCREEN_H);
}

// Text size for the HUD's own strings.
//
// Was written inline at five sites as `isBig() ? 2 : 1`, which answers 2 for
// BOTH round panels: correct at 240 and HALF SIZE at 480, because a raw
// setTextSize() bypasses the uiround helpers that would apply TEXT_SCALE.
//
// A macro rather than a member function, and guarded rather than branched at
// runtime, for the reason ui_menu.h already records: a helper referenced from
// several sites leaves GCC an out-of-line copy, and the non-round boards' images
// grew 16 bytes when this was a method. On a rectangular board this expands to
// the ORIGINAL expression, so their code is textually unchanged.
#if HEXHOUND_PANEL_ROUND
#define HUD_TS  (uiround::ts(2))
#else
#define HUD_TS  (isBig() ? 2 : 1)
#endif

bool UIPatrolHUD::isBig() const {
    return hudWidth() > 100;   // Waveshare portrait is 172 wide; T-Dongle is 80
}

int UIPatrolHUD::headerHeight() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(40);
#else
    return isBig() ? 26 : 12;
#endif
}

int UIPatrolHUD::ringRadius() const {
#if HEXHOUND_PANEL_ROUND
    // Sized so the radar sits between the header band and the counter rows,
    // with the pet riding at its center.
    return uiround::scaled(54);
#else
    bool big = isBig();
    int wLimit = big ? (hudWidth() - 20) / 2 : (hudWidth() - 12) / 3;
    int hLimit = big ? (hudHeight() / 4)     : (hudHeight() / 5);
    int radius = min(wLimit, hLimit);
    int lo = big ? 40 : 18;
    int hi = big ? 80 : 42;
    if (radius < lo) radius = lo;
    if (radius > hi) radius = hi;
    return radius;
#endif
}

// Radius of the clear hub at the middle of the radar. ZERO: there is no hub.
//
// This existed on the theory that the HD stage art carried an opaque dark
// margin, because scripts/gen_hd_sprites.py only chroma-keys pixels under
// ALPHA_CUTOFF = 24 and composites everything softer over black. The theory was
// never measured. It is wrong.
//
// Measured, on the baked 176 px Packet Pup portrait actually compiled into this
// board (src/ui/sprites_hd_round480.h):
//
//     30976 pixels, 25658 of them the 0xF81F chroma key   (82.8%)
//     all four corners chroma
//     126 opaque pixels below luminance 8, and they hug the silhouette
//
// The sprite paints no box. drawSprite() skips every chroma pixel, so the radar
// is free to run underneath it - which is what "the background needs to be
// transparent" was asking for, and it already is.
//
// What actually drew the box is one line in drawPetCorner(): a fillRect of
// COL_BG over the pet's whole bounding square, AFTER the crosshair and the
// sweep had been drawn through that square. A hard-edged black rectangle
// stamped over the radar, every frame. Carving a hub out of the lines only
// stopped them entering the rectangle - so the box became a void instead, which
// is the same thing to look at and is what got reported the second time.
//
// The fillRect is gone on round panels (see drawPetCorner) and the lines run to
// the centre again. Kept as a function returning 0 rather than deleted so the
// spoke geometry below still reads as deliberate.
int UIPatrolHUD::hubRadius() const {
    return 0;
}

int UIPatrolHUD::ringCX() const {
    return hudWidth() / 2;
}

int UIPatrolHUD::ringCY() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(100);
#else
    return headerHeight() + ringRadius() + 10;
#endif
}

int UIPatrolHUD::counterY() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(166);
#else
    return ringCY() + ringRadius() + (isBig() ? 18 : 10);
#endif
}

// Vertical gap between the two counter rows (WiFi/Open, BLE/Trk).
int UIPatrolHUD::counterRowGap() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(18);
#else
    return isBig() ? 22 : 12;
#endif
}

int UIPatrolHUD::leftCounterX() const {
#if HEXHOUND_PANEL_ROUND
    // Pulled well inside the rim: x=8 is behind the bezel at the counter rows,
    // where the chord runs 32..208.
    return uiround::scaled(36);
#else
    return isBig() ? 8 : 2;
#endif
}

int UIPatrolHUD::rightCounterX() const {
#if HEXHOUND_PANEL_ROUND
    // 122, not 128. The longest string in this column is "BLE LOCK", 8 glyphs,
    // which is 6*8*ts wide - 96 px at 240 and 192 at 480. Starting at 128 put
    // its last glyph at 224 (240) and 448 (480), while the bezel-safe chord at
    // the counter row ends at about 220 and 441. The K really was under the rim
    // on both panels; it is just far more visible on the 480.
    return uiround::scaled(122);
#else
    return hudWidth() / 2 + (isBig() ? 6 : 2);
#endif
}

int UIPatrolHUD::progressY() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(190);
#else
    return hudHeight() - (isBig() ? 44 : 22);
#endif
}

int UIPatrolHUD::barWidth() const {
#if HEXHOUND_PANEL_ROUND
    // Chord at the bar row (y=190) is 170px; 140 leaves a clean margin.
    return uiround::scaled(140);
#else
    bool big = isBig();
    int width = hudWidth() - (big ? 44 : 30);
    int lo = big ? 90 : 50;
    int hi = big ? 150 : 96;
    if (width < lo) width = lo;
    if (width > hi) width = hi;
    return width;
#endif
}

int UIPatrolHUD::barHeight() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(8);
#else
    return isBig() ? 10 : 5;
#endif
}

int UIPatrolHUD::barX() const {
#if HEXHOUND_PANEL_ROUND
    return (hudWidth() - barWidth()) / 2;
#else
    int x = (hudWidth() - barWidth()) / 2;
    int lo = isBig() ? 34 : 24;
    return x < lo ? lo : x;
#endif
}

#if !HEXHOUND_PANEL_ROUND
// Side of the HUD pet on a rectangular panel, now that it is the HD still
// rather than a whole-number blow-up of the pixel sprite.
//
// 24 on both families, for two independent reasons that happen to agree:
//
//   * The compact panel bakes its home portrait at exactly HD_HOME_SRC_PX = 24,
//     so this is a plain blit with no resampling anywhere in the path.
//   * The big panel bakes 64, but the corner the pet stands in is only as wide
//     as barX() (34) less its own left margin (6). 24 is what fits without
//     running into the progress bars, and it is still half again the 16 px the
//     pixel sprite was given there.
//
// Stage-independent, unlike the sprite size it replaces, which handed the
// sentinel a 20 px box and everything else 16 or 32. A footprint that cannot
// change is also what lets the clear in drawPetCorner() be exactly this box:
// it can never leave residue from a larger previous frame.
static constexpr int HUD_PET_PX = 24;
#endif

int UIPatrolHUD::petDrawWidth() const {
#if HEXHOUND_PANEL_ROUND
    // Three quarters of the baked HD art: 132 px at 480 and 66 at 240, both
    // integral, and the reduction drops one source pixel in four on a uniform
    // lattice rather than unevenly along one axis.
    //
    // It was HALF, which is a cleaner 2:1 reduction but put an 88 px hound in a
    // 216 px ring on a 480 panel and read, correctly, as too small: the radar is
    // the subject of this screen and the pet was a detail inside it. At 3/4 the
    // hound fills about 61% of the ring on both round panels, and the discovery
    // dots still sit clear of it - they are drawn ON the rim at r =
    // ringRadius(), and the ring itself is untouched, so nothing else on this
    // screen moved.
    return (HD_HOME_SRC_PX * 3) / 4;
#else
    return HUD_PET_PX;
#endif
}

int UIPatrolHUD::petDrawHeight() const {
#if HEXHOUND_PANEL_ROUND
    return (HD_HOME_SRC_PX * 3) / 4;
#else
    return HUD_PET_PX;
#endif
}

int UIPatrolHUD::petX() const {
#if HEXHOUND_PANEL_ROUND
    return ringCX() - petDrawWidth() / 2;
#else
    if (isBig()) {
        return 6;
    }
    return (hudWidth() - petDrawWidth()) / 2;
#endif
}

int UIPatrolHUD::petY() const {
#if HEXHOUND_PANEL_ROUND
    // The hound rides at the center of its own radar sweep. The big-panel
    // bottom-left corner slot (y = height-24) is behind the bezel here.
    return ringCY() - petDrawHeight() / 2;
#else
    if (isBig()) {
        // 32 rather than 24: the slot holds a 24 px HD portrait now instead of
        // a 16 px sprite, and this keeps the same 8 px of air beneath it.
        return hudHeight() - 32;
    }
    return progressY() - petDrawHeight() - 2;
#endif
}

int UIPatrolHUD::scanLabelX() const {
#if HEXHOUND_PANEL_ROUND
    return ringCX() - uiround::scaled(24);
#else
    return hudWidth() - (isBig() ? 56 : 28);
#endif
}

int UIPatrolHUD::scanLabelY() const {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(208);
#else
    return counterY() + (isBig() ? 46 : 30);
#endif
}

bool UIPatrolHUD::bleUnlocked() const {
    return _stage >= STAGE_BEACON_BEAST;
}

// ── Lifecycle ────────────────────────────────────────────────────────────

void UIPatrolHUD::begin(PetStage stage) {
    if (!_tft) return;

    _stage = stage;

    // Switch to portrait mode (80×160)
    uint8_t rotation = StorageModule::instance().getPortraitRotation();
    _tft->setRotation(rotation);
    TouchModule::instance().setDisplayRotation(rotation, _tft->width(), _tft->height());
    _tft->fillScreen(COL_BG);

    // Reset state
    _sweepAngle       = 0;
    _phase            = HUD_PHASE_WIFI;
    _wifiCount        = 0;
    _bleCount         = 0;
    _openCount        = 0;
    _trackCount       = 0;
    _lastDrawnWifi    = -1;
    _lastDrawnBle     = -1;
    _lastDrawnOpen    = -1;
    _lastDrawnTrack   = -1;
    _dotCount         = 0;
    _wifiProgress     = 0;
    _bleProgress      = 0;
    _completionPulses = 0;
    _lastPulseTime    = 0;
    _petAnimOverrideEnd = 0;
    _wifiFlashEnd     = 0;
    _bleFlashEnd      = 0;
    _openFlashEnd     = 0;
    _trackFlashEnd    = 0;
    _ringFlashEnd     = 0;
    _ringFlashColor   = 0;
    memset(_dots, 0, sizeof(_dots));

    uint32_t now = millis();
    _startTime      = now;
    _phaseStartTime = now;
    _lastFrameTime  = now;

    // Set pet to scan animation
    _petAnim.selectForStage(stage, ANIM_SCAN);

    // Draw static elements
    drawHeader();
    drawRadarFrame();
    drawCounters(true);
    drawProgressBars();
    drawPhaseLabel();
    drawPetCorner();

    Serial.println("[HUD] Patrol HUD started (portrait mode)");
}

void UIPatrolHUD::end() {
    if (!_tft) return;

    // Return to landscape mode (160×80)
    uint8_t rotation = StorageModule::instance().getLandscapeRotation();
    _tft->setRotation(rotation);
    TouchModule::instance().setDisplayRotation(rotation, _tft->width(), _tft->height());
    _tft->fillScreen(COL_BG);

    Serial.println("[HUD] Patrol HUD ended (landscape mode)");
}

// ── Main Update (called every loop iteration) ───────────────────────────

void UIPatrolHUD::update() {
    if (!_tft) return;

    uint32_t now = millis();

    // Target ~30fps - skip if too soon
    if (now - _lastFrameTime < 33) return;
    _lastFrameTime = now;

    // Handle completion phase separately
    if (_phase == HUD_PHASE_COMPLETE) {
        updateCompletion();
        return;
    }
    if (_phase == HUD_PHASE_DONE) return;

    // ── Update sweep angle (time-based, smooth) ──────────────────────
    float elapsed = (now - _phaseStartTime) / 1000.0f;
    _sweepAngle = fmod(elapsed * SWEEP_SPEED, 360.0f);

    // ── Update progress bars ─────────────────────────────────────────
    if (_phase == HUD_PHASE_WIFI) {
        // WiFi progress based on time (scan typically takes ~3-8 seconds)
        float wifiElapsed = (now - _phaseStartTime) / 1000.0f;
        _wifiProgress = min(wifiElapsed / 6.0f, 0.95f);  // cap at 95% until done
    } else if (_phase == HUD_PHASE_BLE) {
        _wifiProgress = 1.0f;
        float bleElapsed = (now - _phaseStartTime) / 1000.0f;
        _bleProgress = min(bleElapsed / (float)BLE_SCAN_DURATION_S, 0.95f);
    }

    // ── Clear and redraw radar interior ──────────────────────────────
    uint16_t ringColor = (_phase == HUD_PHASE_BLE) ? COL_RING_BLE : COL_RING;
    if (_ringFlashEnd > now) {
        ringColor = _ringFlashColor;
    }

    // Clear ring interior
    _tft->fillCircle(ringCX(), ringCY(), ringRadius() - 1, COL_BG);

    // Redraw ring outline
    _tft->drawCircle(ringCX(), ringCY(), ringRadius(), ringColor);
    _tft->drawCircle(ringCX(), ringCY(), ringRadius() + 1, dimColor(ringColor, 1));

    // Draw crosshairs (subtle grid inside ring). Shared with drawRadarFrame():
    // this used to be its own full-length pair here, so every frame after the
    // first painted a cross straight under the hound and the pet's opaque
    // margin then punched it back out as a black box. See hubRadius().
    drawCrosshairs(dimColor(ringColor, 1));

    // ── Draw sweep with fading trail ─────────────────────────────────
    drawSweep();

    // ── Draw all discovery dots ──────────────────────────────────────
    drawDots();

    // ── Center dot ───────────────────────────────────────────────────
    // Only where the centre is actually visible. On a round panel the hound is
    // standing on it.
    if (hubRadius() == 0) {
        drawSmallDot(*_tft, ringCX(), ringCY(), ringColor);
    }

    // ── Update counters (only if changed) ────────────────────────────
    drawCounters(false);

    // ── Update progress bars ─────────────────────────────────────────
    drawProgressBars();

    // ── Update pet animation ─────────────────────────────────────────
    // Check if pet animation override has expired
    if (_petAnimOverrideEnd > 0 && now > _petAnimOverrideEnd) {
        _petAnimOverrideEnd = 0;
        _petAnim.selectForStage(_stage, ANIM_SCAN);
    }
    _petAnim.update();
    drawPetCorner();

    // ── Pulse "SCAN" label ───────────────────────────────────────────
    bool scanVisible = ((now / 500) % 2 == 0);
    uint16_t phaseCol = (_phase == HUD_PHASE_BLE) ? COL_PHASE_BLE : COL_SCAN_PULSE;
#if HEXHOUND_PANEL_ROUND
    // 28x8 per unit of text size reproduces the old 56x16 at 240 and grows with
    // the panel; the rectangular literals below are untouched on purpose.
    _tft->fillRect(scanLabelX(), scanLabelY(), 28 * HUD_TS, 8 * HUD_TS, COL_BG);
#else
    _tft->fillRect(scanLabelX(), scanLabelY(), isBig() ? 56 : 28, isBig() ? 16 : 8,
                   COL_BG);
#endif
    _tft->setTextColor(scanVisible ? phaseCol : COL_BG, COL_BG);
    _tft->setTextSize(HUD_TS);
    _tft->setCursor(scanLabelX(), scanLabelY());
    if (_phase == HUD_PHASE_WIFI) {
        _tft->print("WIFI");
    } else {
        _tft->print("BLE");
    }
}

// ── Radar Sweep Drawing ──────────────────────────────────────────────────

void UIPatrolHUD::drawSweep() {
    uint16_t sweepColor = (_phase == HUD_PHASE_BLE) ? COL_RING_BLE : COL_SWEEP;

    // Every beam starts OUTSIDE the hub, so the sweep emanates from behind the
    // hound rather than from under it. Drawing it to the centre and then
    // covering the inner stub with an opaque sprite is what cut the trail off
    // against a hard square edge on the board; see hubRadius().
    const int hub = hubRadius();
    const int rim = ringRadius() - 2;
    if (rim <= hub) return;

    // Draw fading trail behind sweep head (oldest to newest)
    for (int i = TRAIL_SEGMENTS; i >= 1; i--) {
        float trailAngle = _sweepAngle - i * TRAIL_SPREAD;
        if (trailAngle < 0) trailAngle += 360.0f;
        int tx, ty, hx, hy;
        polarToXY(trailAngle, (float)rim, ringCX(), ringCY(), tx, ty);
        polarToXY(trailAngle, (float)hub, ringCX(), ringCY(), hx, hy);
        uint16_t trailCol = dimColor(sweepColor, (uint8_t)(TRAIL_SEGMENTS + 1 - i));
        _tft->drawLine(hx, hy, tx, ty, trailCol);
    }

    // Draw sweep head (brightest)
    int sx, sy, bx, by;
    polarToXY(_sweepAngle, (float)rim, ringCX(), ringCY(), sx, sy);
    polarToXY(_sweepAngle, (float)hub, ringCX(), ringCY(), bx, by);
    _tft->drawLine(bx, by, sx, sy, sweepColor);
}

// ── Discovery Dots ───────────────────────────────────────────────────────

void UIPatrolHUD::drawDots() {
    for (int i = 0; i < _dotCount; i++) {
        if (!_dots[i].active) continue;
        int dx, dy;
        // Place dots ON the ring outline
        polarToXY(_dots[i].angle, (float)ringRadius(), ringCX(), ringCY(), dx, dy);
        drawDot(*_tft, dx, dy, _dots[i].color);
    }
}

void UIPatrolHUD::addDot(float angle, uint16_t color) {
    if (_dotCount < MAX_HUD_DOTS) {
        _dots[_dotCount].angle  = angle;
        _dots[_dotCount].color  = color;
        _dots[_dotCount].active = true;
        _dotCount++;
    }
}

// ── Live Data Callbacks ──────────────────────────────────────────────────

void UIPatrolHUD::onWiFiNetwork(const char* ssid, bool isDuplicate,
                                 bool isOpen, int channel) {
    _wifiCount++;
    _wifiFlashEnd = millis() + 300;

    // Compute dot angle from channel (1-13 -> 0°-312°, spaced 24° apart)
    // Add hash offset for SSIDs on same channel
    float baseAngle = (channel - 1) * 24.0f;
    float offset = (float)(hashToRange(ssid, 20) - 10);  // ±10° jitter
    float angle = fmod(baseAngle + offset + 360.0f, 360.0f);

    if (isDuplicate) {
        addDot(angle, COL_DOT_DUPE);
        _ringFlashColor = COL_DOT_DUPE;
        _ringFlashEnd = millis() + 180;
    } else if (isOpen) {
        _openCount++;
        _openFlashEnd = millis() + 300;
        addDot(angle, COL_DOT_OPEN);
    } else {
        addDot(angle, COL_DOT_WIFI);
    }

    // Briefly trigger happy animation on pet
    _petAnim.selectForStage(_stage, ANIM_HAPPY);
    _petAnimOverrideEnd = millis() + 400;
}

void UIPatrolHUD::onBLEDevice(const char* addr, bool isTracker) {
    _bleCount++;
    _bleFlashEnd = millis() + 300;

    // Compute dot angle from address hash
    float angle = (float)(hashToRange(addr, 360));

    if (isTracker) {
        _trackCount++;
        _trackFlashEnd = millis() + 500;
        addDot(angle, COL_DOT_TRACK);

        _ringFlashColor = COL_DOT_TRACK;
        _ringFlashEnd = millis() + 220;

        // Alert animation on pet
        _petAnim.selectForStage(_stage, ANIM_ALERT);
        _petAnimOverrideEnd = millis() + 800;
    } else {
        addDot(angle, COL_DOT_BLE);

        _petAnim.selectForStage(_stage, ANIM_HAPPY);
        _petAnimOverrideEnd = millis() + 400;
    }
}

// ── Phase Control ────────────────────────────────────────────────────────

void UIPatrolHUD::setPhase(HudPhase phase) {
    _phase = phase;
    _phaseStartTime = millis();

    if (phase == HUD_PHASE_WIFI) {
        _wifiProgress = 0;
        Serial.println("[HUD] Phase: WiFi scan");
    } else if (phase == HUD_PHASE_BLE) {
        _wifiProgress = 1.0f;
        _bleProgress = 0;
        _petAnim.selectForStage(_stage, ANIM_SCAN);
        Serial.println("[HUD] Phase: BLE scan");
    } else if (phase == HUD_PHASE_COMPLETE) {
        _wifiProgress = 1.0f;
        _bleProgress = 1.0f;
        _completionPulses = 0;
        _lastPulseTime = millis();
        Serial.println("[HUD] Phase: Complete");
    }
}

// ── Completion Animation ─────────────────────────────────────────────────

void UIPatrolHUD::updateCompletion() {
    uint32_t now = millis();

    // Pulse ring green 3 times (300ms on, 200ms off = 500ms per pulse)
    if (_completionPulses >= 3) {
        _phase = HUD_PHASE_DONE;
        return;
    }

    if (now - _lastPulseTime < 500) {
        // Draw green ring during "on" phase (first 300ms)
        bool pulseOn = (now - _lastPulseTime < 300);
        uint16_t col = pulseOn ? COL_BAR_DONE : COL_BG;

        _tft->fillCircle(ringCX(), ringCY(), ringRadius() - 1, COL_BG);
        _tft->drawCircle(ringCX(), ringCY(), ringRadius(), col);
        _tft->drawCircle(ringCX(), ringCY(), ringRadius() + 1, pulseOn ? dimColor(COL_BAR_DONE, 2) : COL_BG);

        // Redraw dots during pulse
        drawDots();
        if (hubRadius() == 0) {
            drawSmallDot(*_tft, ringCX(), ringCY(), pulseOn ? COL_BAR_DONE : COL_RING);
        }
    } else {
        _lastPulseTime = now;
        _completionPulses++;
    }

    // Update progress bars to show completion
    drawProgressBars();

    // Pet goes happy
    _petAnim.selectForStage(_stage, ANIM_HAPPY);
    _petAnim.update();
    drawPetCorner();

    // Replace SCAN with DONE
    _tft->setTextColor(COL_BAR_DONE, COL_BG);
    _tft->setTextSize(HUD_TS);
    _tft->setCursor(scanLabelX(), scanLabelY());
    _tft->print("DONE");
}

// ── Static Drawing Routines ──────────────────────────────────────────────

void UIPatrolHUD::drawHeader() {
    // Header bar background
#if HEXHOUND_PANEL_ROUND
    uiround::rowFill(*_tft, uiround::scaled(0), headerHeight(), COL_HEADER_BG, 0);
#else
    _tft->fillRect(0, 0, hudWidth(), headerHeight(), COL_HEADER_BG);
#endif

    // Diamond symbol + title
    int ts = HUD_TS;
    _tft->setTextColor(COL_HEADER_TXT, COL_HEADER_BG);
    _tft->setTextSize(ts);
#if HEXHOUND_PANEL_ROUND
    // Centered under the rim rather than flush left, and one row down so the
    // glyphs clear the narrow top of the circle.
    {
        const char* stageName = PetCore::instance().stageName();
        int w = (int)(2 + strlen(stageName)) * 6 * ts;
        _tft->setCursor(uiround::CX - w / 2, uiround::scaled(22));
    }
#else
    _tft->setCursor(isBig() ? 4 : 2, isBig() ? 5 : 2);
#endif
    // On the wide portrait panel, "HEX " + a long stage name wraps at size 2,
    // so drop the "HEX " prefix there and keep just "<diamond> <stage>" (the
    // longest, "Beacon Beast", fits on one line). The compact panel keeps
    // the full "HEX" branding.
    _tft->print(isBig() ? "\x04 " : "\x04 HEX ");

    // Stage name
    const char* name = PetCore::instance().stageName();
    _tft->setTextColor(0xFD20, COL_HEADER_BG);  // amber
    _tft->print(name);
}

void UIPatrolHUD::drawRadarFrame() {
    // Ring outline
    _tft->drawCircle(ringCX(), ringCY(), ringRadius(), COL_RING);
    _tft->drawCircle(ringCX(), ringCY(), ringRadius() + 1, dimColor(COL_RING, 1));

    drawCrosshairs(dimColor(COL_RING, 1));

    // Center dot, where the centre is not standing under the pet.
    if (hubRadius() == 0) {
        drawSmallDot(*_tft, ringCX(), ringCY(), COL_RING);
    }
}

// The radar's crosshairs, drawn as two spokes per axis around a clear hub.
//
// With hubRadius() == 0 the two halves of each axis meet in the middle and this
// paints exactly the pixels the single full-length pair used to, so the
// rectangular panels are unchanged. See hubRadius() for why the round ones need
// the gap.
void UIPatrolHUD::drawCrosshairs(uint16_t color) {
    const int hub = hubRadius();
    const int arm = ringRadius() - 2 - hub;
    if (arm <= 0) return;
    _tft->drawFastHLine(ringCX() - ringRadius() + 2, ringCY(), arm, color);
    _tft->drawFastHLine(ringCX() + hub,              ringCY(), arm, color);
    _tft->drawFastVLine(ringCX(), ringCY() - ringRadius() + 2, arm, color);
    _tft->drawFastVLine(ringCX(), ringCY() + hub,              arm, color);
}

void UIPatrolHUD::drawCounters(bool force) {
    uint32_t now = millis();
    int ts = HUD_TS;
    int clearW = isBig() ? 84 : 38;
    int clearH = isBig() ? 18 : 9;
#if HEXHOUND_PANEL_ROUND
    clearW = uiround::scaled(clearW);
    clearH = uiround::scaled(clearH);
#endif
    _tft->setTextSize(ts);

    // A fixed two-column layout must never let text run past its column.
    // Real TFT_eSPI WRAPS at the right edge rather than clipping, so one glyph
    // too many does not vanish, it reappears at x=0 on the row below and lands
    // on top of the next counter. That is what "BLE LOCK" was doing to "Open:".
    //
    // Suppressing it with setTextWrap() is not portable: the vendor esp_lcd
    // TFT_eSPI shim in hal/tft_compat.h has no such method, and adding it to
    // both shims plus the sim display for a defensive call is a worse trade than
    // simply never emitting a string that does not fit. So the fix is measured
    // truncation, which behaves identically on every backend.
    const int glyphW = 6 * ts;

    // Never clear past the right edge either: clearW is a fixed 84 and the right
    // column starts at hudWidth()/2 + 6, so on a 170-wide portrait panel the
    // clear rect alone ran 5px over.
    auto cellWidth = [&](int x) {
        int w = clearW;
        if (x + w > hudWidth()) w = hudWidth() - x;
        return w;
    };
    auto clearCell = [&](int x, int y) {
        const int w = cellWidth(x);
        if (w > 0) _tft->fillRect(x, y, w, clearH, COL_BG);
    };

    auto fitted = [&](int x, const char* text, char* out, size_t outSize) {
        const int room = hudWidth() - x;
        size_t maxChars = (glyphW > 0 && room > 0) ? (size_t)(room / glyphW) : 0;
        if (maxChars > outSize - 1) maxChars = outSize - 1;
        size_t i = 0;
        for (; i < maxChars && text[i]; ++i) out[i] = text[i];
        out[i] = '\0';
        return out;
    };

    // Only redraw counters that changed
    auto drawOne = [&](int x, int y, const char* label, int val, int& lastVal,
                       uint32_t flashEnd) {
        if (!force && val == lastVal) return;
        lastVal = val;

        clearCell(x, y);

        // Label
        _tft->setTextColor(COL_LABEL, COL_BG);
        _tft->setCursor(x, y);
        _tft->print(label);

        // Value (flash yellow briefly on change)
        uint16_t valCol = (now < flashEnd) ? COL_FLASH : COL_COUNTER;
        _tft->setTextColor(valCol, COL_BG);
        _tft->printf("%d", val);
    };

    auto drawStatus = [&](int x, int y, const char* text) {
        char buf[16];
        clearCell(x, y);
        _tft->setTextColor(COL_LABEL, COL_BG);
        _tft->setCursor(x, y);
        _tft->print(fitted(x, text, buf, sizeof(buf)));
    };

    // "BLE LOCK" is 8 glyphs, which at text size 2 is 96px, and the right column
    // starts at hudWidth()/2 + 6. That is 91 on the T-Display S3 (170 wide in
    // portrait) and 92 on the Waveshares (172), so the label ran to 187 or 188
    // and overflowed EVERY big panel, not just one. It only ever showed with the
    // BLE row locked, i.e. a pet below Beacon Beast, which is the state every
    // brand new giveaway unit is in - so the first patrol a new owner runs was
    // the one guaranteed to show it. Measure the room instead of assuming a big
    // panel is a wide one.
    const int rightRoom = hudWidth() - rightCounterX();
    const char* bleLockLabel =
        ((int)(sizeof("BLE LOCK") - 1) * glyphW <= rightRoom) ? "BLE LOCK"
                                                             : "B:LOCK";

    // Two columns of counters below the radar
    int gap = counterRowGap();
    drawOne(leftCounterX(),  counterY(),       "WiFi:", _wifiCount,  _lastDrawnWifi,  _wifiFlashEnd);
    drawOne(leftCounterX(),  counterY() + gap, "Open:", _openCount,  _lastDrawnOpen,  _openFlashEnd);
    if (bleUnlocked()) {
        drawOne(rightCounterX(), counterY(),       "BLE:",  _bleCount,   _lastDrawnBle,   _bleFlashEnd);
        drawOne(rightCounterX(), counterY() + gap, "Trk:",  _trackCount, _lastDrawnTrack, _trackFlashEnd);
    } else {
        drawStatus(rightCounterX(), counterY(),       bleLockLabel);
        drawStatus(rightCounterX(), counterY() + gap, "T:--");
        _lastDrawnBle = -1;
        _lastDrawnTrack = -1;
    }
}

void UIPatrolHUD::drawProgressBars() {
#if HEXHOUND_PANEL_ROUND
    // Round: scan progress rides the rim as two arcs rather than two stacked
    // horizontal bars. Stacked bars plus their labels need ~40px of vertical
    // space below the counters, which does not exist on a circle once the
    // chord narrows - and arcs match the language the home screen already uses.
    //
    //   WiFi  left rim,  150..210 deg
    //   BLE   right rim, 330..390 deg
    const int wifiPct = (int)(_wifiProgress * 100.0f);
    const int blePct  = (int)(_bleProgress * 100.0f);

    uint16_t wifiCol = (_wifiProgress >= 1.0f) ? COL_BAR_DONE : COL_BAR_WIFI;
    uiround::arcGauge(*_tft, 150.0f, 210.0f, wifiPct,
                      uiround::RING_R, uiround::RING_TH, wifiCol, COL_BAR_BG);

    if (bleUnlocked()) {
        uint16_t bleCol = (_bleProgress >= 1.0f) ? COL_BAR_DONE : COL_BAR_BLE;
        uiround::arcGauge(*_tft, 330.0f, 390.0f, blePct,
                          uiround::RING_R, uiround::RING_TH, bleCol, COL_BAR_BG);
    } else {
        uiround::arc(*_tft, 330.0f, 390.0f,
                     uiround::RING_R, uiround::RING_TH, COL_BAR_BG);
    }

    _tft->setTextSize(uiround::ts(1));
    _tft->setTextColor(COL_LABEL, COL_BG);
    _tft->setCursor(uiround::rowLeft(uiround::CY) + uiround::scaled(22),
                    uiround::CY - uiround::scaled(4));
    _tft->print("WF");
    _tft->setTextColor(bleUnlocked() ? COL_PHASE_BLE : COL_LABEL, COL_BG);
    _tft->setCursor(uiround::rowRight(uiround::CY) - uiround::scaled(34),
                    uiround::CY - uiround::scaled(4));
    _tft->print(bleUnlocked() ? "BL" : "LK");
    return;
#else
    bool big = isBig();

    // The row gap has to swallow the SECOND bar's label, because on the big
    // panel labels are drawn 10px above their own bar (8px of text plus 2px of
    // padding). At the old `barHeight() + 6` that gap was 16 while the label
    // needed 18, so "LOCK"/"BLE" was drawn at bleBarY-10 = 282 and landed
    // INSIDE the WiFi bar, which occupies 276..286. Visible as the WIFI and
    // LOCK labels sitting on top of each other's bars on a T-Display S3.
    //
    // barHeight() + 14 leaves the label clear of the bar above with 4px to
    // spare, and still ends the second bar 10px short of the panel bottom.
    // The compact panel draws labels to the LEFT of its bars, so it never had
    // this constraint and keeps its original spacing.
    int rowGap = barHeight() + (big ? 14 : 3);
    _tft->setTextSize(1);

    // On the big portrait panel the pet sits bottom-left, so put the WiFi/BLE
    // bar labels ABOVE the bars (centered on barX); on the compact panel keep
    // them to the left as before.
    int wifiBarY = progressY();
    int bleBarY  = progressY() + rowGap;

    // WiFi progress bar
    int wifiFilled = (int)(_wifiProgress * barWidth());
    uint16_t wifiCol = (_wifiProgress >= 1.0f) ? COL_BAR_DONE : COL_BAR_WIFI;
    _tft->fillRect(barX(), wifiBarY, barWidth(), barHeight(), COL_BAR_BG);
    if (wifiFilled > 0)
        _tft->fillRect(barX(), wifiBarY, wifiFilled, barHeight(), wifiCol);

    // BLE progress bar
    int bleFilled = (int)(_bleProgress * barWidth());
    uint16_t bleCol = bleUnlocked()
        ? ((_bleProgress >= 1.0f) ? COL_BAR_DONE : COL_BAR_BLE)
        : COL_BAR_BG;
    _tft->fillRect(barX(), bleBarY, barWidth(), barHeight(), COL_BAR_BG);
    if (bleUnlocked() && bleFilled > 0)
        _tft->fillRect(barX(), bleBarY, bleFilled, barHeight(), bleCol);

    // Labels
    if (big) {
        _tft->setTextColor(COL_LABEL, COL_BG);
        _tft->setCursor(barX(), wifiBarY - 10);
        _tft->print("WIFI");
        _tft->setTextColor(bleUnlocked() ? COL_PHASE_BLE : COL_LABEL, COL_BG);
        _tft->setCursor(barX(), bleBarY - 10);
        _tft->print(bleUnlocked() ? "BLE" : "LOCK");
    } else {
        _tft->setTextColor(COL_LABEL, COL_BG);
        _tft->setCursor(2, wifiBarY - 1);
        _tft->print("WIF");
        _tft->setTextColor(bleUnlocked() ? COL_PHASE_BLE : COL_LABEL, COL_BG);
        _tft->setCursor(2, bleBarY - 1);
        _tft->print(bleUnlocked() ? "BLE" : "LCK");
    }
#endif // !HEXHOUND_PANEL_ROUND
}

void UIPatrolHUD::drawPetCorner() {
#if !HEXHOUND_PANEL_ROUND
    // Clear the pet area. KEPT after the switch to the HD still, unlike on the
    // round panels, and the reason is worth stating because it is not obvious:
    // the HD art is about 83% chroma key (measured - see hubRadius), so
    // blitting it does NOT overwrite what is already in the box. On a round
    // panel update() has cleared the whole ring interior a moment earlier; on a
    // rectangular one this corner sits outside the ring and nothing else
    // touches it, so without this fillRect the previous frame shows through
    // every gap in the silhouette.
    _tft->fillRect(petX(), petY(), petDrawWidth(), petDrawHeight(), COL_BG);
#endif
#if HEXHOUND_PANEL_ROUND
    // NO CLEAR HERE. This is the black box.
    //
    // update() has already erased the whole ring interior with one fillCircle
    // before drawing the ring, the crosshair, the sweep and the dots, and the
    // pet's bounding square is comfortably inside that circle: at 480 the box is
    // 132 px, so its corners sit 93 px from the centre against an interior
    // cleared to 107. There is nothing left to clear, and clearing it anyway
    // stamped a hard-edged black rectangle over the radar AFTER the radar had
    // been drawn.
    //
    // Blitting straight over the sweep is correct, not a compromise: the art is
    // 83% chroma key (measured - see hubRadius) so the beams, the crosshair and
    // the ring show through everywhere the hound is not, and are occluded only
    // by the hound itself. That is what a radar with something standing on it
    // looks like.
    static_assert(HD_HOME_SRC_PX % 4 == 0,
                  "the HUD pet draws the baked HD art at 3/4; a source size "
                  "that is not a multiple of 4 would make that non-integral");
    // The HD still, not the animated pixel sprite. On a round panel the pet
    // rides the middle of the radar and the old pixel scale factor was
    // 2 * TEXT_SCALE, so a 16x16 sprite arrived on the 480 as a 64x64
    // nearest-neighbour blow-up: the blockiest thing on a screen where
    // everything else is smooth. The HD art is already resident in flash for
    // the home and den screens, so this costs nothing to store.
    //
    // The rectangular branch below now does the same thing for the same
    // reason, so that factor no longer exists on any board.
    //
    // The HD set has no motion frames, so this is a still and the ANIM_SCAN /
    // ANIM_HAPPY / ANIM_ALERT cycling is simply not shown here. Nothing the
    // wearer could read is lost: every discovery already lands on three other
    // channels - the counter flashes, a dot appears on the rim, and a duplicate
    // SSID or a tracker flashes the ring itself. A 400 ms sprite swap under a
    // spinning sweep was the least legible of the four. The animator is still
    // driven by update() because the same object serves the non-round boards.
    //
    // srcW/srcH are HD_HOME_SRC_PX because that is the size the art is BAKED
    // at. Passing the DRAWN size instead is the mistake that read 153 KB past
    // the end of the hero array and hard-faulted this board.
    drawSpriteResized(*_tft, getStageHomeHD(_stage),
                      HD_HOME_SRC_PX, HD_HOME_SRC_PX,
                      petX(), petY(), petDrawWidth(), petDrawHeight());
    drawWornOn(*_tft, _stage, petX(), petY(), petDrawWidth());
#else
    // The HD still, not the animated pixel sprite. Same choice the round branch
    // above makes, and the argument that justified it there was never
    // round-specific.
    //
    // Every discovery already lands on three other channels: the counter
    // flashes, a dot appears on the rim, and a duplicate SSID or a tracker
    // flashes the ring itself. A sprite swap every 250 to 400 ms was the least
    // legible of the four - and it was least legible HERE of anywhere, because
    // the scale factor was 1 on the big panels, which put a 16 px pet in a
    // corner under a spinning sweep.
    //
    // What the swap costs, and what buying it back is worth, is the cosmetics.
    // drawWornOn() places overlays from an anchor table normalised to the HD
    // art's own source canvas: it is correct over this image and is NOT correct
    // over a pixel frame, which is framed 108 to 200 thousandths tighter to the
    // figure and whose silhouette moves between frames. ui_worn.h has the
    // measurements and the rejected alternatives. Drawing the HD still is what
    // closes that gap with no second table to keep in step forever.
    //
    // _petAnim is still selected by begin()/onWiFiNetwork()/onBLEDevice() and
    // still advanced by update(); only the draw is gone. Leaving its frame
    // index current costs nothing and keeps one object serving all eight
    // boards.
    //
    // srcW/srcH are HD_HOME_SRC_PX because that is the size the art is BAKED
    // at, never the size it is drawn at. That confusion once read 153 KB past
    // the end of the hero array and hard-faulted a board. drawHDArt() takes the
    // plain-blit path on the compact panel, where baked and drawn are both 24.
    drawHDArt(*_tft, getStageHomeHD(_stage), petX(), petY(),
              HD_HOME_SRC_PX, petDrawWidth());
    drawWornOn(*_tft, _stage, petX(), petY(), petDrawWidth());
#endif
}

void UIPatrolHUD::drawPhaseLabel() {
#if HEXHOUND_PANEL_ROUND
    // 28x8 per unit of text size reproduces the old 56x16 at 240 and grows with
    // the panel; the rectangular literals below are untouched on purpose.
    _tft->fillRect(scanLabelX(), scanLabelY(), 28 * HUD_TS, 8 * HUD_TS, COL_BG);
#else
    _tft->fillRect(scanLabelX(), scanLabelY(), isBig() ? 56 : 28, isBig() ? 16 : 8,
                   COL_BG);
#endif
    _tft->setTextColor((_phase == HUD_PHASE_BLE) ? COL_PHASE_BLE : COL_SCAN_PULSE,
                       COL_BG);
    _tft->setTextSize(HUD_TS);
    _tft->setCursor(scanLabelX(), scanLabelY());
    if (_phase == HUD_PHASE_WIFI) {
        _tft->print("WIFI");
    } else if (_phase == HUD_PHASE_BLE) {
        _tft->print("BLE");
    } else {
        _tft->print("SCAN");
    }
}
