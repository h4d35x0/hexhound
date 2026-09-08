// T-Dongle S3 - Safe serial test using Arduino HWCDC
// Uses Serial.println() with connection guards to avoid boot loops.
// No direct FIFO writes (those can spin forever if no host is reading).
//
// Build: pio run -e serial-test
// Flash: see upload instructions in platformio.ini

#include <Arduino.h>

#define BACKLIGHT_PIN 38

void setup() {
    // Backlight = alive indicator (turns on IMMEDIATELY)
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);

    // Init serial - with USB_MODE=1, Serial is HWCDC (USB-Serial-JTAG)
    Serial.begin(115200);

    // Wait up to 5 seconds for USB host to connect
    // Blink backlight while waiting so we can see it's alive
    unsigned long start = millis();
    bool connected = false;
    while (millis() - start < 5000) {
        if (Serial) {
            connected = true;
            break;
        }
        // Fast blink = waiting for USB
        digitalWrite(BACKLIGHT_PIN, ((millis() / 200) % 2) ? HIGH : LOW);
        delay(50);
    }

    // Solid backlight = setup done
    digitalWrite(BACKLIGHT_PIN, HIGH);

    if (connected) {
        Serial.println();
        Serial.println("========================================");
        Serial.println("=== T-DONGLE S3 SERIAL TEST OK ===");
        Serial.println("========================================");
        Serial.println("HWCDC connected, Serial output working!");
        Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
        Serial.println("========================================");
    }
}

void loop() {
    static uint32_t count = 0;
    count++;

    // Only print if USB host is connected (prevents write hangs)
    if (Serial) {
        Serial.printf("[tick #%lu] %lu ms, heap=%u\n",
                      count, millis(), ESP.getFreeHeap());
    }

    // Heartbeat blink: slow = running, distinguishable from setup fast-blink
    digitalWrite(BACKLIGHT_PIN, (count % 2) ? HIGH : LOW);

    delay(1000);
}
