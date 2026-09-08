// ── HexHound - GUI-keys mission confirm gate unit tests ──────────
//
// Covers UIMissions::longPressShouldExecute(), the two-hold gate in front of
// the two missions that press the host's GUI (Win / Cmd / Super) modifier.
//
// WHY THIS SUITE EXISTS
//
// Seven of the nine missions type text into whatever window the owner already
// had focused. Two do not: mission 4 sends Win+L and mission 6 sends Win+R, so
// running one reaches OUTSIDE that window and changes what the owner's machine
// is doing. On 2026-09-01 mission 4 fired three times in 45 seconds on the
// bench, unintentionally. Item H in docs/open-work.md is the design defect
// behind it, and this gate is the answer to it.
//
// The screenshots in the lane report show what the armed screen LOOKS like on
// each of the four panel families. What no screenshot can show is the state
// machine: which hold runs, which only arms, and every way an arm is supposed
// to go away again. That is what is below.
//
// WHAT IT LINKS AGAINST, AND WHY THAT MATTERS
//
// The REAL src/hal/sim_modules.cpp, so the mission table under test is the
// shipping one. A test that carried its own copy of the table would still pass
// on the day somebody dropped MISSION_GUI_KEYS from mission 4, which is the one
// regression this whole feature has to survive. Only the three HAL singletons
// are faked here, and the display fake is a recorder so the tests can also
// assert that the warning NAMES the key combination.
//
// The suite does NOT include test/test_stubs.h. That header defines its own
// inline millis(), and so does src/hal/tft_compat.h under SIMULATOR_BUILD,
// which ui_missions.h pulls in - two inline definitions in one translation
// unit will not compile. The few macros needed are therefore restated below
// with the same names and the same semantics as the shared header, including
// its quirk that a failing RUN_TEST increments BOTH counters. Only the
// "0 failed" line means anything.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/hal/hal.h"
#include "../../src/ui/ui_missions.h"
#include "../../src/modules/usb_module.h"

// ── Fake HAL ──────────────────────────────────────────────────────────────

// Records every string and character painted since the last fillScreen(), so a
// test can ask what the screen actually SAYS. Clearing on fillScreen is what
// makes `painted` mean "the current screen" rather than "everything ever".
class RecordingDisplay : public HalDisplay {
public:
    std::string painted;
    int         screenFills = 0;

    void reset() { painted.clear(); screenFills = 0; }

    void init() override {}
    void fillScreen(uint16_t) override { painted.clear(); screenFills++; }
    void drawPixel(int, int, uint16_t) override {}
    void writePixel(int, int, uint16_t) override {}
    void fillRect(int, int, int, int, uint16_t) override {}
    void writeFillRect(int, int, int, int, uint16_t) override {}
    void drawRect(int, int, int, int, uint16_t) override {}
    void drawFastHLine(int, int, int, uint16_t) override {}
    void drawFastVLine(int, int, int, uint16_t) override {}
    void drawCircle(int, int, int, uint16_t) override {}
    void fillCircle(int, int, int, uint16_t) override {}
    void drawLine(int, int, int, int, uint16_t) override {}
    void setTextColor(uint16_t, uint16_t) override {}
    void setTextSize(int) override {}
    void setCursor(int, int) override {}
    void print(const char* s) override { if (s) painted += s; }
    void print(char c) override { painted += c; }
    void printf(const char*, ...) override {}
    void setRotation(int) override {}
    void startWrite() override {}
    void endWrite() override {}
    int  width() const override { return SCREEN_W; }
    int  height() const override { return SCREEN_H; }
};

class FakeTime : public HalTime {
public:
    uint32_t now = 100000;   // not 0: proves nothing depends on a zero epoch
    uint32_t millis() override { return now; }
    void delay(uint32_t) override {}
};

class FakeLED : public HalLED {
public:
    void init() override {}
    void setColor(uint8_t, uint8_t, uint8_t) override {}
    void clear() override {}
    void setBrightness(uint8_t) override {}
};

class SilentSerial : public HalSerial {
public:
    void begin(uint32_t) override {}
    void println(const char*) override {}
    void printf(const char*, ...) override {}
};

static RecordingDisplay g_display;
static FakeTime         g_time;
static FakeLED          g_led;
static SilentSerial     g_serial;

