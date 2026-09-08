#include "tft_compat.h"

#if defined(HEXHOUND_SOFT_TFT) || defined(HEXHOUND_VENDOR_TFT) || defined(HEXHOUND_RGB_PANEL)

// The RGB backend reuses everything in this file that is written in terms of
// drawPixel()/fillRect() - lines, rectangles, circles, the GLCD font,
// print/printf/drawString - and replaces only the blocks fenced off below,
// which talk to a 4-wire SPI panel that T-RGB does not have. Its versions of
// those live in tft_compat_rgb.cpp.

#include "../config.h"
#include "backlight.h"
#include "sim_font.h"
#include <math.h>

#ifdef HEXHOUND_VENDOR_TFT
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace {

#ifdef HEXHOUND_VENDOR_TFT
// esp_lcd_panel_draw_bitmap() is ASYNCHRONOUS: the panel IO is created with
// trans_queue_depth = 10 (see ST7735_PANEL_IO_SPI_CONFIG), so it queues the
// SPI transfer and returns while the DMA still reads the caller's buffer.
// fillRect() hands it one shared static buffer, so back-to-back calls used to
// overwrite pixels that were still in flight - harmless-looking with pixel art
// (a few long runs per row), badly wrong with HD art (hundreds of short spans).
//
// Signalling completion and waiting for it makes the shared buffer safe. The
// wait is bounded: a lost callback must degrade to slow, never to a hang.
SemaphoreHandle_t g_flushDone = nullptr;

bool IRAM_ATTR vendorColorTransDone(esp_lcd_panel_io_handle_t,
                                    esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t higherPriorityWoken = pdFALSE;
    if (g_flushDone) {
        xSemaphoreGiveFromISR(g_flushDone, &higherPriorityWoken);
    }
    return higherPriorityWoken == pdTRUE;
}

void vendorWaitForFlush() {
    if (g_flushDone) {
        xSemaphoreTake(g_flushDone, pdMS_TO_TICKS(100));
    }
}
#endif  // HEXHOUND_VENDOR_TFT


#ifndef HEXHOUND_VENDOR_TFT
constexpr uint8_t ST7735_SWRESET = 0x01;
constexpr uint8_t ST7735_SLPOUT  = 0x11;
constexpr uint8_t ST7735_COLMOD  = 0x3A;
constexpr uint8_t ST7735_MADCTL  = 0x36;
constexpr uint8_t ST7735_CASET   = 0x2A;
constexpr uint8_t ST7735_RASET   = 0x2B;
constexpr uint8_t ST7735_RAMWR   = 0x2C;
constexpr uint8_t ST7735_INVON   = 0x21;
constexpr uint8_t ST7735_INVOFF  = 0x20;
constexpr uint8_t ST7735_NORON   = 0x13;
constexpr uint8_t ST7735_DISPOFF = 0x28;
constexpr uint8_t ST7735_DISPON  = 0x29;
#endif

// This exact native memory layout is the one proven by src/tft_test.cpp.
// We keep the controller in this portrait mode and rotate coordinates in
// software so we do not depend on uncertain ST7735 offset tables.
constexpr uint8_t ST7735_NATIVE_MADCTL = 0xC8;
constexpr int ST7735_NATIVE_WIDTH = 80;
constexpr int ST7735_NATIVE_HEIGHT = 160;
constexpr int ST7735_NATIVE_COLSTART = 26;
constexpr int ST7735_NATIVE_ROWSTART = 1;

inline int clampLow(int v, int low) {
    return v < low ? low : v;
}

inline void bbClockPulse() {
    digitalWrite(PIN_TFT_SCLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_TFT_SCLK, LOW);
    delayMicroseconds(2);
}

inline void bbSendByte(uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
        digitalWrite(PIN_TFT_MOSI, (value >> bit) & 0x01);
        bbClockPulse();
    }
}

} // namespace

