#include "ui_home.h"
#include "../pet/pet_core.h"
#include "../modules/battery_module.h"
#include "../config.h"
#include "ui_round.h"
#include "ui_worn.h"
#include "ui_flourish.h"   // drawFlourish(), flourishForget(), clearFlourish()
#include "sprites_hd_poses.h"   // getStagePoseHD(), PetPose, HD_POSE_PX

// ── HexHound - Home Screen Implementation ───────────────────────

// Color palette
#define COL_BG        TFT_BLACK
#define COL_TITLE     TFT_CYAN
#define COL_STAGE     TFT_YELLOW
#define COL_HUNGER    TFT_GREEN
#define COL_MOOD      TFT_MAGENTA
#define COL_XP        TFT_ORANGE
#define COL_TEXT      TFT_WHITE
#define COL_BAR_BG    0x2104   // dark gray
#define COL_TRUST     TFT_BLUE
#define COL_MISCHIEF  TFT_RED
#define COL_ENERGY    TFT_YELLOW

UIHome& UIHome::instance() {
    static UIHome ui;
    return ui;
}

// Which baked pose an animation state should be drawn as, or -1 for the plain
// resting portrait. Only the states the home screen can actually select are
// listed; anything else answers -1 and takes the pixel path as before.
static int8_t homePoseFor(uint8_t anim) {
    switch (anim) {
        case ANIM_HUNGRY: return (int8_t)POSE_HUNGRY;
        case ANIM_HAPPY:  return (int8_t)POSE_HAPPY;
        case ANIM_ALERT:  return (int8_t)POSE_ALERT;
        default:          return -1;
    }
}

void UIHome::init(TFT_eSPI* tft) {
    _tft = tft;
    // Start with idle animation for current stage
    auto stage = PetCore::instance().state().stage;
    _lastStage = stage;
    _animator.selectForStage(stage, ANIM_IDLE);
}

