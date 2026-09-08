#include <Arduino.h>
#include <driver/gpio.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft;

static constexpr int TFT_CS_PIN = 4;
static constexpr int TFT_DC_PIN = 2;
static constexpr int TFT_RST_PIN = 1;
static constexpr int TFT_BL_PIN = 38;

static void hardResetPanel() {
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(50);
    digitalWrite(TFT_RST_PIN, LOW);
    delay(50);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(150);
}

static void drawFrame(uint8_t rotation, uint16_t bg, uint16_t fg, const char* label) {
    tft.setRotation(rotation);
    tft.fillScreen(bg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.printf("TFT_eSPI rot=%u", rotation);
    tft.setCursor(4, 18);
    tft.print(label);
    tft.setCursor(4, 32);
    tft.printf("w=%d h=%d", tft.width(), tft.height());
    tft.fillRect(4, tft.height() - 18, 20, 12, TFT_RED);
    tft.fillRect(28, tft.height() - 18, 20, 12, TFT_GREEN);
    tft.fillRect(52, tft.height() - 18, 20, 12, TFT_BLUE);
}

void setup() {
    Serial.begin(115200);
    { unsigned long ws = millis(); while (!Serial && (millis() - ws < 3000)) delay(50); }

    Serial.println();
    Serial.println("========================================");
    Serial.println("[TFT_eSPI TEST] starting");
    Serial.println("========================================");

    gpio_reset_pin(GPIO_NUM_1);
    gpio_reset_pin(GPIO_NUM_2);
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_4);
    gpio_reset_pin(GPIO_NUM_5);
    gpio_reset_pin(GPIO_NUM_38);

    pinMode(TFT_CS_PIN, OUTPUT);
    pinMode(TFT_DC_PIN, OUTPUT);
    pinMode(TFT_RST_PIN, OUTPUT);
    pinMode(TFT_BL_PIN, OUTPUT);

    digitalWrite(TFT_CS_PIN, HIGH);
    digitalWrite(TFT_BL_PIN, LOW);
    Serial.println("[TFT_eSPI TEST] backlight OFF, hard reset panel");

    hardResetPanel();

    Serial.println("[TFT_eSPI TEST] calling tft.init()");
    tft.init();
    Serial.println("[TFT_eSPI TEST] tft.init() returned");

    digitalWrite(TFT_BL_PIN, HIGH);
    Serial.println("[TFT_eSPI TEST] backlight ON");

    drawFrame(1, TFT_RED, TFT_WHITE, "ROT1 RED");
    delay(2000);
    drawFrame(1, TFT_GREEN, TFT_BLACK, "ROT1 GREEN");
    delay(2000);
    drawFrame(1, TFT_BLUE, TFT_WHITE, "ROT1 BLUE");
    delay(2000);
    drawFrame(3, TFT_YELLOW, TFT_BLACK, "ROT3 YELLOW");
    delay(2000);
    drawFrame(3, TFT_MAGENTA, TFT_WHITE, "ROT3 MAGENTA");
    delay(2000);

    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(4, 4);
    tft.print("TFT_eSPI OK?");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, 18);
    tft.print("If visible, lib path works");
    Serial.println("[TFT_eSPI TEST] initial color cycle complete");
}

void loop() {
    static uint32_t tick = 0;
    static bool invert = false;
    tick++;

    invert = !invert;
    tft.invertDisplay(invert);
    tft.setRotation(invert ? 1 : 3);
    tft.fillScreen(invert ? TFT_BLACK : TFT_NAVY);
    tft.setTextColor(invert ? TFT_CYAN : TFT_YELLOW, invert ? TFT_BLACK : TFT_NAVY);
    tft.setCursor(4, 4);
    tft.printf("tick %lu", tick);
    tft.setCursor(4, 18);
    tft.printf("invert=%d rot=%d", invert ? 1 : 0, invert ? 1 : 3);

    digitalWrite(TFT_BL_PIN, HIGH);
    Serial.printf("[TFT_eSPI TEST] tick=%lu invert=%d rot=%d\n",
                  tick, invert ? 1 : 0, invert ? 1 : 3);
    delay(3000);
}
