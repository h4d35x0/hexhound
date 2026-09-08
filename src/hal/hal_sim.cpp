#ifdef SIMULATOR_BUILD

// ── HexHound - SDL2 Simulator HAL ────────────────────────────────

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "hal.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H per board
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <string>

// ── Constants ──────────────────────────────────────────────────────────────

// Native panel size follows the compiled board profile so the simulator
// window matches the real hardware (160x80 T-Dongle, 320x172 Waveshare, ...).
#define SIM_NATIVE_W   SCREEN_W
#define SIM_NATIVE_H   SCREEN_H
// Framebuffer/window are square (side = long edge) so PORTRAIT mode (used by
// the patrol HUD) fits too. Since the side equals the landscape width, the
// landscape stride is unchanged and landscape rendering is byte-identical.
#define SIM_FB_SIDE    ((SIM_NATIVE_W > SIM_NATIVE_H) ? SIM_NATIVE_W : SIM_NATIVE_H)
// Scale down as the panel grows so the window stays usable on a laptop screen:
// 160 -> 4x (640), 240/320 -> 3x (720/960), 480 -> 2x (960). At 3x the T-RGB
// would open a 1440x1440 window that does not fit most displays.
// The >400 tier is reachable only by the 480 panel, so every existing sim env
// keeps the scale it had. scripts/capture_screens.py PANELS must agree with
// this, or the screenshot crop silently does the wrong thing.
#define SIM_SCALE      ((SIM_FB_SIDE > 400) ? 2 : (SIM_FB_SIDE > 200) ? 3 : 4)
#define SIM_WINDOW_W   (SIM_FB_SIDE * SIM_SCALE)
#define SIM_WINDOW_H   (SIM_FB_SIDE * SIM_SCALE)
#define SIM_FPS        30
#define SIM_FRAME_MS   (1000 / SIM_FPS)

// LED indicator circle radius and position (top-right corner)
#define LED_CIRCLE_R   8
#define LED_CIRCLE_CX  (SIM_WINDOW_W - LED_CIRCLE_R - 4)
#define LED_CIRCLE_CY  (LED_CIRCLE_R + 4)

// ── RGB565 conversion helpers ──────────────────────────────────────────────

static void rgb565_to_rgb(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5) & 0x3F) * 255 / 63;
    b = (c & 0x1F) * 255 / 31;
}

// ── Simple 6x8 bitmap font ────────────────────────────────────────────────
// Minimal ASCII font for simulator display (covers printable ASCII 32-126)

#include "sim_font.h"

// ── Global SDL state ───────────────────────────────────────────────────────

static SDL_Window*   g_window   = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture*  g_texture  = nullptr;
static uint16_t      g_framebuf[SIM_FB_SIDE * SIM_FB_SIDE];
static bool          g_quit     = false;
static uint32_t      g_startMs  = 0;

// LED state
static uint8_t g_ledR = 0, g_ledG = 0, g_ledB = 0;

// Button state (keyboard-driven)
static bool g_buttonPressed   = false;
static bool g_screenshotReq   = false;

// Current rotation (0=portrait 80x160, 1=landscape 160x80)
static int g_rotation = 1;
// Effective dimensions after rotation
static int g_effW = SIM_NATIVE_W;
static int g_effH = SIM_NATIVE_H;

// Text state
static uint16_t g_textFg = 0xFFFF;
static uint16_t g_textBg = 0x0000;
static int g_textSize = 1;
static int g_cursorX = 0;
static int g_cursorY = 0;

// ── Framebuffer helpers ────────────────────────────────────────────────────

static inline void fb_setPixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= g_effW || y < 0 || y >= g_effH) return;

    // Map logical coordinates to physical framebuffer based on rotation.
    // ST7735 rotation 1 sets MADCTL MV|MX, which mirrors the column address.
    // We replicate that here so the simulator matches hardware output.
    int px, py;
    switch (g_rotation) {
    case 0: // portrait: 80x160
        px = x;
        py = y;
        break;
    case 1: // landscape: 160x80 (default)
    default:
        px = x;
        py = y;
        break;
    }

    if (px >= 0 && px < SIM_FB_SIDE && py >= 0 && py < SIM_FB_SIDE) {
        g_framebuf[py * SIM_FB_SIDE + px] = color;
    }
}