void UIHome::draw(bool forceRedraw) {
#if HEXHOUND_PANEL_ROUND
    drawRound(forceRedraw);
#else
    if (!_tft) return;
    const auto& pet = PetCore::instance();
    const auto& st  = pet.state().stats;
    auto stage = pet.state().stage;
    int batteryPct = BatteryModule::instance().isAvailable()
        ? BatteryModule::instance().percent()
        : -1;
    uint32_t masteryCurrent = pet.state().masteryXP - pet.masteryForCurrentRank();
    uint32_t masteryNeeded = pet.masteryForNextRank() - pet.masteryForCurrentRank();

    if (stage != _lastStage) {
        _lastStage = stage;
        _animator.selectForStage(stage, ANIM_IDLE);
        forceRedraw = true;
    }

    // Update animator frame
    _animator.update();

    // Only redraw text/bars if stats changed or forced
    bool statsChanged = (st.hunger   != _lastHunger ||
                         st.mood     != _lastMood ||
                         st.xp       != _lastXP ||
                         st.trust    != _lastTrust ||
                         st.mischief != _lastMischief ||
                         st.energy   != _lastEnergy ||
                         batteryPct  != _lastBatteryPct);

    const bool bigScreen = (SCREEN_H > 100);

    if (forceRedraw || statsChanged) {
        _lastHunger   = st.hunger;
        _lastMood     = st.mood;
        _lastXP       = st.xp;
        _lastTrust    = st.trust;
        _lastMischief = st.mischief;
        _lastEnergy   = st.energy;
        _lastBatteryPct = batteryPct;

        _tft->fillScreen(COL_BG);

        const char* traitNames[] = {
            "Curious", "Protect", "Chaotic", "Sleepy", "Greedy", "Brave"
        };

        if (bigScreen) {
            // ── Large display layout (Waveshare 320x172) ──────────────────
            // Stats live in the top ~95px; the pet fills the roomy lower area.

            // Title - pet name (large font)
            _tft->setTextColor(COL_TITLE, COL_BG);
            _tft->setTextSize(2);
            _tft->setCursor(6, 4);
            _tft->print(pet.state().name);
            _tft->setTextSize(1);

            if (batteryPct >= 0) {
                _tft->setTextColor(COL_TEXT, COL_BG);
                _tft->setCursor(SCREEN_W - 52, 6);
                _tft->printf("BAT %d%%", batteryPct);
            }

            // Stage label (large font)
            _tft->setTextColor(COL_STAGE, COL_BG);
            _tft->setTextSize(2);
            _tft->setCursor(6, 26);
            _tft->printf("%s", pet.stageName());
            _tft->setTextSize(1);

            // Traits (or mastery title for Sentinel)
            _tft->setTextColor(COL_TEXT, COL_BG);
            _tft->setCursor(6, 46);
            if (stage >= STAGE_SENTINEL) {
                _tft->printf("%s R%u  %s/%s",
                             pet.masteryTitle(), pet.state().masteryRank + 1,
                             traitNames[pet.state().traits[0]],
                             traitNames[pet.state().traits[1]]);
            } else {
                _tft->printf("%s / %s",
                             traitNames[pet.state().traits[0]],
                             traitNames[pet.state().traits[1]]);
            }

            int barY = 60;

            // Hunger / Mood (left column)
            drawStatBar(6, barY,      100, 8, st.hunger, COL_HUNGER, "Hun", 26);
            drawStatBar(6, barY + 14, 100, 8, st.mood,   COL_MOOD,   "Mod", 26);

            // XP / mastery progress
            _tft->setTextColor(COL_XP, COL_BG);
            _tft->setCursor(6, barY + 30);
            if (stage >= STAGE_SENTINEL) {
                _tft->printf("MR %u  %lu/%lu",
                             pet.state().masteryRank + 1,
                             masteryCurrent, masteryNeeded);
            } else {
                _tft->printf("XP %lu/%lu", st.xp, pet.xpForNextStage());
            }

            // Trust / Mischief / Energy (right column). Leave room on the
            // right for the value text (up to 3 digits) so nothing clips.
            int rx = SCREEN_W - 100;
            drawStatBar(rx, barY,      44, 8, st.trust,    COL_TRUST,    "TRU", 22);
            drawStatBar(rx, barY + 14, 44, 8, st.mischief, COL_MISCHIEF, "MIS", 22);
            drawStatBar(rx, barY + 28, 44, 8, st.energy,   COL_ENERGY,   "NRG", 22);
        } else {
            // ── Compact layout (LilyGo T-Dongle S3 160x80) ────────────────
            _tft->setTextColor(COL_TITLE, COL_BG);
            _tft->setTextSize(1);
            _tft->setCursor(4, 2);
            _tft->print(pet.state().name);

            if (batteryPct >= 0) {
                _tft->setTextColor(COL_TEXT, COL_BG);
                _tft->setCursor(SCREEN_W - 48, 2);
                _tft->printf("BAT %d%%", batteryPct);
            }

            // Stage label
            _tft->setTextColor(COL_STAGE, COL_BG);
            _tft->setCursor(4, 14);
            _tft->printf("Stage: %s", pet.stageName());

            int barY = 40;

            // Traits
            _tft->setTextColor(COL_TEXT, COL_BG);
            _tft->setCursor(4, 26);
            _tft->printf("%s/%s",
                         traitNames[pet.state().traits[0]],
                         traitNames[pet.state().traits[1]]);

            // Stat bars
            drawStatBar(4, barY,      55, 6, st.hunger, COL_HUNGER, "Hun");
            drawStatBar(4, barY + 12, 55, 6, st.mood,   COL_MOOD,   "Mod");

            // XP display
            _tft->setTextColor(COL_XP, COL_BG);
            _tft->setCursor(4, barY + 26);
            if (stage >= STAGE_SENTINEL) {
                _tft->printf("MR %u %lu/%lu",
                             pet.state().masteryRank + 1,
                             masteryCurrent,
                             masteryNeeded);
            } else {
                _tft->printf("XP %lu/%lu", st.xp, pet.xpForNextStage());
            }

            // Trust / Mischief / Energy (right column with bars)
            int rx = 84;
            drawStatBar(rx, barY,      30, 6, st.trust,    COL_TRUST,    "TRU", 22);
            drawStatBar(rx, barY + 12, 30, 6, st.mischief, COL_MISCHIEF, "MIS", 22);
            drawStatBar(rx, barY + 24, 30, 6, st.energy,   COL_ENERGY,   "NRG", 22);
        }

        // Force sprite redraw after the panel was cleared
        _lastDrawnFrame = -1;
        _hdDrawnStage   = -1;
    }

    // Redraw the pet sprite. On the large display the pet is enlarged and
    // centered in the empty lower half; on the compact display it stays in
    // the top-right corner at native size.
    int spriteSize = (stage == STAGE_SENTINEL) ? 20 : 16;
    int frameNow   = _animator.currentFrame();

    // The four states with an HD portrait, and the states without one.
    //
    // It used to be just ANIM_IDLE: everything else fell through to the 16x16
    // pixel sprite, which on the 480 panel is drawn at scale 8 and is the
    // blockiest thing on the board - reported from glass as the pet looking
    // pixelated for a moment. Worse, worn cosmetics cannot be placed on the
    // pixel art at all (the anchor table is normalised to the HD canvas), so
    // the pet also lost its hat while it was there.
    //
    // HUNGRY, HAPPY and ALERT now have their own baked poses. What is left on
    // the pixel path is SCAN, HATCH, TYPE and LAUGH, none of which the home
    // screen selects.
    const int8_t pose = homePoseFor(_animator.currentAnim());
    const bool   resting = (_animator.currentAnim() == ANIM_IDLE) || pose >= 0;

    // A pose is drawn into the slot the resting portrait was laid out for, so
    // they have to be the same size on every panel family.
    static_assert(HD_POSE_PX == HD_HOME_SRC_PX,
                  "a pose is baked at a different size from the resting "
                  "portrait; re-run scripts/gen_hd_poses.py");
    const uint16_t* restArt = (pose >= 0)
        ? getStagePoseHD(stage, (PetPose)pose) : getStageHomeHD(stage);

    if (bigScreen) {
        int petScale   = (spriteSize >= 20) ? 3 : 4;   // 60px or 64px tall
        int petTop     = 100;
        int petAreaH   = SCREEN_H - petTop - 4;
        int drawW      = spriteSize * petScale;
        int drawH      = spriteSize * petScale;
        int sprX       = (SCREEN_W - drawW) / 2;
        int sprY       = petTop + (petAreaH - drawH) / 2;

        // The two footprints differ, so clear the whole pet area on any switch
        // between them rather than leaving a fringe of the previous image.
        int slotW = (drawW > HD_HOME_PX) ? drawW : HD_HOME_PX;
        int slotH = (drawH > HD_HOME_PX) ? drawH : HD_HOME_PX;
        int slotX = (SCREEN_W - slotW) / 2;
        int slotY = petTop + (petAreaH - slotH) / 2;

        if (resting) {
            const uint16_t worn = wornSignature();
            // Hoisted out of the redraw branch: the flourish is drawn on every
            // frame, not only the ones that repaint the pet, so it needs the
            // portrait's corner even when nothing else does.
            const int hx = (SCREEN_W - HD_HOME_PX) / 2;
            const int hy = petTop + (petAreaH - HD_HOME_PX) / 2;
            if (_hdDrawnStage != (int)stage || _wornDrawn != worn ||
                _hdDrawnPose != pose) {
                _tft->fillRect(slotX, slotY, slotW, slotH, COL_BG);
                drawSprite(*_tft, restArt, hx, hy, HD_HOME_PX, HD_HOME_PX);
                drawWornOn(*_tft, stage, hx, hy, HD_HOME_PX);
                // The pet frame just painted over any motes standing in its
                // box, so the flourish must forget where they were rather than
                // try to erase pixels the pet now owns.
                flourishForget();
                _hdDrawnStage   = (int)stage;
                _wornDrawn      = worn;
                _hdDrawnPose    = pose;
                _lastDrawnFrame = -1;
            }
            drawFlourish(*_tft, hx, hy, HD_HOME_PX, millis());
        } else {
            if (_hdDrawnStage >= 0) {
                clearFlourish(*_tft);
                _tft->fillRect(slotX, slotY, slotW, slotH, COL_BG);
                _hdDrawnStage = -1;
            }
            if (frameNow != _lastDrawnFrame) {
                _animator.clear(*_tft, sprX, sprY, COL_BG, petScale);
                _animator.draw(*_tft, sprX, sprY, petScale);
                _lastDrawnFrame = frameNow;
            }
        }
    } else {
        int petScale = 1;
        int sprX = SCREEN_W - spriteSize - 8;
        int sprY = 2;
#ifdef HEXHOUND_BOARD_LILYGO_T_DONGLE
        if (spriteSize < 20) {
            petScale = 2;
            sprX = SCREEN_W - (spriteSize * petScale) - 6;
            sprY = 4;
        }
#endif
        // HD slot on the compact panel: the free block beside the stat bars,
        // below the battery line. Verified against the 160x80 layout above.
        const int hdX = SCREEN_W - HD_HOME_PX - 4;
        const int hdY = 10;

        if (resting) {
            const uint16_t worn = wornSignature();
            if (_hdDrawnStage != (int)stage || _wornDrawn != worn ||
                _hdDrawnPose != pose) {
                _animator.clear(*_tft, sprX, sprY, COL_BG, petScale);
                _tft->fillRect(hdX, hdY, HD_HOME_PX, HD_HOME_PX, COL_BG);
                drawSprite(*_tft, restArt, hdX, hdY, HD_HOME_PX, HD_HOME_PX);
                drawWornOn(*_tft, stage, hdX, hdY, HD_HOME_PX);
                flourishForget();
                _hdDrawnStage   = (int)stage;
                _wornDrawn      = worn;
                _hdDrawnPose    = pose;
                _lastDrawnFrame = -1;
            }
            drawFlourish(*_tft, hdX, hdY, HD_HOME_PX, millis());
        } else {
            if (_hdDrawnStage >= 0) {
                clearFlourish(*_tft);
                _tft->fillRect(hdX, hdY, HD_HOME_PX, HD_HOME_PX, COL_BG);
                _hdDrawnStage = -1;
            }
            _animator.clear(*_tft, sprX, sprY, COL_BG, petScale);
            _animator.draw(*_tft, sprX, sprY, petScale);
            _lastDrawnFrame = frameNow;
        }
    }
#endif // !HEXHOUND_PANEL_ROUND
}

