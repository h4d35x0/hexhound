// ── HexHound - LilyGo T-RGB panel driver ─────────────────────────
//
// See rgb_panel_trgb.h. The pin map and init table below were transcribed from
// LilyGo's own driver and then confirmed on hardware by the bring-up sketch in
// src/diagnostics/trgb_bringup.cpp.
//
// There is deliberately no dependency on SensorLib here. LilyGo's driver calls
// extension.transfer9()/beginSPI() on its ExtensionIOXL9555 and pins SensorLib
// to 0.2.3 with an #error against anything newer, but neither method exists in
// v0.2.3 or in any later tag through v0.4.1. The XL9535 is PCA9535-compatible
// and the driver it needs is the sixty lines below.

#if defined(HEXHOUND_RGB_PANEL)

#include "rgb_panel_trgb.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_sleep.h>      // ext0 wakeup + deep sleep for the config soft power off
#include <driver/rtc_io.h>  // RTC-domain pullup that survives deep sleep

#include "../board/board_profile.h"   // PIN_BUTTON

namespace trgb {
namespace {

constexpr int PIN_BL    = 46;
constexpr int PIN_HSYNC = 47;
constexpr int PIN_VSYNC = 41;
constexpr int PIN_DE    = 45;
constexpr int PIN_PCLK  = 42;

constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 48;

// Expander port-0 bit positions, not GPIOs.
constexpr uint8_t XIO_TP_RESET     = 1;
constexpr uint8_t XIO_POWER_ENABLE = 2;
constexpr uint8_t XIO_LCD_CS       = 3;
constexpr uint8_t XIO_LCD_MOSI     = 4;
constexpr uint8_t XIO_LCD_SCLK     = 5;
constexpr uint8_t XIO_LCD_RST      = 6;
constexpr uint8_t XIO_SDMMC_CS     = 7;

// The panel is physically RGB666; the S3 drives the low 16 lines as RGB565.
// Index 0 is the LSB of the framebuffer's blue field, index 15 the MSB of red.
// The apparent red/blue lane swap is deliberate and comes from LilyGo: the
// glass is wired BGR and this map compensates, which is why "RGB order" is the
// correct setting despite the ordering looking inverted.
const int kDataMap[16] = {
    7, 6, 5, 3, 2,            // DATA13..DATA17
    14, 13, 12, 11, 10, 9,    // DATA6..DATA11
    21, 18, 17, 16, 15        // DATA1..DATA5
};

struct InitCmd {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;   // low 5 bits = length, bit 7 = delay 100 ms, 0xFF ends
};

const InitCmd kInit[] = {
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 0x05},
    {0xC0, {0x3b, 0x00}, 0x02},
    {0xC1, {0x0b, 0x02}, 0x02},
    {0xC2, {0x07, 0x02}, 0x02},
    {0xCC, {0x10}, 0x01},
    {0xCD, {0x08}, 0x01},
    {0xb0, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}, 0x10},
    {0xb1, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2a, 0x31, 0x18}, 0x10},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 0x05},
    {0xb0, {0x6d}, 0x01},
    {0xb1, {0x37}, 0x01},
    {0xb2, {0x81}, 0x01},
    {0xb3, {0x80}, 0x01},
    {0xb5, {0x43}, 0x01},
    {0xb7, {0x85}, 0x01},
    {0xb8, {0x20}, 0x01},
    {0xc1, {0x78}, 0x01},
    {0xc2, {0x78}, 0x01},
    {0xc3, {0x8c}, 0x01},
    {0xd0, {0x88}, 0x01},
    {0xe0, {0x00, 0x00, 0x02}, 0x03},
    {0xe1, {0x03, 0xa0, 0x00, 0x00, 0x04, 0xa0, 0x00, 0x00, 0x00, 0x20, 0x20}, 0x0b},
    {0xe2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x0d},
    {0xe3, {0x00, 0x00, 0x11, 0x00}, 0x04},
    {0xe4, {0x22, 0x00}, 0x02},
    {0xe5, {0x05, 0xec, 0xa0, 0xa0, 0x07, 0xee, 0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x10},
    {0xe6, {0x00, 0x00, 0x11, 0x00}, 0x04},
    {0xe7, {0x22, 0x00}, 0x02},
    {0xe8, {0x06, 0xed, 0xa0, 0xa0, 0x08, 0xef, 0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x10},
    {0xeb, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}, 0x07},
    {0xed, {0xff, 0xff, 0xff, 0xba, 0x0a, 0xbf, 0x45, 0xff, 0xff, 0x54, 0xfb, 0xa0, 0xab, 0xff, 0xff, 0xff}, 0x10},
    {0xef, {0x10, 0x0d, 0x04, 0x08, 0x3f, 0x1f}, 0x06},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 0x05},
    {0xef, {0x08}, 0x01},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 0x05},
    {0x36, {0x08}, 0x01},
    {0x3a, {0x66}, 0x01},
    {0x11, {0x00}, 0x80},
    {0x29, {0x00}, 0x80},
    {0, {0}, 0xff}
};