#ifndef HEXHOUND_RGB_PANEL

TFT_eSPI::TFT_eSPI()
    : _initialized(false),
      _inTransaction(false),
      _rotation(1),
      _colstart(ST7735_NATIVE_COLSTART),
      _rowstart(ST7735_NATIVE_ROWSTART),
      _width(160),
      _height(80),
      _textFg(TFT_WHITE),
      _textBg(TFT_BLACK),
      _textSize(1),
      _cursorX(0),
      _cursorY(0)
#ifdef HEXHOUND_VENDOR_TFT
      , _panel(nullptr),
      _io(nullptr)
#endif
{}

void TFT_eSPI::rotationConfig(uint8_t rotation, uint8_t& madctl, uint8_t& colstart, uint8_t& rowstart, int& width, int& height) {
    madctl = ST7735_NATIVE_MADCTL;
    colstart = ST7735_NATIVE_COLSTART;
    rowstart = ST7735_NATIVE_ROWSTART;
    switch (rotation & 0x03) {
        case 0:
            width = ST7735_NATIVE_WIDTH;
            height = ST7735_NATIVE_HEIGHT;
            break;
        case 1:
            width = 160;
            height = 80;
            break;
        case 2:
            width = ST7735_NATIVE_WIDTH;
            height = ST7735_NATIVE_HEIGHT;
            break;
        default:
            width = 160;
            height = 80;
            break;
    }
}

void TFT_eSPI::hardReset() {
    digitalWrite(PIN_TFT_RST, HIGH);
    delay(50);
    digitalWrite(PIN_TFT_RST, LOW);
    delay(50);
    digitalWrite(PIN_TFT_RST, HIGH);
    delay(150);
}

void TFT_eSPI::beginScopedWrite(bool& ownsTransaction) {
    ownsTransaction = !_inTransaction;
}

void TFT_eSPI::endScopedWrite(bool ownsTransaction) {
    (void)ownsTransaction;
}

void TFT_eSPI::sendCommand(uint8_t cmd) {
#ifdef HEXHOUND_VENDOR_TFT
    if (_io) {
        esp_lcd_panel_io_tx_param(_io, cmd, nullptr, 0);
    }
#else
    digitalWrite(PIN_TFT_DC, LOW);
    digitalWrite(PIN_TFT_CS, LOW);
    bbSendByte(cmd);
    digitalWrite(PIN_TFT_CS, HIGH);
#endif
}

void TFT_eSPI::sendData(uint8_t data) {
#ifdef HEXHOUND_VENDOR_TFT
    if (_io) {
        esp_lcd_panel_io_tx_param(_io, -1, &data, 1);
    }
#else
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    bbSendByte(data);
    digitalWrite(PIN_TFT_CS, HIGH);
#endif
}

void TFT_eSPI::sendData16(uint16_t data) {
#ifdef HEXHOUND_VENDOR_TFT
    uint8_t bytes[] = {
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF)
    };
    if (_io) {
        esp_lcd_panel_io_tx_param(_io, -1, bytes, sizeof(bytes));
    }
#else
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    bbSendByte(data >> 8);
    bbSendByte(data & 0xFF);
    digitalWrite(PIN_TFT_CS, HIGH);
#endif
}