#if HEXHOUND_PANEL_ROUND

// ── Round layout (240x240 GC9A01) ─────────────────────────────────────────
//
// The pet is the whole screen. Stats move off the body and onto the rim: XP is
// a full progress ring at the very edge, hunger and mood are side arcs. That
// buys the entire middle of the glass for the HD portrait, and it is the one
// layout where a circular panel is an advantage rather than a constraint.
//
//   XP ring        r=117, full turn from 12 o'clock
//   HUN arc        r=108, left side  (155..205 deg)
//   MOD arc        r=108, right side (335..25 deg)
//   name           y=26   size 2, centered
//   stage          y=46   size 1, centered
//   portrait       y=62   88px, centered
//   xp / mastery   y=156  size 1, centered
//   TRU/MIS/NRG    y=172  size 1, centered
//   battery        y=188  size 1, centered
//   hint           y=206  size 1, centered

void UIHome::drawRound(bool forceRedraw) {
    if (!_tft) return;
    const auto& pet = PetCore::instance();
    const auto& st  = pet.state().stats;
    auto stage = pet.state().stage;
    int batteryPct = BatteryModule::instance().isAvailable()
        ? BatteryModule::instance().percent()
        : -1;
    uint32_t masteryCurrent = pet.state().masteryXP - pet.masteryForCurrentRank();
    uint32_t masteryNeeded  = pet.masteryForNextRank() - pet.masteryForCurrentRank();

    if (stage != _lastStage) {
        _lastStage = stage;
        _animator.selectForStage(stage, ANIM_IDLE);
        forceRedraw = true;
    }

    _animator.update();

    bool statsChanged = (st.hunger   != _lastHunger ||
                         st.mood     != _lastMood ||
                         st.xp       != _lastXP ||
                         st.trust    != _lastTrust ||
                         st.mischief != _lastMischief ||
                         st.energy   != _lastEnergy ||
                         batteryPct  != _lastBatteryPct);

    if (forceRedraw || statsChanged) {
        _lastHunger     = st.hunger;
        _lastMood       = st.mood;
        _lastXP         = st.xp;
        _lastTrust      = st.trust;
        _lastMischief   = st.mischief;
        _lastEnergy     = st.energy;
        _lastBatteryPct = batteryPct;

        // Only a full redraw clears the panel. A stats-only refresh repaints
        // just the text block below the portrait: clearing the whole screen
        // every time a stat moved is what makes the display visibly flash,
        // because the arcs and the 88px portrait then have to be repainted
        // from scratch. The arcs self-clear (arcGauge paints its background
        // sweep first), and the name/stage rows above the portrait cannot
        // change without a forceRedraw.
        if (forceRedraw) {
            _tft->fillScreen(COL_BG);
        } else {
            // Scaled: at 240 this band sits just BELOW the portrait slot, but
        // unscaled on the 480 panel it cut straight through the top of it,
        // and the _hdDrawnStage cache then never repainted the pet - which
        // is why the egg looked cropped rather than misplaced.
        _tft->fillRect(0, uiround::scaled(150), uiround::DIAM,
                       uiround::scaled(70), COL_BG);
        }

        // ── Rim gauges ────────────────────────────────────────────────────
        // Growth ring: XP toward the next stage, or mastery within the rank
        // once the pet is a Sentinel and has nothing left to evolve into.
        int growthPct;
        if (stage >= STAGE_SENTINEL) {
            growthPct = masteryNeeded > 0
                ? (int)((masteryCurrent * 100) / masteryNeeded) : 0;
        } else {
            uint32_t need = pet.xpForNextStage();
            growthPct = need > 0 ? (int)((st.xp * 100) / need) : 0;
        }
        uiround::progressRing(*_tft, growthPct, COL_XP, COL_BAR_BG);

        // Side arcs: the two stats the keeper actually acts on.
        uiround::arcGauge(*_tft, 155.0f, 205.0f, st.hunger,
                          uiround::ARC_R, uiround::ARC_TH, COL_HUNGER, COL_BAR_BG);
        uiround::arcGauge(*_tft, 335.0f, 385.0f, st.mood,
                          uiround::ARC_R, uiround::ARC_TH, COL_MOOD, COL_BAR_BG);

        // ── Text stack ────────────────────────────────────────────────────
        // Name and stage sit above the portrait and only a full redraw can
        // change them, so a stats-only refresh leaves them alone rather than
        // repainting over untouched pixels.
        if (forceRedraw) {
            uiround::centerText(*_tft, uiround::scaled(26), pet.state().name, 2, COL_TITLE, COL_BG);
            uiround::centerText(*_tft, uiround::scaled(46), pet.stageName(), 1, COL_STAGE, COL_BG);
        }

        if (stage >= STAGE_SENTINEL) {
            uiround::centerPrintf(*_tft, uiround::scaled(156), 1, COL_XP, COL_BG,
                                  "%s R%u  %lu/%lu",
                                  pet.masteryTitle(), pet.state().masteryRank + 1,
                                  (unsigned long)masteryCurrent,
                                  (unsigned long)masteryNeeded);
        } else {
            uiround::centerPrintf(*_tft, uiround::scaled(156), 1, COL_XP, COL_BG,
                                  "XP %lu/%lu",
                                  (unsigned long)st.xp,
                                  (unsigned long)pet.xpForNextStage());
        }

        // Traits sit under the portrait rather than beside it - at 240 wide
        // there is no side column that would not collide with the arcs.
        static const char* kTraits[] = {
            "Curious", "Protect", "Chaotic", "Sleepy", "Greedy", "Brave"
        };
        uiround::centerPrintf(*_tft, uiround::scaled(168), 1, COL_TEXT, COL_BG, "%s / %s",
                              kTraits[pet.state().traits[0]],
                              kTraits[pet.state().traits[1]]);

        uiround::centerPrintf(*_tft, uiround::scaled(182), 1, COL_TEXT, COL_BG,
                              "T%d  M%d  E%d", st.trust, st.mischief, st.energy);

        if (batteryPct >= 0) {
            uiround::centerPrintf(*_tft, uiround::scaled(196), 1, COL_TEXT, COL_BG,
                                  "BAT %d%%", batteryPct);
        }

        uiround::centerText(*_tft, uiround::scaled(210), "hold = menu", 1, TFT_DARKGREY, COL_BG);

        // Only invalidate the pet slot when the panel was actually cleared.
        // Resetting these on every stats change would repaint the portrait
        // every 2 seconds - the other half of the flashing.
        if (forceRedraw) {
            _lastDrawnFrame = -1;
            _hdDrawnStage   = -1;
        }
    }

    drawRoundPet(stage, _animator.currentAnim() == ANIM_IDLE,
                 homePoseFor(_animator.currentAnim()),
                 _animator.currentFrame());
}