HalDisplay& halDisplay() { return g_display; }
HalTime&    halTime()    { return g_time; }
HalLED&     halLED()     { return g_led; }
HalSerial&  halSerial()  { return g_serial; }

// tft_compat.h declares this extern for the simulator; the SDL backend defines
// it. Nothing here is under test through it, so it is defined and left silent.
SimSerialProxy Serial;

// ── The modules under test ────────────────────────────────────
//
// [env:native] sets test_build_src = no and build_src_filter = -<*>, so the
// test runner compiles NO project sources. Every suite has to pull in the
// implementations it needs itself, and this one did not: it included only the
// two headers, so it failed to LINK and had never once run under `pio test`.
//
// They are included HERE, below the fakes, rather than beside the headers at
// the top, because they have to be compiled after the fake HAL singletons and
// Serial above. Under -DUNIT_TEST event_bus.cpp includes no Arduino and no
// simulator header at all, on the assumption that the suite already provided
// Serial; that assumption only holds further down the file.
//
// sim_modules.cpp is the one that carries the weight. It holds the real
// USBModule::_missions[] table, so dropping MISSION_GUI_KEYS from mission 4
// fails these tests instead of quietly passing them against a private copy.
#include "../../src/ui/ui_missions.cpp"
#include "../../src/hal/sim_modules.cpp"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.cpp"

// ── Harness ───────────────────────────────────────────────────────────────

static int s_testsPassed = 0;
static int s_testsFailed = 0;

#define TEST(name) static void name()
#define RUN_TEST(name) do { \
    printf("  %-52s ", #name); \
    name(); \
    s_testsPassed++; \
    printf("PASS\n"); \
} while(0)