void TFT_eSPI::setAddrWindow(int x, int y, int w, int h) {
#ifdef HEXHOUND_VENDOR_TFT
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return;
#else
    int x0 = x;
    int y0 = y;
    int x1 = x + w - 1;
    int y1 = y + h - 1;

    int tx0 = 0;
    int ty0 = 0;
    int tx1 = 0;
    int ty1 = 0;

    switch (_rotation & 0x03) {
        case 0:
            tx0 = x0;
            tx1 = x1;
            ty0 = y0;
            ty1 = y1;
            break;
        case 1:
            tx0 = y0;
            tx1 = y1;
            ty0 = ST7735_NATIVE_HEIGHT - 1 - x1;
            ty1 = ST7735_NATIVE_HEIGHT - 1 - x0;
            break;
        case 2:
            tx0 = ST7735_NATIVE_WIDTH - 1 - x1;
            tx1 = ST7735_NATIVE_WIDTH - 1 - x0;
            ty0 = ST7735_NATIVE_HEIGHT - 1 - y1;
            ty1 = ST7735_NATIVE_HEIGHT - 1 - y0;
            break;
        default:
            tx0 = ST7735_NATIVE_WIDTH - 1 - y1;
            tx1 = ST7735_NATIVE_WIDTH - 1 - y0;
            ty0 = x0;
            ty1 = x1;
            break;
    }

    x0 = tx0 + _colstart;
    x1 = tx1 + _colstart;
    y0 = ty0 + _rowstart;
    y1 = ty1 + _rowstart;

    sendCommand(ST7735_CASET);
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    bbSendByte((x0 >> 8) & 0xFF);
    bbSendByte(x0 & 0xFF);
    bbSendByte((x1 >> 8) & 0xFF);
    bbSendByte(x1 & 0xFF);
    digitalWrite(PIN_TFT_CS, HIGH);

    sendCommand(ST7735_RASET);
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    bbSendByte((y0 >> 8) & 0xFF);
    bbSendByte(y0 & 0xFF);
    bbSendByte((y1 >> 8) & 0xFF);
    bbSendByte(y1 & 0xFF);
    digitalWrite(PIN_TFT_CS, HIGH);

    sendCommand(ST7735_RAMWR);
#endif
}

void TFT_eSPI::streamColor(uint16_t color, uint32_t count) {
#ifdef HEXHOUND_VENDOR_TFT
    (void)color;
    (void)count;
#else
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    for (uint32_t i = 0; i < count; ++i) {
        bbSendByte(hi);
        bbSendByte(lo);
    }
    digitalWrite(PIN_TFT_CS, HIGH);
#endif
}

void TFT_eSPI::init() {
#ifdef HEXHOUND_VENDOR_TFT
    gpio_reset_pin((gpio_num_t)PIN_TFT_RST);
    gpio_reset_pin((gpio_num_t)PIN_TFT_DC);
    gpio_reset_pin((gpio_num_t)PIN_TFT_MOSI);
    gpio_reset_pin((gpio_num_t)PIN_TFT_CS);
    gpio_reset_pin((gpio_num_t)PIN_TFT_SCLK);

    hexhoundInitBacklightHardware();
    hexhoundSetBacklight(false);

    static spi_bus_config_t spi_config = ST7735_PANEL_BUS_SPI_CONFIG(
        PIN_TFT_SCLK,
        PIN_TFT_MOSI,
        SCREEN_W * SCREEN_H * sizeof(uint16_t)
    );
    // The completion callback is what makes the shared draw buffer safe to
    // reuse; see vendorColorTransDone above.
    if (g_flushDone == nullptr) {
        g_flushDone = xSemaphoreCreateBinary();
    }
    static esp_lcd_panel_io_spi_config_t io_config = ST7735_PANEL_IO_SPI_CONFIG(
        PIN_TFT_CS,
        PIN_TFT_DC,
        vendorColorTransDone,
        nullptr
    );
    static esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_TFT_RST,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
#else
        .color_space = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#endif
        .bits_per_pixel = 16,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &spi_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[VendorTFT] spi_bus_initialize failed: %d\n", (int)err);
        return;
    }
    if ((_io == nullptr) &&
        ((err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &_io)) != ESP_OK)) {
        Serial.printf("[VendorTFT] panel_io init failed: %d\n", (int)err);
        return;
    }
    if ((_panel == nullptr) &&
        ((err = esp_lcd_new_panel_st7735(_io, &panel_config, &_panel)) != ESP_OK)) {
        Serial.printf("[VendorTFT] panel init failed: %d\n", (int)err);
        return;
    }

    if ((err = esp_lcd_panel_reset(_panel)) != ESP_OK ||
        (err = esp_lcd_panel_init(_panel)) != ESP_OK ||
        (err = esp_lcd_panel_invert_color(_panel, true)) != ESP_OK ||
        (err = esp_lcd_panel_disp_on_off(_panel, true)) != ESP_OK) {
        Serial.printf("[VendorTFT] panel bring-up failed: %d\n", (int)err);
        return;
    }

    _initialized = true;
    setRotation(1);
    fillScreen(TFT_BLACK);
    hexhoundSetBacklight(true);