void UIHome::drawRoundPet(PetStage stage, bool resting, int8_t pose,
                          int frameNow) {
    // The portrait slot is the middle band of the glass, clear of the text
    // stack above and below. Both the HD portrait and the pixel animation are
    // centered in it, and the slot is cleared on any switch between them so no
    // fringe of the previous image survives.
    // All three were tuned for the 240 panel. slotTop unscaled put the
    // portrait over the title on the 480 panel, and because the HD art
    // composites its soft edges over black, that read as an opaque box eating
    // the header rather than as a misplaced sprite.
    const int slotTop = uiround::scaled(62);
    const int spriteSize = (stage == STAGE_SENTINEL) ? 20 : 16;
    const int petScale = 4 * uiround::TEXT_SCALE;
    const int animW = spriteSize * petScale;

    const int slotSide = (animW > HD_HOME_PX) ? animW : HD_HOME_PX;
    const int slotX = uiround::CX - slotSide / 2;
    const int slotY = slotTop + (HD_HOME_PX - slotSide) / 2;

    // A posed pet gets its still, never the idle breath. The breath is what
    // "at rest and well" looks like; playing it under a hungry pose would say
    // the opposite of what the portrait says.
    const uint16_t* restArt = (pose >= 0)
        ? getStagePoseHD(stage, (PetPose)pose) : getStageHomeHD(stage);

    if (resting && pose < 0) {
#if HEXHOUND_HD_IDLE
        // On the 480 panel the resting pet BREATHES rather than freezing: idle
        // has HD motion frames baked for it, so the one state the keeper
        // actually watches never drops to 8px blocks. frameNow is the
        // animator's own index into the pixel idle loop, which has the same two
        // frames at the same per-stage cadence, so the HD loop inherits its
        // timing for free.
        //
        // _lastDrawnFrame uses -1 as "nothing drawn yet", so the frame index
        // stored in it must never be able to BE -1. A negative frameNow would
        // make it one (C++ modulo keeps the sign), the two states would become
        // indistinguishable, and the pet would stop redrawing. Clamp here
        // rather than trust the animator to stay non-negative forever.
        const int idleFrame = (frameNow < 0)
            ? 0 : (frameNow % getStageIdleHDCount());
        // Only the FIRST frame after a switch clears the slot. The frames are
        // opaque, so each one overwrites the last completely - and clearing to
        // black between them would be a visible flash every 600 ms, forever.
        const uint16_t worn = wornSignature();
        if (_hdDrawnStage != (int)stage || _wornDrawn != worn) {
            _tft->fillRect(slotX, slotY, slotSide, slotSide, COL_BG);
            _hdDrawnStage   = (int)stage;
            _wornDrawn      = worn;
            _lastDrawnFrame = -1;
        }
        if (idleFrame != _lastDrawnFrame) {
            drawSprite(*_tft, getStageIdleHD(stage, idleFrame),
                       uiround::CX - HD_IDLE_PX / 2, slotTop,
                       HD_IDLE_PX, HD_IDLE_PX);
            // Over each breath frame, not once: the frames are opaque and each
            // one repaints the whole pet, so an overlay drawn only on the first
            // would be wiped by the second and the hat would flicker at the
            // idle cadence.
            drawWornOn(*_tft, stage, uiround::CX - HD_IDLE_PX / 2, slotTop,
                       HD_IDLE_PX);
            // The breath frame just painted over any motes in the pet's box.
            flourishForget();
            _lastDrawnFrame = idleFrame;
        }
        drawFlourish(*_tft, uiround::CX - HD_IDLE_PX / 2, slotTop,
                     HD_IDLE_PX, millis());
        return;
#else
        const uint16_t worn = wornSignature();
        if (_hdDrawnStage != (int)stage || _wornDrawn != worn ||
            _hdDrawnPose != pose) {
            _tft->fillRect(slotX, slotY, slotSide, slotSide, COL_BG);
            drawHDArt(*_tft, restArt, uiround::CX - HD_HOME_PX / 2, slotTop,
                      HD_HOME_SRC_PX, HD_HOME_PX);
            drawWornOn(*_tft, stage, uiround::CX - HD_HOME_PX / 2, slotTop,
                       HD_HOME_PX);
            flourishForget();
            _hdDrawnStage   = (int)stage;
            _wornDrawn      = worn;
            _hdDrawnPose    = pose;
            _lastDrawnFrame = -1;
        }
        drawFlourish(*_tft, uiround::CX - HD_HOME_PX / 2, slotTop,
                     HD_HOME_PX, millis());
        return;
#endif
    }

    // Hungry: the HD still, wearing whatever it is wearing. Same slot and same
    // cache key as the resting portrait, so feeding the pet repaints it.
    if (pose >= 0) {
        const uint16_t worn = wornSignature();
        if (_hdDrawnStage != (int)stage || _wornDrawn != worn ||
            _hdDrawnPose != pose) {
            _tft->fillRect(slotX, slotY, slotSide, slotSide, COL_BG);
            drawHDArt(*_tft, restArt, uiround::CX - HD_HOME_PX / 2, slotTop,
                      HD_HOME_SRC_PX, HD_HOME_PX);
            drawWornOn(*_tft, stage, uiround::CX - HD_HOME_PX / 2, slotTop,
                       HD_HOME_PX);
            flourishForget();
            _hdDrawnStage   = (int)stage;
            _wornDrawn      = worn;
            _hdDrawnPose    = pose;
            _lastDrawnFrame = -1;
        }
        // A posed pet still wears its flourish. The pose is a mood and the
        // flourish is a choice; hiding one because of the other would be the
        // firmware editing what the owner picked.
        drawFlourish(*_tft, uiround::CX - HD_HOME_PX / 2, slotTop,
                     HD_HOME_PX, millis());
        return;
    }

    if (_hdDrawnStage >= 0) {
        clearFlourish(*_tft);
        _tft->fillRect(slotX, slotY, slotSide, slotSide, COL_BG);
        _hdDrawnStage = -1;
#if HEXHOUND_HD_IDLE
        // The resting branch now leaves a real frame index behind instead of
        // always -1, and the pixel animation numbers its frames from 0 too. So
        // without this the first pixel frame after leaving idle could match the
        // last HD frame index, be skipped as "already drawn", and leave the pet
        // missing from a slot that was just cleared to black.
        _lastDrawnFrame = -1;
#endif
    }
    if (frameNow != _lastDrawnFrame) {
        int sprX = uiround::CX - animW / 2;
        int sprY = slotTop + (HD_HOME_PX - animW) / 2;
        _animator.clear(*_tft, sprX, sprY, COL_BG, petScale);
        _animator.draw(*_tft, sprX, sprY, petScale);
        _lastDrawnFrame = frameNow;
    }
}

#endif // HEXHOUND_PANEL_ROUND

void UIHome::drawStatBar(int x, int y, int w, int h, int val,
                          uint16_t color, const char* label, int labelW) {
    // Label
    _tft->setTextColor(COL_TEXT, COL_BG);
    _tft->setCursor(x, y - 1);
    _tft->print(label);

    // Bar background
    int barX = x + labelW;
    _tft->fillRect(barX, y, w, h, COL_BAR_BG);

    // Filled portion
    int filled = (val * w) / 100;
    if (filled > 0) {
        _tft->fillRect(barX, y, filled, h, color);
    }

    // Value text
    _tft->setCursor(barX + w + 3, y - 1);
    _tft->printf("%d", val);
}