// ASSERT_* expand to a bare `return`, so they only work inside a void function.
#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true: %s\n", __FILE__, __LINE__, #x); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false: %s\n", __FILE__, __LINE__, #x); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %d != %d\n", __FILE__, __LINE__, (int)_a, (int)_b); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_CONTAINS(hay, needle) do { \
    if (std::string(hay).find(needle) == std::string::npos) { \
        printf("FAIL\n    %s:%d: %s not found in \"%s\"\n", \
               __FILE__, __LINE__, needle, std::string(hay).c_str()); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_LACKS(hay, needle) do { \
    if (std::string(hay).find(needle) != std::string::npos) { \
        printf("FAIL\n    %s:%d: %s unexpectedly present in \"%s\"\n", \
               __FILE__, __LINE__, needle, std::string(hay).c_str()); \
        s_testsFailed++; return; \
    } \
} while(0)

// ── Fixture ───────────────────────────────────────────────────────────────

// Mission indices in the shipping table. Asserted, not assumed - see
// test_the_table_still_marks_exactly_the_two_win_missions.
static const int M_TEXT_ONLY    = 0;   // Password Tip, MISSION_NORMAL
static const int M_LOCK_SCREEN  = 4;   // Win+L,  HIGH_MISCHIEF | GUI_KEYS
static const int M_CHECKLIST    = 5;   // Sec Checklist, MISSION_NORMAL
static const int M_MAP_LINK     = 6;   // Win+R,  GUI_KEYS only
static const int M_ASSESSMENT   = 7;   // HIGH_MISCHIEF only, types text

static TFT_eSPI g_tft;

// Every test starts from the same place: a fresh screen, a known clock, and a
// disarmed gate. drawBriefing() is the disarm point, which is also the thing
// under test in a couple of cases below, so it is called explicitly rather
// than being what "fresh" quietly means.
static UIMissions& fresh() {
    UIMissions& ui = UIMissions::instance();
    ui.init(&g_tft);
    g_time.now = 100000;
    ui.drawBriefing(M_TEXT_ONLY);
    g_display.reset();
    return ui;
}

// ── Tests ─────────────────────────────────────────────────────────────────

// The premise of the whole gate. If this fails, nothing else in the suite is
// testing what it claims to be testing.
TEST(test_the_table_still_marks_exactly_the_two_win_missions) {
    USBModule& usb = USBModule::instance();
    for (int i = 0; i < usb.missionCount(); i++) {
        const Mission& m = usb.getMission((uint8_t)i);
        const bool gui = (m.flags & MISSION_GUI_KEYS) != 0;
        const bool expected = (i == M_LOCK_SCREEN || i == M_MAP_LINK);
        if (gui != expected) {
            printf("FAIL\n    mission %d flags=0x%02X gui=%d expected=%d\n",
                   i, m.flags, (int)gui, (int)expected);
            s_testsFailed++;
            return;
        }
    }
}

// Every mission must brief with real text.
//
// MISSION_DESC[] had eight entries against nine missions, so `Term Tip` briefed
// as "No description." on every board from the day it was added. The bound is
// now derived from the array and a static_assert ties that to MAX_MISSIONS, so
// the count cannot drift again. This is the other half: the assert proves there
// are nine STRINGS, not that any of them says anything. Driven through the real
// drawBriefing() so it checks what actually reaches the glass.
TEST(test_every_mission_briefs_with_a_real_description) {
    UIMissions& ui = fresh();
    for (int i = 0; i < USBModule::instance().missionCount(); i++) {
        g_display.reset();
        ui.drawBriefing(i);
        if (g_display.painted.find("No description.") != std::string::npos) {
            printf("FAIL\n    mission %d has no briefing text\n", i);
            s_testsFailed++;
            return;
        }
    }
}

// GUI keys and high mischief are DIFFERENT bits and the two sets deliberately
// do not coincide. Gating on the wrong one would have confirmed mission 7 and
// left Map Link on a single hold.
TEST(test_gui_keys_is_not_the_same_bit_as_high_mischief) {
    USBModule& usb = USBModule::instance();
    const Mission& mapLink = usb.getMission(M_MAP_LINK);
    const Mission& note    = usb.getMission(M_ASSESSMENT);
    ASSERT_TRUE(mapLink.flags & MISSION_GUI_KEYS);
    ASSERT_FALSE(mapLink.flags & MISSION_HIGH_MISCHIEF);
    ASSERT_TRUE(note.flags & MISSION_HIGH_MISCHIEF);
    ASSERT_FALSE(note.flags & MISSION_GUI_KEYS);
}

// The majority path, and the one a regression would hurt most.
TEST(test_text_only_mission_runs_on_the_first_hold) {
    UIMissions& ui = fresh();
    ASSERT_TRUE(ui.longPressShouldExecute(M_TEXT_ONLY));
    ASSERT_FALSE(ui.isArmed());
}

// ... and does not repaint. A text-only mission must cost nothing extra: no
// arm, no frame, no second hold.
TEST(test_text_only_mission_does_not_repaint) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_TEXT_ONLY);
    ASSERT_EQ(g_display.screenFills, 0);
}

TEST(test_high_mischief_text_mission_still_runs_on_the_first_hold) {
    UIMissions& ui = fresh();
    ASSERT_TRUE(ui.longPressShouldExecute(M_ASSESSMENT));
    ASSERT_FALSE(ui.isArmed());
}

TEST(test_lock_screen_does_not_run_on_the_first_hold) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ASSERT_TRUE(ui.isArmed());
}

TEST(test_map_link_does_not_run_on_the_first_hold) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(M_MAP_LINK));
    ASSERT_TRUE(ui.isArmed());
}

// The warning has to be specific enough that an owner who did not intend this
// can recognise it and stop. "This mission is dangerous" would not be.
TEST(test_lock_screen_warning_names_win_l) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    ASSERT_CONTAINS(g_display.painted, "WIN+L");
    ASSERT_CONTAINS(g_display.painted, "HOLD AGAIN");
}

TEST(test_map_link_warning_names_win_r_and_not_win_l) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_MAP_LINK);
    ASSERT_CONTAINS(g_display.painted, "WIN+R");
    ASSERT_LACKS(g_display.painted, "WIN+L");
}

TEST(test_second_hold_runs_it) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    g_time.now += 1500;
    ASSERT_TRUE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ASSERT_FALSE(ui.isArmed());
}

// One arm buys exactly one run. A third hold has to arm again rather than
// running a second time off the same confirmation.
TEST(test_one_arm_buys_only_one_run) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 1500;
    ASSERT_TRUE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// The wiring guard: two calls for one physical press must not add up to a