#else
    gpio_reset_pin((gpio_num_t)PIN_TFT_RST);
    gpio_reset_pin((gpio_num_t)PIN_TFT_DC);
    gpio_reset_pin((gpio_num_t)PIN_TFT_MOSI);
    gpio_reset_pin((gpio_num_t)PIN_TFT_CS);
    gpio_reset_pin((gpio_num_t)PIN_TFT_SCLK);
    gpio_reset_pin((gpio_num_t)PIN_TFT_BL);

    pinMode(PIN_TFT_CS, OUTPUT);
    pinMode(PIN_TFT_DC, OUTPUT);
    pinMode(PIN_TFT_RST, OUTPUT);
    pinMode(PIN_TFT_MOSI, OUTPUT);
    pinMode(PIN_TFT_SCLK, OUTPUT);
    pinMode(PIN_TFT_BL, OUTPUT);

    digitalWrite(PIN_TFT_CS, HIGH);
    digitalWrite(PIN_TFT_SCLK, LOW);
    digitalWrite(PIN_TFT_BL, LOW);

    hardReset();

    startWrite();

    sendCommand(ST7735_SWRESET);
    delay(150);

    sendCommand(ST7735_SLPOUT);
    delay(500);

    sendCommand(ST7735_COLMOD);
    sendData(0x05);

    sendCommand(ST7735_MADCTL);
    sendData(ST7735_NATIVE_MADCTL);

    setRotation(1);
    invertDisplay(true);

    sendCommand(ST7735_NORON);
    delay(10);
    sendCommand(ST7735_DISPON);
    delay(100);

    endWrite();

    fillScreen(TFT_BLACK);
    digitalWrite(PIN_TFT_BL, HIGH);
    _initialized = true;
#endif
}

void TFT_eSPI::fillScreen(uint16_t color) {
    fillRect(0, 0, _width, _height, color);
}

void TFT_eSPI::drawPixel(int x, int y, uint16_t color) {
#ifdef HEXHOUND_VENDOR_TFT
    fillRect(x, y, 1, 1, color);
#else
    if (x < 0 || y < 0 || x >= _width || y >= _height) {
        return;
    }

    bool owns = false;
    beginScopedWrite(owns);
    setAddrWindow(x, y, 1, 1);
    streamColor(color, 1);
    endScopedWrite(owns);
#endif
}

void TFT_eSPI::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }

    int x0 = clampLow(x, 0);
    int y0 = clampLow(y, 0);
    int x1 = x + w;
    int y1 = y + h;
    if (x0 >= _width || y0 >= _height || x1 <= 0 || y1 <= 0) {
        return;
    }
    if (x1 > _width) x1 = _width;
    if (y1 > _height) y1 = _height;

    int drawW = x1 - x0;
    int drawH = y1 - y0;
    if (drawW <= 0 || drawH <= 0) {
        return;
    }

