// T-Dongle S3 - SPI Routing Diagnostic
// Confirmed: bitbang GPIO works (GREEN on screen).
// Issue: hardware SPI peripheral not routing to GPIO 3/5.
// This test:
//   1. Reads GPIO matrix registers to see SPI routing state
//   2. Tests hardware SPI FIRST (no bitbang contamination)
//   3. Falls back to bitbang as visual confirmation
//
// Build: pio run -e tft-test -t clean && pio run -e tft-test
// Flash: pio run -e tft-test -t upload --upload-port <PORT>
// POR (unplug/replug USB), then: pio device monitor -b 115200

#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_struct.h>
#include <soc/spi_reg.h>

// Pin definitions
#define TFT_MOSI_PIN  3
#define TFT_SCLK_PIN  5
#define TFT_CS_PIN    4
#define TFT_DC_PIN    2
#define TFT_RST_PIN   1
#define TFT_BL_PIN    38

// ST7735 commands
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_COLMOD  0x3A
#define ST7735_MADCTL  0x36
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_INVON   0x21
#define ST7735_NORON   0x13
#define ST7735_DISPON  0x29

// ─── GPIO MATRIX DIAGNOSTIC ─────────────────────────────────────────────────
// Read the GPIO output routing register to see what signal is assigned
// GPIO_FUNCn_OUT_SEL_CFG_REG = 0x60004554 + 4*n (ESP32-S3)
// Bits 0-8: output signal index (0x100 = GPIO mode, other = peripheral)

static void dumpGpioRouting(const char* label) {
    Serial.printf("\n[GPIO-MATRIX] %s\n", label);
    int pins[] = {1, 2, 3, 4, 5, 38};
    const char* names[] = {"RST", "DC", "MOSI", "CS", "SCLK", "BL"};
    for (int i = 0; i < 6; i++) {
        int pin = pins[i];
        // Output function select register
        uint32_t out_sel_reg = 0x60004554 + 4 * pin;
        uint32_t out_sel_val = *(volatile uint32_t*)out_sel_reg;
        uint32_t func_sel = out_sel_val & 0x1FF;  // bits 0-8
        bool inv = (out_sel_val >> 9) & 1;         // bit 9: invert
        bool oen_inv = (out_sel_val >> 10) & 1;    // bit 10: oen invert
        bool oen_sel = (out_sel_val >> 11) & 1;    // bit 11: oen select

        // IO_MUX register for this pin
        uint32_t iomux_reg = REG_IO_MUX_BASE + 0x4 + 4 * pin;
        uint32_t iomux_val = *(volatile uint32_t*)iomux_reg;
        uint32_t mcu_sel = (iomux_val >> 12) & 0x7;  // MCU_SEL bits

        Serial.printf("  GPIO%-2d (%s): OUT_SEL=0x%03X (func=%u) INV=%d OEN_INV=%d OEN_SEL=%d IO_MUX_MCU_SEL=%u\n",
                      pin, names[i], func_sel, func_sel, inv, oen_inv, oen_sel, mcu_sel);
    }

    // Also dump SPI2 register state
    Serial.printf("  SPI2_CLK_GATE_REG (0x%08X) = 0x%08X\n",
                  (uint32_t)(DR_REG_SPI2_BASE + 0xE8),
                  *(volatile uint32_t*)(DR_REG_SPI2_BASE + 0xE8));
    Serial.printf("  SPI2_USER_REG  = 0x%08X\n", *(volatile uint32_t*)(DR_REG_SPI2_BASE + 0x10));
    Serial.printf("  SPI2_CTRL_REG  = 0x%08X\n", *(volatile uint32_t*)(DR_REG_SPI2_BASE + 0x08));
    Serial.printf("  SPI2_CLOCK_REG = 0x%08X\n", *(volatile uint32_t*)(DR_REG_SPI2_BASE + 0x0C));
}

// ─── BITBANG SPI ─────────────────────────────────────────────────────────────

static void bb_clockPulse() {
    digitalWrite(TFT_SCLK_PIN, HIGH);
    delayMicroseconds(2);
    digitalWrite(TFT_SCLK_PIN, LOW);
    delayMicroseconds(2);
}

static void bb_sendByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(TFT_MOSI_PIN, (b >> i) & 1);
        bb_clockPulse();
    }
}

static void bb_sendCommand(uint8_t cmd) {
    digitalWrite(TFT_DC_PIN, LOW);
    digitalWrite(TFT_CS_PIN, LOW);
    bb_sendByte(cmd);
    digitalWrite(TFT_CS_PIN, HIGH);
}

static void bb_sendData8(uint8_t data) {
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    bb_sendByte(data);
    digitalWrite(TFT_CS_PIN, HIGH);
}