// confirmation. Same millis() is the worst case of that.
TEST(test_two_calls_in_the_same_instant_do_not_confirm) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

TEST(test_confirming_too_soon_rearms_instead_of_running) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 299;                       // just under ARM_MIN_CONFIRM_MS
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

TEST(test_confirming_at_the_minimum_delay_runs) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 300;                       // exactly ARM_MIN_CONFIRM_MS
    ASSERT_TRUE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

TEST(test_confirming_at_the_window_edge_runs) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 10000;                     // exactly ARM_WINDOW_MS
    ASSERT_TRUE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// The pocket case: an arm left behind must not still be live later.
TEST(test_the_arm_expires) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 10001;                     // one past ARM_WINDOW_MS
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// An expired arm re-arms rather than being simply dropped, so the screen the
// owner is looking at and the state the gate is in cannot disagree.
TEST(test_an_expired_arm_rearms_and_repaints) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 60000;
    g_display.reset();
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ASSERT_TRUE(ui.isArmed());
    ASSERT_EQ(g_display.screenFills, 1);
    ASSERT_CONTAINS(g_display.painted, "WIN+L");
}

// Item H one screen further in: an arm on one mission must never confirm a
// different one. _armed stores the index for exactly this.
TEST(test_arming_lock_screen_does_not_confirm_map_link) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_MAP_LINK));
    g_time.now += 1500;
    ASSERT_TRUE(ui.longPressShouldExecute(M_MAP_LINK));
}

// Moving to a text-only mission must not leave an arm waiting behind it.
TEST(test_a_text_only_mission_clears_a_standing_arm) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    g_time.now += 1500;
    ASSERT_TRUE(ui.longPressShouldExecute(M_CHECKLIST));
    ASSERT_FALSE(ui.isArmed());
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// The reason the gate needs only ONE line in main.cpp: leaving the brief
// screen and coming back disarms, with nothing for the call site to remember.
TEST(test_reentering_the_brief_screen_disarms) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    ASSERT_TRUE(ui.isArmed());
    ui.drawBriefing(M_LOCK_SCREEN);
    ASSERT_FALSE(ui.isArmed());
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