static void fb_fillRect(int x, int y, int w, int h, uint16_t color) {
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            fb_setPixel(col, row, color);
        }
    }
}

static void fb_drawHLine(int x, int y, int w, uint16_t color) {
    for (int i = 0; i < w; i++) fb_setPixel(x + i, y, color);
}

static void fb_drawVLine(int x, int y, int h, uint16_t color) {
    for (int i = 0; i < h; i++) fb_setPixel(x, y + i, color);
}

static void fb_drawCircle(int cx, int cy, int r, uint16_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        fb_setPixel(cx + x, cy + y, color);
        fb_setPixel(cx - x, cy + y, color);
        fb_setPixel(cx + x, cy - y, color);
        fb_setPixel(cx - x, cy - y, color);
        fb_setPixel(cx + y, cy + x, color);
        fb_setPixel(cx - y, cy + x, color);
        fb_setPixel(cx + y, cy - x, color);
        fb_setPixel(cx - y, cy - x, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void fb_fillCircle(int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        fb_drawHLine(cx - dx, cy + dy, dx * 2 + 1, color);
    }
}

static void fb_drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        fb_setPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ── Text rendering ────────────────────────────────────────────────────────

static void fb_drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int size) {
    if (c < 32 || c > 126) c = '?';
    int idx = c - 32;
    for (int row = 0; row < 8; row++) {
        uint8_t rowBits = sim_font_data[idx * 8 + row];
        for (int col = 0; col < 6; col++) {
            bool on = (rowBits >> col) & 1;
            uint16_t color = on ? fg : bg;
            if (size == 1) {
                fb_setPixel(x + col, y + row, color);
            } else {
                fb_fillRect(x + col * size, y + row * size, size, size, color);
            }
        }
    }
}

static void fb_print(const char* str) {
    while (*str) {
        if (*str == '\n') {
            g_cursorX = 0;
            g_cursorY += 8 * g_textSize;
            str++;
            continue;
        }
        fb_drawChar(g_cursorX, g_cursorY, *str, g_textFg, g_textBg, g_textSize);
        g_cursorX += 6 * g_textSize;
        str++;
    }
}

// ── Present framebuffer to SDL window ──────────────────────────────────────