#ifdef HEXHOUND_VENDOR_TFT
    if (!_initialized || !_panel) {
        return;
    }

    static uint16_t chunkBuffer[SCREEN_W * 8];
    int maxChunkPixels = (int)(sizeof(chunkBuffer) / sizeof(chunkBuffer[0]));
    int chunkRows = maxChunkPixels / drawW;
    if (chunkRows < 1) {
        chunkRows = 1;
    }
    if (chunkRows > drawH) {
        chunkRows = drawH;
    }

    for (int row = 0; row < drawH; row += chunkRows) {
        int rowsThisChunk = drawH - row;
        if (rowsThisChunk > chunkRows) {
            rowsThisChunk = chunkRows;
        }
        int pixelsThisChunk = drawW * rowsThisChunk;
        for (int i = 0; i < pixelsThisChunk; ++i) {
            chunkBuffer[i] = color;
        }
        esp_lcd_panel_draw_bitmap(_panel, x0, y0 + row, x0 + drawW, y0 + row + rowsThisChunk, chunkBuffer);
        // Do not touch chunkBuffer again until this transfer has landed.
        vendorWaitForFlush();
    }
#else
    bool owns = false;
    beginScopedWrite(owns);
    setAddrWindow(x0, y0, drawW, drawH);
    streamColor(color, (uint32_t)drawW * (uint32_t)drawH);
    endScopedWrite(owns);
#endif
}

#endif  // !HEXHOUND_RGB_PANEL

void TFT_eSPI::drawFastHLine(int x, int y, int w, uint16_t color) {
    fillRect(x, y, w, 1, color);
}

void TFT_eSPI::drawFastVLine(int x, int y, int h, uint16_t color) {
    fillRect(x, y, 1, h, color);
}

void TFT_eSPI::drawRect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color);
    drawFastVLine(x + w - 1, y, h, color);
}

void TFT_eSPI::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    startWrite();
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err << 1;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    endWrite();
}

void TFT_eSPI::drawCircle(int cx, int cy, int r, uint16_t color) {
    startWrite();
    int x = r;
    int y = 0;
    int err = 1 - r;

    while (x >= y) {
        drawPixel(cx + x, cy + y, color);
        drawPixel(cx - x, cy + y, color);
        drawPixel(cx + x, cy - y, color);
        drawPixel(cx - x, cy - y, color);
        drawPixel(cx + y, cy + x, color);
        drawPixel(cx - y, cy + x, color);
        drawPixel(cx + y, cy - x, color);
        drawPixel(cx - y, cy - x, color);
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
    endWrite();
}

void TFT_eSPI::fillCircle(int cx, int cy, int r, uint16_t color) {
    startWrite();
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        drawFastHLine(cx - dx, cy + dy, dx * 2 + 1, color);
    }
    endWrite();
}

void TFT_eSPI::setTextColor(uint16_t fg, uint16_t bg) {
    _textFg = fg;
    _textBg = bg;
}

void TFT_eSPI::setTextSize(int size) {
    _textSize = size < 1 ? 1 : size;
}

void TFT_eSPI::setCursor(int x, int y) {
    _cursorX = x;
    _cursorY = y;
}

void TFT_eSPI::drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int size) {
    if (c < 32 || c > 126) {
        c = '?';
    }
    int idx = c - 32;
    for (int row = 0; row < 8; ++row) {
        uint8_t rowBits = sim_font_data[idx * 8 + row];
        for (int col = 0; col < 6; ++col) {
            bool on = ((rowBits >> col) & 0x01) != 0;
            uint16_t color = on ? fg : bg;
            if (size == 1) {
                drawPixel(x + col, y + row, color);
            } else {
                fillRect(x + col * size, y + row * size, size, size, color);
            }
        }
    }
}

size_t TFT_eSPI::print(const char* str) {
    if (!str) {
        return 0;
    }

    startWrite();
    size_t count = 0;
    while (*str) {
        if (*str == '\n') {
            _cursorX = 0;
            _cursorY += 8 * _textSize;
            ++str;
            continue;
        }
        drawChar(_cursorX, _cursorY, *str, _textFg, _textBg, _textSize);
        _cursorX += 6 * _textSize;
        ++str;
        ++count;
    }
    endWrite();
    return count;
}