// ── XL9535 expander (PCA9535-compatible). Port 0 only; every pin used is 1..7.

constexpr uint8_t REG_OUTPUT0 = 0x02;
constexpr uint8_t REG_CONFIG0 = 0x06;

uint8_t s_addr   = 0;
uint8_t s_output = 0xFF;
uint8_t s_config = 0xFF;   // 1 = input, the power-on default

esp_lcd_panel_handle_t s_panel = nullptr;

bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool expanderBegin() {
    for (uint8_t a = 0x20; a <= 0x27; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            s_addr = a;
            return true;
        }
    }
    return false;
}

void pinModeOutput(uint8_t pin) {
    s_config &= ~(uint8_t)(1u << pin);
    writeReg(REG_CONFIG0, s_config);
}

inline void stage(uint8_t pin, bool level) {
    if (level) {
        s_output |= (uint8_t)(1u << pin);
    } else {
        s_output &= ~(uint8_t)(1u << pin);
    }
}

inline void flush() { writeReg(REG_OUTPUT0, s_output); }

void writePin(uint8_t pin, bool level) {
    stage(pin, level);
    flush();
}

// 9-bit SPI word, MSB first; bit 8 is the data/command flag (0 = command).
// Staging MOSI together with the falling clock edge keeps this to two bus
// transactions per bit instead of three, which is what makes the whole init
// table take ~530 ms instead of the ~1833 ms LilyGo measure.
void transfer9(uint16_t word) {
    writePin(XIO_LCD_CS, false);
    for (int i = 8; i >= 0; --i) {
        stage(XIO_LCD_MOSI, (word >> i) & 1u);
        stage(XIO_LCD_SCLK, false);
        flush();
        writePin(XIO_LCD_SCLK, true);
    }
    writePin(XIO_LCD_CS, true);
}

}  // namespace

// Free a slave that is still driving SDA from a transfer that was cut off.
//
// A WARM reset does not reset the FT3267 or the XL9535: esptool's RTS pulse
// after a flash, and the reset button, both restart the S3 while leaving every
// I2C slave exactly as it was. If one was mid-byte when the CPU restarted it
// carries on holding SDA low, and from then on every START the master issues is
// corrupted. That is not something a repeated START can clear - the documented
// recovery is to clock SCL until the slave finishes the byte it thinks it is
// sending and releases SDA, then issue a STOP.
//
// HONESTY NOTE, 2026-08-29: this has never actually fired. It was written for
// a high first-read failure rate on the FT3267 that looked like a stuck slave,
// and that turned out to be something else entirely - the controller enters a
// low-power monitor state about 30 s after boot and stops answering the first
// read, which a retry in TouchModule handles. Measured across every boot since:
// zero recoveries.
//
// It is kept because it is the standard, cheap remedy for a genuinely wedged
// bus, it runs once at boot, and nothing else in this firmware can clear that
// condition if it ever happens. Do not cite it as the fix for the touch read
// failures; it is insurance against a different fault that has not been
// observed here.
static void i2cBusRecover() {
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, OUTPUT);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);

    int pulses = 0;
    while (digitalRead(PIN_I2C_SDA) == LOW && pulses < 9) {
        digitalWrite(PIN_I2C_SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(PIN_I2C_SCL, HIGH);
        delayMicroseconds(5);
        pulses++;
    }

    // STOP condition: SDA released while SCL is high.
    pinMode(PIN_I2C_SDA, OUTPUT);
    digitalWrite(PIN_I2C_SDA, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SDA, HIGH);
    delayMicroseconds(5);

    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);

    if (pulses > 0) {
        Serial.printf("[TRGB] I2C bus was held low, freed after %d clock pulses\n",
                      pulses);
    }
}