static void bb_sendCommandWithData(uint8_t cmd, const uint8_t* data, uint8_t len) {
    bb_sendCommand(cmd);
    for (uint8_t i = 0; i < len; i++) {
        bb_sendData8(data[i]);
    }
}

// ─── HARDWARE SPI HELPERS ────────────────────────────────────────────────────

static void hw_sendCommand(uint8_t cmd) {
    digitalWrite(TFT_DC_PIN, LOW);
    digitalWrite(TFT_CS_PIN, LOW);
    SPI.transfer(cmd);
    digitalWrite(TFT_CS_PIN, HIGH);
}

static void hw_sendData8(uint8_t data) {
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    SPI.transfer(data);
    digitalWrite(TFT_CS_PIN, HIGH);
}

static void hw_sendCommandWithData(uint8_t cmd, const uint8_t* data, uint8_t len) {
    hw_sendCommand(cmd);
    for (uint8_t i = 0; i < len; i++) {
        hw_sendData8(data[i]);
    }
}

// ─── ST7735 INIT ─────────────────────────────────────────────────────────────

typedef void (*SendCmdFn)(uint8_t);
typedef void (*SendCmdDataFn)(uint8_t, const uint8_t*, uint8_t);

static void hardwareReset() {
    Serial.println("  [RST] Hardware reset...");
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(50);
    digitalWrite(TFT_RST_PIN, LOW);
    delay(50);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(150);
    Serial.println("  [RST] Done");
}

static void initST7735(SendCmdFn sendCmd, SendCmdDataFn sendCmdData) {
    sendCmd(ST7735_SWRESET);
    delay(150);
    Serial.println("  [INIT] SWRESET");

    sendCmd(ST7735_SLPOUT);
    delay(500);  // extra long delay for display readiness
    Serial.println("  [INIT] SLPOUT (500ms wait)");

    uint8_t colmod = 0x05;
    sendCmdData(ST7735_COLMOD, &colmod, 1);
    Serial.println("  [INIT] COLMOD=0x05");

    uint8_t madctl = 0xC8;
    sendCmdData(ST7735_MADCTL, &madctl, 1);
    Serial.println("  [INIT] MADCTL=0xC8");

    sendCmd(ST7735_INVON);
    Serial.println("  [INIT] INVON");

    sendCmd(ST7735_NORON);
    delay(10);
    sendCmd(ST7735_DISPON);
    delay(100);  // extra delay after DISPON
    Serial.println("  [INIT] DISPON - ready");
}

// ─── FILL HELPERS ────────────────────────────────────────────────────────────

static void setWindow(SendCmdDataFn sendCmdData) {
    uint8_t caset[] = {0x00, 26, 0x00, 105};
    sendCmdData(ST7735_CASET, caset, 4);
    uint8_t raset[] = {0x00, 1, 0x00, 160};
    sendCmdData(ST7735_RASET, raset, 4);
}