size_t TFT_eSPI::print(char c) {
    char buf[2] = { c, '\0' };
    return print(buf);
}

size_t TFT_eSPI::print(int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    return print(buf);
}

size_t TFT_eSPI::print(unsigned long val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", val);
    return print(buf);
}

int TFT_eSPI::printf(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    print(buf);
    return n;
}

int TFT_eSPI::drawString(const char* str, int x, int y, int font) {
    int oldSize = _textSize;
    _textSize = font <= 1 ? 1 : font;
    setCursor(x, y);
    print(str);
    int widthPx = (int)strlen(str) * 6 * _textSize;
    _textSize = oldSize;
    return widthPx;
}

#ifndef HEXHOUND_RGB_PANEL
void TFT_eSPI::setRotation(int r) {
    _rotation = r & 0x03;

    uint8_t madctl = 0;
    rotationConfig(_rotation, madctl, _colstart, _rowstart, _width, _height);
#ifdef HEXHOUND_VENDOR_TFT
    applyVendorRotation();
#endif
}
#endif  // !HEXHOUND_RGB_PANEL

void TFT_eSPI::startWrite() {
    _inTransaction = true;
}

void TFT_eSPI::endWrite() {
    _inTransaction = false;
}

#ifndef HEXHOUND_RGB_PANEL
void TFT_eSPI::writecommand(uint8_t cmd) {
#ifdef HEXHOUND_VENDOR_TFT
    if (!_initialized || !_panel) {
        return;
    }
    if (cmd == ST7735_DISPON) {
        esp_lcd_panel_disp_on_off(_panel, true);
    } else if (cmd == ST7735_DISPOFF) {
        esp_lcd_panel_disp_on_off(_panel, false);
    } else if (_io) {
        esp_lcd_panel_io_tx_param(_io, cmd, nullptr, 0);
    }
#else
    bool owns = false;
    beginScopedWrite(owns);
    sendCommand(cmd);
    endScopedWrite(owns);
#endif
}

void TFT_eSPI::writedata(uint8_t data) {
#ifdef HEXHOUND_VENDOR_TFT
    if (!_initialized || !_panel) {
        return;
    }
    if (_io) {
        esp_lcd_panel_io_tx_param(_io, -1, &data, 1);
    }
#else
    bool owns = false;
    beginScopedWrite(owns);
    sendData(data);
    endScopedWrite(owns);
#endif
}

void TFT_eSPI::invertDisplay(bool invert) {
#ifdef HEXHOUND_VENDOR_TFT
    if (_initialized && _panel) {
        esp_lcd_panel_invert_color(_panel, invert);
    }
#else
    bool owns = false;
    beginScopedWrite(owns);
    sendCommand(invert ? ST7735_INVON : ST7735_INVOFF);
    endScopedWrite(owns);
#endif
}

#ifdef HEXHOUND_VENDOR_TFT
void TFT_eSPI::applyVendorRotation() {
    if (!_panel) {
        return;
    }

    switch (_rotation & 0x03) {
        case 0:
            esp_lcd_panel_set_gap(_panel, 26, 1);
            esp_lcd_panel_swap_xy(_panel, false);
            esp_lcd_panel_mirror(_panel, true, true);
            break;
        case 1:
            esp_lcd_panel_set_gap(_panel, 1, 26);
            esp_lcd_panel_swap_xy(_panel, true);
            esp_lcd_panel_mirror(_panel, false, true);
            break;
        case 2:
            esp_lcd_panel_set_gap(_panel, 26, 1);
            esp_lcd_panel_swap_xy(_panel, false);
            esp_lcd_panel_mirror(_panel, false, false);
            break;
        default:
            esp_lcd_panel_set_gap(_panel, 1, 26);
            esp_lcd_panel_swap_xy(_panel, true);
            esp_lcd_panel_mirror(_panel, true, false);
            break;
    }
}
#endif
#endif  // !HEXHOUND_RGB_PANEL

#endif