TEST(test_going_back_to_the_mission_list_disarms) {
    UIMissions& ui = fresh();
    ui.longPressShouldExecute(M_LOCK_SCREEN);
    ui.open();
    ASSERT_FALSE(ui.isArmed());
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// Fails closed. getMission() clamps a bad index to mission 0, so asking it
// would answer about a mission nobody selected.
TEST(test_an_out_of_range_mission_never_runs) {
    UIMissions& ui = fresh();
    ASSERT_FALSE(ui.longPressShouldExecute(-1));
    ASSERT_FALSE(ui.longPressShouldExecute(USBModule::instance().missionCount()));
    ASSERT_FALSE(ui.longPressShouldExecute(9999));
    ASSERT_FALSE(ui.isArmed());
}

// Correct across the 49-day millis() wrap, not just inside one window per
// boot. Unsigned subtraction is what makes this hold.
TEST(test_the_window_survives_the_millis_wrap) {
    UIMissions& ui = UIMissions::instance();
    ui.init(&g_tft);
    g_time.now = 0xFFFFFF00u;                // 256 ms before the wrap
    ui.drawBriefing(M_TEXT_ONLY);
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    g_time.now += 1000;                      // wrapped past zero
    ASSERT_TRUE(ui.longPressShouldExecute(M_LOCK_SCREEN));
}

// No display means the warning was never on the glass, so nobody consented.
// It must refuse AND must not leave an arm that a second hold could cash in.
TEST(test_with_no_display_a_gui_mission_never_runs) {
    UIMissions& ui = fresh();
    ui.init(nullptr);
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ASSERT_FALSE(ui.isArmed());
    g_time.now += 1500;
    ASSERT_FALSE(ui.longPressShouldExecute(M_LOCK_SCREEN));
    ui.init(&g_tft);
}

// -- The backstop, not the gate --------------------------------------------
//
// Everything above tests the ASKING. These test the REFUSING, which is a
// separate mechanism guarding a separate failure. The gate above is one line in
// main.cpp, and deleting that line is silent: the build stays green, every case
// above still passes, and both Win missions go back to running on a single
// hold. So executeMission() takes a GuiKeyConsent with no default value and
// refuses a GUI-keys mission that does not carry one.
//
// These cases are what stops that backstop being quietly removed in its turn.

TEST(test_a_gui_mission_is_refused_without_consent) {
    auto& usb = USBModule::instance();
    ASSERT_TRUE(!usb.executeMission(4, GuiKeyConsent::NotGiven));
    ASSERT_TRUE(!usb.executeMission(6, GuiKeyConsent::NotGiven));
}

TEST(test_a_gui_mission_runs_with_consent) {
    auto& usb = USBModule::instance();
    ASSERT_TRUE(usb.executeMission(4, GuiKeyConsent::ConfirmedOnGlass));
    ASSERT_TRUE(usb.executeMission(6, GuiKeyConsent::ConfirmedOnGlass));
}

// The backstop must not become a second confirm step for the seven missions
// that only type text. Their path is unchanged, consent or not.
TEST(test_a_text_only_mission_is_unaffected_by_consent) {
    auto& usb = USBModule::instance();
    ASSERT_TRUE(usb.executeMission(0, GuiKeyConsent::NotGiven));
    ASSERT_TRUE(usb.executeMission(7, GuiKeyConsent::NotGiven));
    ASSERT_TRUE(usb.executeMission(0, GuiKeyConsent::ConfirmedOnGlass));
}

// NotGiven must be the zero value, so that a caller who zero-initialises or
// forgets a field lands on the refusing side rather than the running one.
TEST(test_consent_defaults_to_the_safe_value) {
    ASSERT_EQ((int)GuiKeyConsent::NotGiven, 0);
    GuiKeyConsent zeroed{};
    ASSERT_TRUE(zeroed != GuiKeyConsent::ConfirmedOnGlass);
}

int main() {
    printf("\n=== HexHound GUI-keys confirm gate tests ===\n\n");

    RUN_TEST(test_the_table_still_marks_exactly_the_two_win_missions);
    RUN_TEST(test_every_mission_briefs_with_a_real_description);
    RUN_TEST(test_gui_keys_is_not_the_same_bit_as_high_mischief);
    RUN_TEST(test_text_only_mission_runs_on_the_first_hold);
    RUN_TEST(test_text_only_mission_does_not_repaint);
    RUN_TEST(test_high_mischief_text_mission_still_runs_on_the_first_hold);
    RUN_TEST(test_lock_screen_does_not_run_on_the_first_hold);
    RUN_TEST(test_map_link_does_not_run_on_the_first_hold);
    RUN_TEST(test_lock_screen_warning_names_win_l);
    RUN_TEST(test_map_link_warning_names_win_r_and_not_win_l);
    RUN_TEST(test_second_hold_runs_it);
    RUN_TEST(test_one_arm_buys_only_one_run);
    RUN_TEST(test_two_calls_in_the_same_instant_do_not_confirm);
    RUN_TEST(test_confirming_too_soon_rearms_instead_of_running);
    RUN_TEST(test_confirming_at_the_minimum_delay_runs);
    RUN_TEST(test_confirming_at_the_window_edge_runs);
    RUN_TEST(test_the_arm_expires);
    RUN_TEST(test_an_expired_arm_rearms_and_repaints);
    RUN_TEST(test_arming_lock_screen_does_not_confirm_map_link);
    RUN_TEST(test_a_text_only_mission_clears_a_standing_arm);
    RUN_TEST(test_reentering_the_brief_screen_disarms);
    RUN_TEST(test_going_back_to_the_mission_list_disarms);
    RUN_TEST(test_an_out_of_range_mission_never_runs);
    RUN_TEST(test_the_window_survives_the_millis_wrap);
    RUN_TEST(test_with_no_display_a_gui_mission_never_runs);

    RUN_TEST(test_a_gui_mission_is_refused_without_consent);
    RUN_TEST(test_a_gui_mission_runs_with_consent);
    RUN_TEST(test_a_text_only_mission_is_unaffected_by_consent);
    RUN_TEST(test_consent_defaults_to_the_safe_value);

    printf("\n=== %d passed, %d failed ===\n", s_testsPassed, s_testsFailed);
    return s_testsFailed ? 1 : 0;
}
