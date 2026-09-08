#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_st7735.h>

// LilyGo T-Dongle-S3 vendor-baseline screen bring-up.
// This intentionally follows the official factory_screen example's display
// stack and orientation so we can validate the board against known-good code.

namespace {

constexpr gpio_num_t PIN_NUM_MOSI = GPIO_NUM_3;
constexpr gpio_num_t PIN_NUM_CLK = GPIO_NUM_5;
constexpr gpio_num_t PIN_NUM_CS = GPIO_NUM_4;
constexpr gpio_num_t PIN_NUM_DC = GPIO_NUM_2;
constexpr gpio_num_t PIN_NUM_RST = GPIO_NUM_1;
constexpr gpio_num_t PIN_NUM_BCKL = GPIO_NUM_38;

constexpr int LCD_PIXEL_WIDTH = 160;
constexpr int LCD_PIXEL_HEIGHT = 80;
constexpr spi_host_device_t LCD_HOST = SPI2_HOST;

constexpr uint32_t LEDC_BACKLIGHT_FREQ = 1000;
constexpr uint8_t LEDC_BACKLIGHT_BIT_WIDTH = 8;
constexpr uint8_t LEDC_BACKLIGHT_CHANNEL = 3;

esp_lcd_panel_handle_t panel_handle = nullptr;
esp_lcd_panel_io_handle_t io_handle = nullptr;

spi_bus_config_t spi_config = ST7735_PANEL_BUS_SPI_CONFIG(
    PIN_NUM_CLK,
    PIN_NUM_MOSI,
    LCD_PIXEL_WIDTH * LCD_PIXEL_HEIGHT * sizeof(uint16_t)
);

esp_lcd_panel_io_spi_config_t io_config = ST7735_PANEL_IO_SPI_CONFIG(
    PIN_NUM_CS,
    PIN_NUM_DC,
    nullptr,
    nullptr
);

esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = PIN_NUM_RST,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    .color_space = ESP_LCD_COLOR_SPACE_BGR,
#else
    .color_space = LCD_RGB_ELEMENT_ORDER_BGR,
    .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#endif
    .bits_per_pixel = 16,
};

void setBacklightRaw(uint8_t duty) {
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, duty);
}

void initBacklight() {
    pinMode((int)PIN_NUM_BCKL, OUTPUT);
    digitalWrite((int)PIN_NUM_BCKL, HIGH);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcAttach((int)PIN_NUM_BCKL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
#else
    ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
    ledcAttachPin((int)PIN_NUM_BCKL, LEDC_BACKLIGHT_CHANNEL);
#endif

    // LilyGo factory example drives the backlight active-low:
    // 255 = off, 0 = full brightness.
    setBacklightRaw(255);
}

void setBacklightOn() {
    setBacklightRaw(0);
}

void fillScreen(uint16_t color565) {
    static uint16_t line[LCD_PIXEL_WIDTH * 8];
    for (size_t i = 0; i < (sizeof(line) / sizeof(line[0])); ++i) {
        line[i] = color565;
    }

    for (int y = 0; y < LCD_PIXEL_HEIGHT; y += 8) {
        int h = min(8, LCD_PIXEL_HEIGHT - y);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            panel_handle,
            0,
            y,
            LCD_PIXEL_WIDTH,
            y + h,
            line
        ));
    }
}

void initPanel() {
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi_config, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

    // Exact landscape orientation from LilyGo's factory_screen example.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 1, 26));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

} // namespace

void setup() {
    Serial.begin(115200);
    {
        unsigned long waitStart = millis();
        while (!Serial && (millis() - waitStart < 3000)) {
            delay(50);
        }
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println("  LilyGo Vendor Baseline");
    Serial.println("========================================");

    initBacklight();
    Serial.println("[Vendor] Backlight PWM ready");

    initPanel();
    Serial.println("[Vendor] esp_lcd ST7735 init OK");

    fillScreen(0xF800);
    setBacklightOn();
    Serial.println("[Vendor] RED");
}

void loop() {
    static uint32_t nextChange = 0;
    static uint32_t frame = 0;

    if (millis() >= nextChange) {
        nextChange = millis() + 1000;
        frame++;

        static const uint16_t colors[] = {
            0xF800, // red
            0x07E0, // green
            0x001F, // blue
            0xFFE0, // yellow
            0x07FF, // cyan
            0xF81F, // magenta
            0xFFFF, // white
            0x0000  // black
        };

        uint16_t color = colors[(frame - 1) % (sizeof(colors) / sizeof(colors[0]))];
        fillScreen(color);
        setBacklightOn();
        Serial.printf("[Vendor] frame=%lu color=0x%04X\n", (unsigned long)frame, color);
    }

    delay(10);
}