bool begin() {
#if HEXHOUND_HAS_SOFT_SLEEP
    // ── Release whatever enterDeepSleep() parked ──────────────────────────
    //
    // FIRST, before anything else touches these pads, and unconditionally
    // rather than only on a deep-sleep wake: a pad hold lives in RTC_CNTL and
    // survives the core reset that a wake performs, so it is cleared only by a
    // power-on reset or by being released here. The T-Display S3 has already
    // paid for getting this wrong - it held its panel rail low for sleep, never
    // released it, and from then on EVERY boot came up lit and blank, a symptom
    // that looked like it followed "sleep or reset" at random. See the hold
    // release beside PIN_TFT_POWER in main.cpp.
    //
    // GPIO46 is a digital pad, so only the gpio_* pair applies to it; GPIO0 is
    // an RTC pad that esp_sleep's own ext0 preparation puts into RTC input mode
    // and holds, so it needs the rtc_gpio_* pair and a return to digital GPIO
    // before main.cpp's digitalRead() can see the button again. Clearing the
    // wrong register set leaves the pin stuck, which is why both are here.
    //
    // Every call below is a no-op on a cold boot.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PIN_BL);
    rtc_gpio_hold_dis((gpio_num_t)PIN_BUTTON);
    rtc_gpio_deinit((gpio_num_t)PIN_BUTTON);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
#endif

    // Before the peripheral takes the pins: a slave left holding SDA from
    // before the reset has to be clocked out first, or Wire will never get a
    // clean START.
    i2cBusRecover();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000UL);

    if (!expanderBegin()) {
        Serial.println("[TRGB] FATAL: no XL9535 expander on 0x20..0x27");
        return false;
    }
    Serial.printf("[TRGB] XL9535 at 0x%02X\n", s_addr);

    for (uint8_t pin : {XIO_POWER_ENABLE, XIO_LCD_CS, XIO_LCD_MOSI,
                        XIO_LCD_SCLK, XIO_LCD_RST, XIO_TP_RESET, XIO_SDMMC_CS}) {
        pinModeOutput(pin);
    }
    writePin(XIO_SDMMC_CS, true);
    writePin(XIO_LCD_CS, true);
    writePin(XIO_POWER_ENABLE, true);

    // Release the touch controller from reset here rather than in the touch
    // module: its reset line is on this expander, and FocalTech parts need
    // roughly 300 ms afterwards before their registers read anything but zero.
    // Doing it now means that delay overlaps the panel init instead of adding
    // to boot time.
    writePin(XIO_TP_RESET, false);
    delay(30);
    writePin(XIO_TP_RESET, true);

    writePin(XIO_LCD_RST, false);
    delay(20);
    writePin(XIO_LCD_RST, true);
    delay(10);

    Wire.setClock(1000000UL);
    for (int i = 0; kInit[i].databytes != 0xFF; ++i) {
        transfer9(kInit[i].cmd);
        const int len = kInit[i].databytes & 0x1F;
        for (int b = 0; b < len; ++b) {
            transfer9((uint16_t)kInit[i].data[b] | 0x100u);
        }
        if (kInit[i].databytes & 0x80) delay(100);
    }
    // Touch cannot be addressed above 400 kHz, so drop back before it is used.
    Wire.setClock(400000UL);

    // Field-by-field rather than a designated initialiser: the struct layout
    // differs between Arduino core 2.x and 3.x.
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src                       = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz               = 8000000UL;
    cfg.timings.h_res                 = PANEL_W;
    cfg.timings.v_res                 = PANEL_H;
    cfg.timings.hsync_pulse_width     = 1;
    cfg.timings.hsync_back_porch      = 30;
    cfg.timings.hsync_front_porch     = 50;
    cfg.timings.vsync_pulse_width     = 1;
    cfg.timings.vsync_back_porch      = 30;
    cfg.timings.vsync_front_porch     = 20;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.data_width        = 16;
    cfg.psram_trans_align = 64;
    cfg.hsync_gpio_num    = PIN_HSYNC;
    cfg.vsync_gpio_num    = PIN_VSYNC;
    cfg.de_gpio_num       = PIN_DE;
    cfg.pclk_gpio_num     = PIN_PCLK;
    cfg.disp_gpio_num     = GPIO_NUM_NC;
    memcpy(cfg.data_gpio_nums, kDataMap, sizeof(cfg.data_gpio_nums));
    cfg.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        Serial.printf("[TRGB] FATAL: esp_lcd_new_rgb_panel: %s\n", esp_err_to_name(err));
        s_panel = nullptr;
        return false;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        Serial.printf("[TRGB] FATAL: esp_lcd_panel_init: %s\n", esp_err_to_name(err));
        s_panel = nullptr;
        return false;
    }
    Serial.println("[TRGB] RGB panel running");
    return true;
}