// ─── SETUP ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    { unsigned long ws = millis(); while (!Serial && (millis() - ws < 3000)) delay(50); }

    Serial.println("\n================================================================");
    Serial.println("  T-DONGLE S3 - SPI ROUTING DIAGNOSTIC v2");
    Serial.println("  Tests HW SPI first, then bitbang as verification");
    Serial.println("================================================================\n");

    // ─── STEP 0: gpio_reset_pin on ALL TFT pins ─────────────────────────
    Serial.println("[STEP 0] gpio_reset_pin() on all TFT pins...");
    gpio_reset_pin(GPIO_NUM_1);
    gpio_reset_pin(GPIO_NUM_2);
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_4);
    gpio_reset_pin(GPIO_NUM_5);
    gpio_reset_pin(GPIO_NUM_38);
    Serial.println("[STEP 0] Done - all pins in default state");

    dumpGpioRouting("After gpio_reset_pin (before any config)");

    // ─── STEP 1: Configure control pins as GPIO OUTPUT ──────────────────
    Serial.println("\n[STEP 1] Configure control pins...");
    pinMode(TFT_CS_PIN, OUTPUT);
    pinMode(TFT_DC_PIN, OUTPUT);
    pinMode(TFT_RST_PIN, OUTPUT);
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_CS_PIN, HIGH);
    digitalWrite(TFT_BL_PIN, LOW);
    Serial.println("[STEP 1] CS=HIGH, BL=OFF");

    // ─── STEP 2: Hardware reset ─────────────────────────────────────────
    Serial.println("\n[STEP 2] Hardware reset...");
    hardwareReset();

    // ─── STEP 3: Initialize SPI peripheral ──────────────────────────────
    Serial.println("\n[STEP 3] SPI.begin(SCLK=5, MISO=-1, MOSI=3, SS=-1)...");
    SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, -1);
    Serial.println("[STEP 3] SPI.begin() returned");

    dumpGpioRouting("After SPI.begin()");

    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    Serial.println("[STEP 3] beginTransaction(4MHz MODE0) OK");

    dumpGpioRouting("After beginTransaction()");

    // ─── STEP 4: HW SPI - Init display + fill ──────────────────────────
    Serial.println("\n[STEP 4] === HARDWARE SPI TEST ===");
    initST7735(hw_sendCommand, hw_sendCommandWithData);

    // Extra delay before pixel writes
    delay(200);

    // Backlight ON
    digitalWrite(TFT_BL_PIN, HIGH);
    Serial.println("  [BL] ON");

    // Fill CYAN (0x07FF) - bright, visible in any color mode
    Serial.println("  [FILL] CYAN via HW SPI...");
    setWindow(hw_sendCommandWithData);
    hw_sendCommand(ST7735_RAMWR);
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    for (uint32_t i = 0; i < 80UL * 160; i++) {
        SPI.transfer(0x07);
        SPI.transfer(0xFF);
    }
    digitalWrite(TFT_CS_PIN, HIGH);
    Serial.println("  [FILL] CYAN done - LOOK AT SCREEN NOW");
    delay(4000);

    // Fill YELLOW (0xFFE0)
    Serial.println("  [FILL] YELLOW via HW SPI...");
    setWindow(hw_sendCommandWithData);
    hw_sendCommand(ST7735_RAMWR);
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    for (uint32_t i = 0; i < 80UL * 160; i++) {
        SPI.transfer(0xFF);
        SPI.transfer(0xE0);
    }
    digitalWrite(TFT_CS_PIN, HIGH);
    Serial.println("  [FILL] YELLOW done - LOOK AT SCREEN NOW");
    delay(4000);

    SPI.endTransaction();
    SPI.end();
    Serial.println("[STEP 4] SPI ended\n");

    dumpGpioRouting("After SPI.end()");

    // ─── STEP 5: Bitbang test (known working) ───────────────────────────
    Serial.println("\n[STEP 5] === BITBANG VERIFICATION ===");
    Serial.println("Re-resetting MOSI/SCLK pins for bitbang...");
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_5);
    pinMode(TFT_MOSI_PIN, OUTPUT);
    pinMode(TFT_SCLK_PIN, OUTPUT);
    digitalWrite(TFT_SCLK_PIN, LOW);

    // Backlight off briefly
    digitalWrite(TFT_BL_PIN, LOW);
    delay(300);

    hardwareReset();
    initST7735(bb_sendCommand, bb_sendCommandWithData);

    // Extra delay
    delay(500);

    digitalWrite(TFT_BL_PIN, HIGH);
    Serial.println("  [BL] ON");

    // Fill GREEN (known working from previous test)
    Serial.println("  [FILL] GREEN via bitbang...");
    setWindow(bb_sendCommandWithData);
    bb_sendCommand(ST7735_RAMWR);
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    for (uint32_t i = 0; i < 80UL * 160; i++) {
        bb_sendByte(0x07);
        bb_sendByte(0xE0);
    }
    digitalWrite(TFT_CS_PIN, HIGH);
    Serial.println("  [FILL] GREEN done - should be GREEN on screen");
    delay(4000);

    // Fill MAGENTA (0xF81F) - tests both R and B channels
    Serial.println("  [FILL] MAGENTA via bitbang...");
    setWindow(bb_sendCommandWithData);
    bb_sendCommand(ST7735_RAMWR);
    digitalWrite(TFT_DC_PIN, HIGH);
    digitalWrite(TFT_CS_PIN, LOW);
    for (uint32_t i = 0; i < 80UL * 160; i++) {
        bb_sendByte(0xF8);
        bb_sendByte(0x1F);
    }
    digitalWrite(TFT_CS_PIN, HIGH);
    Serial.println("  [FILL] MAGENTA done - should be MAGENTA (or CYAN with BGR)");
    delay(4000);

    dumpGpioRouting("After bitbang phase");

    // ─── STEP 6: Final blinks ───────────────────────────────────────────
    Serial.println("\n[STEP 6] 3 slow blinks - test complete");
    for (int i = 0; i < 3; i++) {
        digitalWrite(TFT_BL_PIN, LOW);
        delay(500);
        digitalWrite(TFT_BL_PIN, HIGH);
        delay(500);
    }

    Serial.println("\n================================================================");
    Serial.println("  DIAGNOSTIC COMPLETE");
    Serial.println("  Step 4 (HW SPI): Did you see CYAN then YELLOW?");
    Serial.println("  Step 5 (Bitbang): Did you see GREEN then MAGENTA?");
    Serial.println("  Check GPIO matrix register dumps above.");
    Serial.println("================================================================");
}

void loop() {
    static uint32_t count = 0;
    count++;
    Serial.printf("[LOOP] tick #%lu, %lu ms\n", count, millis());
    delay(5000);
}