static void sim_present() {
    // Convert RGB565 framebuffer to ARGB8888 for SDL texture
    static uint32_t pixels[SIM_FB_SIDE * SIM_FB_SIDE];
    for (int i = 0; i < SIM_FB_SIDE * SIM_FB_SIDE; i++) {
        uint8_t r, g, b;
        rgb565_to_rgb(g_framebuf[i], r, g, b);
        pixels[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

#if HEXHOUND_PANEL_ROUND
    // Round panels only expose the circle inscribed in the framebuffer; every
    // corner pixel is behind the bezel on real glass. Mask them here so the
    // simulator cannot show layout that the hardware would hide, and tint the
    // masked ring so it reads as bezel rather than as black UI background.
    {
        constexpr int kR = SIM_FB_SIDE / 2;
        constexpr int kCX = kR;
        constexpr int kCY = kR;
        for (int y = 0; y < SIM_FB_SIDE; y++) {
            int dy = y - kCY;
            for (int x = 0; x < SIM_FB_SIDE; x++) {
                int dx = x - kCX;
                if (dx * dx + dy * dy > kR * kR) {
                    pixels[y * SIM_FB_SIDE + x] = 0xFF141414;
                }
            }
        }
    }
#endif
    SDL_UpdateTexture(g_texture, nullptr, pixels, SIM_FB_SIDE * sizeof(uint32_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);

    // Draw LED indicator circle (directly in renderer, not framebuffer)
    if (g_ledR > 0 || g_ledG > 0 || g_ledB > 0) {
        SDL_SetRenderDrawColor(g_renderer, g_ledR, g_ledG, g_ledB, 255);
        for (int dy = -LED_CIRCLE_R; dy <= LED_CIRCLE_R; dy++) {
            int dx = (int)sqrt((double)(LED_CIRCLE_R * LED_CIRCLE_R - dy * dy));
            SDL_RenderDrawLine(g_renderer,
                LED_CIRCLE_CX - dx, LED_CIRCLE_CY + dy,
                LED_CIRCLE_CX + dx, LED_CIRCLE_CY + dy);
        }
    } else {
        // Draw filled dark gray circle when LED is off
        SDL_SetRenderDrawColor(g_renderer, 40, 40, 40, 255);
        for (int dy = -LED_CIRCLE_R; dy <= LED_CIRCLE_R; dy++) {
            int dx = (int)sqrt((double)(LED_CIRCLE_R * LED_CIRCLE_R - dy * dy));
            SDL_RenderDrawLine(g_renderer,
                LED_CIRCLE_CX - dx, LED_CIRCLE_CY + dy,
                LED_CIRCLE_CX + dx, LED_CIRCLE_CY + dy);
        }
    }

    SDL_RenderPresent(g_renderer);
}

// ── Screenshot ─────────────────────────────────────────────────────────────

static bool sim_saveScreenshot(const char* path = nullptr) {
    SDL_Surface* surface = SDL_CreateRGBSurface(0, SIM_WINDOW_W, SIM_WINDOW_H,
        32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!surface) return false;
    SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
        surface->pixels, surface->pitch);

    char generatedPath[128];
    if (!path) {
        time_t now = time(nullptr);
        snprintf(generatedPath, sizeof(generatedPath), "/tmp/hexhound_screenshot_%ld.bmp", (long)now);
        path = generatedPath;
    }

    int saveResult = SDL_SaveBMP(surface, path);
    SDL_FreeSurface(surface);
    if (saveResult == 0) {
        ::printf("[Sim] Screenshot saved: %s\n", path);
        return true;
    }

    ::printf("[Sim] Screenshot save failed: %s\n", SDL_GetError());
    return false;
}

// Headless capture: HEXHOUND_SIM_SHOT_MS="1000,3000" fires screenshots at those
// elapsed-ms marks into HEXHOUND_SIM_SHOT_DIR (default "."), and
// HEXHOUND_SIM_QUIT_MS quits the sim automatically. Lets CI/agents grab frames
// with no keyboard automation or window focus.
static void sim_autoCapture() {
    static bool     inited = false;
    // 240 marks, not 16. Sixteen is plenty to verify a layout, which is what
    // this harness was built for, and nowhere near enough to record one: a
    // four-second scene at 15 fps is sixty frames. This array lives in the SDL
    // backend, which is compiled only for the simulator, so the firmware pays
    // nothing for the headroom.
    static uint32_t shotTimes[240];
    // SAME LENGTH AS shotTimes, and the static_assert is why this comment
    // exists. Raising shotTimes to 240 and leaving this at 16 wrote past the
    // end of it for every mark after the sixteenth - an out-of-bounds write
    // introduced while adding video capture, in the harness that is supposed
    // to be verifying everything else. It corrupted whatever the compiler put
    // next in .bss and would have been read as a simulator flake.
    static bool     shotDone[sizeof(shotTimes) / sizeof(shotTimes[0])];
    static_assert(sizeof(shotDone) / sizeof(shotDone[0]) ==
                      sizeof(shotTimes) / sizeof(shotTimes[0]),
                  "shotDone must have one flag per mark in shotTimes");
    static int      nShots  = 0;
    static std::string dir  = ".";
    static uint32_t quitMs  = 0;

    if (!inited) {
        inited = true;
        if (const char* d = getenv("HEXHOUND_SIM_SHOT_DIR")) {
            // A std::string, not a fixed buffer. This was a char[160] filled
            // by strncpy, and create_directories() then ran on the TRUNCATED
            // path: a directory name one character too long silently created
            // a DIFFERENT directory, and every frame landed somewhere the
            // caller was not looking. A harness that quietly writes elsewhere
            // is worse than one that fails, because the screenshots it was
            // asked for simply appear never to have been taken.
            dir = d;
            // SDL_SaveBMP will not create the directory, so a fresh path would
            // otherwise fail once per shot with no frames to show for the run.
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                ::printf("[Sim] Cannot create shot dir '%s': %s\n",
                         dir.c_str(), ec.message().c_str());
            }
        }
        if (const char* s = getenv("HEXHOUND_SIM_SHOT_MS")) {
            const char* p = s;
            constexpr int MAX_SHOTS =
                (int)(sizeof(shotTimes) / sizeof(shotTimes[0]));
            while (*p) {
                if (nShots >= MAX_SHOTS) {
                    // Say so rather than dropping them silently. A caller who
                    // asked for twenty frames and quietly got sixteen would
                    // read the missing ones as "the screen never changed".
                    ::printf("[Sim] HEXHOUND_SIM_SHOT_MS: only the first %d "
                             "marks are used\n", MAX_SHOTS);
                    break;
                }
                shotTimes[nShots] = (uint32_t)strtoul(p, nullptr, 10);
                shotDone[nShots]  = false;
                nShots++;
                const char* comma = strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        }
        if (const char* q = getenv("HEXHOUND_SIM_QUIT_MS")) {
            quitMs = (uint32_t)strtoul(q, nullptr, 10);
        }
    }

    if (nShots == 0 && quitMs == 0) return;

    uint32_t t = SDL_GetTicks() - g_startMs;
    for (int i = 0; i < nShots; i++) {
        if (!shotDone[i] && t >= shotTimes[i]) {
            shotDone[i] = true;
            const std::string path = dir + "/shot_" +
                std::to_string((unsigned)shotTimes[i]) + "ms.bmp";
            sim_saveScreenshot(path.c_str());
        }
    }
    if (quitMs && t >= quitMs) g_quit = true;
}

// ── Poll SDL events ────────────────────────────────────────────────────────

static void sim_pollEvents() {
    SDL_Event evt;
    while (SDL_PollEvent(&evt)) {
        switch (evt.type) {
        case SDL_QUIT:
            g_quit = true;
            break;
        case SDL_KEYDOWN:
            switch (evt.key.keysym.sym) {
            case SDLK_q:
                g_quit = true;
                break;
            case SDLK_SPACE:
                g_buttonPressed = true;
                break;
            case SDLK_RETURN:
                g_buttonPressed = true;
                break;
            case SDLK_s:
                g_screenshotReq = true;
                break;
            }
            break;
        case SDL_KEYUP:
            switch (evt.key.keysym.sym) {
            case SDLK_SPACE:
            case SDLK_RETURN:
                g_buttonPressed = false;
                break;
            }
            break;
        }
    }

    if (g_screenshotReq) {
        sim_saveScreenshot();
        g_screenshotReq = false;
    }

    sim_autoCapture();
}

// ── HalDisplay implementation ──────────────────────────────────────────────

class SimDisplay : public HalDisplay {
public:
    void init() override {
        memset(g_framebuf, 0, sizeof(g_framebuf));
    }

    void fillScreen(uint16_t color) override {
        for (int i = 0; i < SIM_NATIVE_W * SIM_NATIVE_H; i++) {
            g_framebuf[i] = color;
        }
    }

    void drawPixel(int x, int y, uint16_t color) override {
        fb_setPixel(x, y, color);
    }

    void writePixel(int x, int y, uint16_t color) override {
        fb_setPixel(x, y, color);
    }

    void fillRect(int x, int y, int w, int h, uint16_t color) override {
        fb_fillRect(x, y, w, h, color);
    }

    void writeFillRect(int x, int y, int w, int h, uint16_t color) override {
        fb_fillRect(x, y, w, h, color);
    }

    void drawRect(int x, int y, int w, int h, uint16_t color) override {
        fb_drawHLine(x, y, w, color);
        fb_drawHLine(x, y + h - 1, w, color);
        fb_drawVLine(x, y, h, color);
        fb_drawVLine(x + w - 1, y, h, color);
    }

    void drawFastHLine(int x, int y, int w, uint16_t color) override {
        fb_drawHLine(x, y, w, color);
    }

    void drawFastVLine(int x, int y, int h, uint16_t color) override {
        fb_drawVLine(x, y, h, color);
    }

    void drawCircle(int cx, int cy, int r, uint16_t color) override {
        fb_drawCircle(cx, cy, r, color);
    }

    void fillCircle(int cx, int cy, int r, uint16_t color) override {
        fb_fillCircle(cx, cy, r, color);
    }

    void drawLine(int x0, int y0, int x1, int y1, uint16_t color) override {
        fb_drawLine(x0, y0, x1, y1, color);
    }

    void setTextColor(uint16_t fg, uint16_t bg) override {
        g_textFg = fg;
        g_textBg = bg;
    }

    void setTextSize(int size) override {
        g_textSize = size;
    }

    void setCursor(int x, int y) override {
        g_cursorX = x;
        g_cursorY = y;
    }

    void print(const char* str) override {
        fb_print(str);
    }

    void print(char c) override {
        char buf[2] = { c, '\0' };
        fb_print(buf);
    }

    void printf(const char* fmt, ...) override {
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        fb_print(buf);
    }

    void setRotation(int r) override {
        g_rotation = r;
        // Even rotations (0/2) are portrait: swap the native landscape dims.
        if (r == 0 || r == 2) {
            g_effW = SIM_NATIVE_H;
            g_effH = SIM_NATIVE_W;
        } else {
            g_effW = SIM_NATIVE_W;
            g_effH = SIM_NATIVE_H;
        }
    }

    void startWrite() override { }
    void endWrite() override { }

    int width() const override { return g_effW; }
    int height() const override { return g_effH; }
};

// ── HalLED implementation ──────────────────────────────────────────────────

class SimLED : public HalLED {
public:
    void init() override { clear(); }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        g_ledR = r;
        g_ledG = g;
        g_ledB = b;
    }

    void clear() override {
        g_ledR = g_ledG = g_ledB = 0;
    }

    void setBrightness(uint8_t brightness) override {
        (void)brightness;
    }
};

// ── HalButton implementation ───────────────────────────────────────────────

class SimButton : public HalButton {
public:
    void init() override { }

    bool isPressed() override {
        sim_pollEvents();
        sim_present();
        return g_buttonPressed;
    }
};

// ── HalTime implementation ─────────────────────────────────────────────────

class SimTime : public HalTime {
public:
    uint32_t millis() override {
        return SDL_GetTicks() - g_startMs;
    }

    void delay(uint32_t ms) override {
        uint32_t end = SDL_GetTicks() + ms;
        while (SDL_GetTicks() < end && !g_quit) {
            sim_pollEvents();
            sim_present();
            // AND service the capture. Without this every mark that falls
            // inside a delay() is simply lost: a recording of the home screen
            // asked for 45 frames and got 13, with a 2.2 second hole in the
            // middle where the boot sequence sits in delay(). Verifying a
            // layout never noticed, because one frame is enough for that.
            sim_autoCapture();
            SDL_Delay(1);
        }
    }
};

// ── HalSerial implementation ───────────────────────────────────────────────

class SimSerial : public HalSerial {
public:
    void begin(uint32_t baud) override {
        (void)baud;
        ::printf("[Sim] Serial initialized (stdout)\n");
    }

    void println(const char* str) override {
        ::printf("%s\n", str);
    }

    void printf(const char* fmt, ...) override {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
};

// ── Global instances ───────────────────────────────────────────────────────

static SimDisplay s_display;
static SimLED     s_led;
static SimButton  s_button;
static SimTime    s_time;
static SimSerial  s_serial;

HalDisplay& halDisplay() { return s_display; }
HalLED&     halLED()     { return s_led; }
HalButton&  halButton()  { return s_button; }
HalTime&    halTime()    { return s_time; }
HalSerial&  halSerial()  { return s_serial; }

void halInit() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        ::printf("[Sim] SDL init failed: %s\n", SDL_GetError());
        exit(1);
    }

    g_window = SDL_CreateWindow(
        "HexHound - Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SIM_WINDOW_W, SIM_WINDOW_H,
        SDL_WINDOW_SHOWN);

    if (!g_window) {
        ::printf("[Sim] Window creation failed: %s\n", SDL_GetError());
        exit(1);
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }

    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, SIM_FB_SIDE, SIM_FB_SIDE);

    SDL_RenderSetLogicalSize(g_renderer, SIM_WINDOW_W, SIM_WINDOW_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor

    g_startMs = SDL_GetTicks();

    s_display.init();
    s_led.init();

    ::printf("[Sim] SDL2 simulator initialized (%dx%d scaled %dx)\n",
             SIM_NATIVE_W, SIM_NATIVE_H, SIM_SCALE);
}

bool halShouldQuit() { return g_quit; }

bool halSaveScreenshot(const char* path) {
    sim_present();
    return sim_saveScreenshot(path);
}

#endif // SIMULATOR_BUILD