void blitRows(int y0, int rows, const uint16_t* src) {
    if (!s_panel || !src || rows <= 0) return;
    if (y0 < 0) {
        src  += (size_t)(-y0) * PANEL_W;
        rows += y0;
        y0    = 0;
    }
    if (y0 >= PANEL_H) return;
    if (y0 + rows > PANEL_H) rows = PANEL_H - y0;
    if (rows <= 0) return;

    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, PANEL_W, y0 + rows, src);
}

void backlightOn() {
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, LOW);
    delay(4);
    digitalWrite(PIN_BL, HIGH);
    delayMicroseconds(30);
}

void backlightOff() {
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, LOW);
}

uint8_t expanderAddress() { return s_addr; }

// ── Soft power off ────────────────────────────────────────────────────────
//
// The T-Display S3's softPowerOff() in main.cpp is the reference, and THREE of
// its load-bearing assumptions are false on this board. Each one is answered
// here rather than copied.
//
// 1. "Cut the panel's power rail and gpio_hold_en it."
//    There is no panel-rail GPIO here. The rail enable is expander pin IO2, on
//    the far side of an I2C bus, and no RTC mechanism can hold an I2C slave.
//
//    IO2 IS ALSO DELIBERATELY LEFT HIGH, and that is not caution, it is what
//    the vendor does. LilyGo's own LilyGo_RGBPanel::sleep() for this board -
//    the one behind their examples/DeepSleep - dims the backlight, sleeps the
//    touch controller, arms a wake source, deletes the esp_lcd panel, ends Wire
//    and Serial, and never touches power_enable. Their driver's only comment on
//    that pin, at the one place it is written, is:
//
//        The power enable is connected to the XL9555 expansion chip GPIO.
//        It must be turned on and can only be started when using a battery.
//
//    That reads as a battery power HOLD for the board, not a panel rail. Our
//    own bring-up note said the softer version of the same thing - without IO2
//    high "the board is dead on battery but fine on USB" - and those two
//    sentences together settle which way to resolve it: dropping IO2 on battery
//    plausibly removes the CPU that was supposed to wake up, and the only
//    recovery would be a USB cable. A few microamps is the cheaper mistake.
//
//    Worth saying plainly, because it is a real product option and not a bug:
//    if a TRUE zero-drain power off is ever wanted, dropping IO2 on battery is
//    probably exactly that. It would be a deliberate feature with a deliberate
//    recovery story, not a tweak to this routine. See LANE_C_CALLSITE.md.
//
//    What replaces the rail cut is BETTER than a GPIO hold, and it is the one
//    genuine advantage this board's expander gives back: the XL9535's output
//    register is a latch inside a chip that stays powered while the S3 sleeps.
//    Driving XIO_LCD_RST low is therefore held for the whole sleep, with no
//    RTC involvement at all, and an ST7701S held in reset is dark by
//    construction - its source drivers are off and no command can turn them on.
//
//    The graceful sequence (0x28 display off, 0x10 sleep in) is NOT sent, on
//    purpose. transfer9() costs about twenty I2C transactions per word, and if
//    the bus were wedged each one burns Wire's timeout. That turns "asked it to
//    sleep" into seconds of apparent hang, which is the exact false-hang this
//    project has already chased twice. One writePin() is bounded by a single
//    timeout and reaches the same dark panel.
//
// 2. "Wake on ext0 from PIN_BUTTON_2, because GPIO14 is RTC-capable and
//    button A is not."
//    That reason is simply wrong, and it is worth saying so because it is the
//    sentence that would stop someone porting this. The ESP32-S3's RTC pads are
//    GPIO0..GPIO21 - the same range that comment itself quotes - and the
//    T-Display's button A is GPIO0, inside it. Button B was not the only
//    RTC-capable choice, it was the ergonomic one. This board's only button is
//    GPIO0 and it is a perfectly good ext0 source.
//
//    LilyGo agree: their sleep() offers exactly three wake sources on this
//    board and the button one is esp_sleep_enable_ext1_wakeup(_BV(0), ANY_LOW),
//    commented "Use BUTTON 0 (BOOT) button to wake up". ext0 is used here
//    instead of ext1 only because it is what the T-Display path in this
//    codebase already uses and it configures the RTC pull-up itself.
//
//    Touch would be the nicer gesture on a board whose primary input is a
//    finger, and GPIO1 carries the FT3267's interrupt and is equally
//    RTC-capable; LilyGo support it. It is NOT the wake source here because
//    their own touch path has to wait for the finger to lift and then sit for a
//    further two seconds "for the interrupt level to stabilize" before arming.
//    An INT that is still low when the board sleeps wakes it immediately, which
//    presents, once again, as sleep doing nothing. A button whose idle level is
//    known beats a gesture whose idle level needs a two-second settling ritual
//    that has never been tested here.
//
// 3. "The entry gesture is hold both buttons."
//    There is no second button. Entry is a CONFIG row committed with a hold;
//    see UIConfig::onLongPress().
//
// Deliberately no Serial.flush(), for the reason main.cpp records: on a
// cdc_on_boot board flush() waits on the USB host and has run to tens of
// seconds. platformio.ini sets board_build.cdc_on_boot = 1 for this env.
void enterDeepSleep() {
    // Backlight first, so the panel reset below is not a visible flash.
    //
    // GPIO46 is a DIGITAL pad, outside the RTC range, and the sibling comment
    // in main.cpp says of its own GPIO38 that such a pin "cannot be held and
    // its level is undefined once the CPU stops". That is not true on the S3.
    // Digital pads GPIO26..GPIO48 have their own hold bits in RTC_CNTL, armed
    // per pin by gpio_hold_en() and switched on for sleep by the global
    // gpio_deep_sleep_hold_en(); ESP-IDF's own gpio.h says in as many words
    // that gpio_hold_en() alone will NOT hold a digital pad through deep sleep
    // and that the deep-sleep call is the one that does it. Both are needed,
    // and both are here.
    //
    // Belt and braces underneath it: GPIO46 is a strapping pin whose datasheet
    // reset default is a weak PULL-DOWN, so even an unheld pad settles at the
    // level this dimmer reads as off. The hold is what turns "settles off" into
    // "driven off", which matters because a lit blank screen is the one outcome
    // this whole routine exists to avoid.
    backlightOff();
    gpio_hold_en((gpio_num_t)PIN_BL);
    gpio_deep_sleep_hold_en();

    // Stop the RGB peripheral and free its framebuffer before the panel is put
    // into reset, so the DMA is not still streaming pixels at a controller that
    // has stopped listening. This is LilyGo's own order of operations too.
    //
    // esp_lcd_panel_disp_off() is NOT called alongside it, unlike theirs: this
    // driver configures disp_gpio_num as GPIO_NUM_NC, so there is no DISP pin
    // for it to act on and the call would only return an error to ignore.
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = nullptr;
    }

    // Hold the panel controller in reset. Latched by the expander, so it
    // survives the CPU stopping. Skipped if the expander was never found,
    // because writePin() to address 0 would just burn bus timeouts.
    //
    // LilyGo do not do this - they leave the ST7701S powered and initialised
    // with nothing clocking it. It is added here because "the backlight is off"
    // and "the panel is off" are different claims, and only the second one
    // survives someone finding the device in a bag and shining a light at it.
    if (s_addr != 0) {
        writePin(XIO_LCD_RST, false);
    }

    // Wake on the BOOT button going low.
    //
    // GPIO0 is RTC-capable: the S3's RTC pads are GPIO0..GPIO21 and the TRM
    // names that exact range. It is also a strapping pin, and the obvious worry
    // is that waking with it held low lands in ROM download mode. It does not.
    // The strap latches sample at CHIP RESET, and the TRM's reset table puts a
    // deep-sleep wake one level below that as a CORE reset, which leaves the
    // RTC domain - and the latch sampled at power-on, when GPIO0 was high -
    // untouched. The exposure that remains is an EN-pin reset, a power cycle or
    // a brownout with the button held, none of which is this path.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);

    // Hold the pull-up in the RTC domain. Without it the pad floats once the
    // digital domain powers down, and a floating input wakes the board
    // immediately, which presents as sleep doing nothing at all.
    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);

    // Never sleep while the button is still down, or the press asking for
    // sleep is also the press that wakes it. Bounded, unlike the original: a
    // stuck-low pin here would otherwise be an infinite loop with a dark
    // screen, and this board's entry gesture is a touch hold, so the button
    // is usually not involved at all.
    const uint32_t start = millis();
    while (digitalRead(PIN_BUTTON) == LOW && (millis() - start) < 3000) {
        delay(20);
    }
    delay(120);   // debounce the release

    Serial.println("[Power] Entering deep sleep. Press the BOOT button to wake.");
    esp_deep_sleep_start();
}

}  // namespace trgb

#endif  // HEXHOUND_RGB_PANEL
