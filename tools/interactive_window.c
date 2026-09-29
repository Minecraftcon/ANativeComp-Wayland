/**
 * tools/interactive_window.c
 *
 * Interactive Wayland Client for testing touch, pointer, and keyboard input
 * on Android SurfaceFlinger via ANativeDrawer.
 *
 * Features:
 * - Listens for wl_touch, wl_pointer, and wl_keyboard events
 * - Visually responds to taps and drags by drawing ripples and crosshairs
 * - Displays a live click counter and last input coordinates
 * - Tapping the Red Close traffic light exits cleanly
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <wayland-client.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

// ── Minimal 5x7 ASCII font table (characters 32-126) ─────────────────────────
static const unsigned char font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // 87 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // 94 '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 97 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 98 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 99 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 100 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 101 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 102 'f'
    {0x08, 0x14, 0x54, 0x54, 0x3C}, // 103 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 104 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 105 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 106 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 107 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 108 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 109 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 110 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 111 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 112 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 113 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 114 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 115 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 116 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 117 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 118 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 119 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 120 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 121 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 122 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // 123 '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // 124 '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // 125 '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // 126 '~'
};

static void draw_char(uint32_t* buf, int stride, int x, int y, char c, int scale, uint32_t color) {
    if (c < 32 || c > 126) c = ' ';
    const unsigned char* glyph = font5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
        unsigned char bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        buf[py * stride + px] = color;
                    }
                }
            }
        }
    }
}

static void draw_text(uint32_t* buf, int stride, int x, int y, const char* str, int scale, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_char(buf, stride, cur_x, y, *str, scale, color);
        cur_x += 6 * scale;
        str++;
    }
}

static void fill_rect(uint32_t* buf, int stride, int x, int y, int w, int h, uint32_t color) {
    for (int r = y; r < y + h; ++r) {
        for (int c = x; c < x + w; ++c) {
            buf[r * stride + c] = color;
        }
    }
}

static void draw_circle(uint32_t* buf, int stride, int cx, int cy, int radius, uint32_t color) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                int px = cx + x;
                int py = cy + y;
                if (px >= 0 && px < stride && py >= 0) {
                    buf[py * stride + px] = color;
                }
            }
        }
    }
}

static void draw_ring(uint32_t* buf, int stride, int cx, int cy, int radius, int thickness, uint32_t color) {
    int rInner = radius - thickness;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            int d2 = x * x + y * y;
            if (d2 <= radius * radius && d2 >= rInner * rInner) {
                int px = cx + x;
                int py = cy + y;
                if (px >= 0 && px < stride && py >= 0) {
                    buf[py * stride + px] = color;
                }
            }
        }
    }
}

// ── State ────────────────────────────────────────────────────────────────────
static struct wl_compositor* g_compositor = NULL;
static struct wl_shm*        g_shm        = NULL;
static struct wl_seat*       g_seat       = NULL;
static struct wl_pointer*    g_pointer    = NULL;
static struct wl_keyboard*   g_keyboard   = NULL;
static struct wl_touch*      g_touch      = NULL;
static struct wl_output*     g_output     = NULL;
static int g_outputWidth  = 0;
static int g_outputHeight = 0;

static volatile sig_atomic_t g_running    = 1;

static int g_tapCount = 0;
static int g_lastX = -1;
static int g_lastY = -1;
static int g_prevX = -1;
static int g_prevY = -1;
static char g_lastEvent[128] = "Ready for input...";
static char g_lastInputType[32] = "None";
static uint32_t* g_pixels = NULL;
static struct wl_surface* g_surface = NULL;
static struct wl_buffer*  g_buffer  = NULL;
// Window geometry. Defaults to a 560x400 desktop-style window; override with
// --fullscreen (matches the display) or --size <w> <h> for arbitrary sizes.
static int W = 560;
static int H = 400;
static int g_fullscreen = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void redraw() {
    if (!g_pixels || !g_surface) return;

    // Static chrome is drawn once by drawStaticChrome(); only the dynamic
    // regions below are repainted per event. Every dynamic element first clears
    // its own footprint, because the damage rect tells the compositor only that
    // something in the region changed, not what.
    char count_str[64];
    snprintf(count_str, sizeof(count_str), "TAPS: %d", g_tapCount);
    fill_rect(g_pixels, W, 40, 65, W - 80, 60, 0xFF0E1117);
    draw_text(g_pixels, W, 50, 75, count_str, 5, 0xFF38BDF8);

    // 5. Input Info Card
    fill_rect(g_pixels, W, 30, 140, W - 60, 110, 0xFF141822);
    for (int y = 140; y < 250; ++y) {
        for (int x = 30; x < W - 30; ++x) {
            if (x == 30 || x == W - 31 || y == 140 || y == 249) {
                g_pixels[y * W + x] = 0xFF2A3245;
            }
        }
    }

    char type_str[64];
    snprintf(type_str, sizeof(type_str), "Event Type: %s", g_lastInputType);
    draw_text(g_pixels, W, 45, 155, type_str, 2, 0xFFF1F5F9);

    char coord_str[64];
    if (g_lastX >= 0 && g_lastY >= 0) {
        snprintf(coord_str, sizeof(coord_str), "Surface Pos: (%d, %d)", g_lastX, g_lastY);
    } else {
        snprintf(coord_str, sizeof(coord_str), "Surface Pos: (tap anywhere)");
    }
    draw_text(g_pixels, W, 45, 185, coord_str, 2, 0xFF4ADE80);

    draw_text(g_pixels, W, 45, 215, g_lastEvent, 2, 0xFF94A3B8);

    // 6. Draw Ripple Indicator in the interaction area (below info card)
    if (g_prevX >= 0 && g_prevY >= 280) {
        int ex = g_prevX - 28; if (ex < 0) ex = 0; if (ex > W - 56) ex = W - 56;
        int ey = g_prevY - 28; if (ey < 252) ey = 252; if (ey > H - 56) ey = H - 56;
        fill_rect(g_pixels, W, ex, ey, 56, 56, 0xFF0E1117);
    }
    if (g_lastX >= 0 && g_lastY >= 280 && g_lastX < W && g_lastY < H - 20) {
        draw_ring(g_pixels, W, g_lastX, g_lastY, 24, 3, 0xFFFF007F); // Neon Magenta Ring
        draw_circle(g_pixels, W, g_lastX, g_lastY, 5, 0xFFFFFFFF);    // Center white dot
    }
    g_prevX = g_lastX;
    g_prevY = g_lastY;

    // 7. Footer Instructions
    draw_text(g_pixels, W, 35, 275, "Tap inside this window to trigger input events.", 2, 0xFFCBD5E1);
    draw_text(g_pixels, W, 35, 305, "Tap top-left Red button to close.", 2, 0xFFF87171);

    // Status Indicator
    draw_circle(g_pixels, W, 45, 360, 5, 0xFF22C55E);
    draw_text(g_pixels, W, 60, 355, "Seat: Touch + Pointer + Keyboard ACTIVE", 2, 0xFF4ADE80);

    // g_pixels is persistent: the static chrome drawn once by
    // drawStaticChrome() is still valid, and only the regions below change.
    // The compositor merges this damage into its own complete back buffer, so
    // a partial rect here is both accurate and safe.
    const int kDirtyX0 = 20,  kDirtyY0 = 60;
    const int kDirtyX1 = W - 20, kDirtyY1 = 380;
    wl_surface_attach(g_surface, g_buffer, 0, 0);
    wl_surface_damage(g_surface, kDirtyX0, kDirtyY0,
                      kDirtyX1 - kDirtyX0, kDirtyY1 - kDirtyY0);
    wl_surface_commit(g_surface);
}

// Repaints the static chrome once; the dynamic regions above are redrawn by
// redraw() on every event. Keeps per-event work off the 1.15M-pixel border loop.
static void drawStaticChrome(void) {
    if (!g_pixels) return;

    fill_rect(g_pixels, W, 0, 0, W, H, 0xFF0E1117);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) {
                g_pixels[y * W + x] = 0xFF00E5FF;
            }
        }
    }

    fill_rect(g_pixels, W, 2, 2, W - 4, 44, 0xFF181C26);
    for (int x = 2; x < W - 2; ++x) g_pixels[46 * W + x] = 0xFF282D3D;

    draw_circle(g_pixels, W, 22, 24, 6, 0xFFFF5F56);
    draw_circle(g_pixels, W, 42, 24, 6, 0xFFFFBD2E);
    draw_circle(g_pixels, W, 62, 24, 6, 0xFF27C93F);
    draw_text(g_pixels, W, 86, 19, "ANativeDrawer  Interactive Input Test", 2, 0xFFE2E8F0);

    // Info card frame (its text is dynamic, redrawn each frame)
    fill_rect(g_pixels, W, 30, 140, W - 60, 110, 0xFF141822);
    for (int y = 140; y < 250; ++y) {
        for (int x = 30; x < W - 30; ++x) {
            if (x == 30 || x == W - 31 || y == 140 || y == 249) {
                g_pixels[y * W + x] = 0xFF2A3245;
            }
        }
    }
}

// ── wl_touch listener ────────────────────────────────────────────────────────
static void touch_down(void* data, struct wl_touch* touch,
                       uint32_t serial, uint32_t time, struct wl_surface* surface,
                       int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)touch; (void)serial; (void)time; (void)surface;
    int px = wl_fixed_to_int(x);
    int py = wl_fixed_to_int(y);
    printf(">> [TOUCH DOWN] slot=%d, x=%d, y=%d\n", id, px, py);

    // Check red button tap (x ~ 14..30, y ~ 16..32)
    if (px >= 12 && px <= 32 && py >= 14 && py <= 34) {
        printf(">> [CLOSE BUTTON TAPPED] Exiting!\n");
        g_running = 0;
        return;
    }

    g_tapCount++;
    g_lastX = px;
    g_lastY = py;
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_touch (slot %d)", id);
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Touch DOWN at (%d, %d)", px, py);
    redraw();
}

static void touch_up(void* data, struct wl_touch* touch,
                     uint32_t serial, uint32_t time, int32_t id) {
    (void)data; (void)touch; (void)serial; (void)time;
    printf(">> [TOUCH UP] slot=%d\n", id);
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_touch (slot %d)", id);
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Touch UP for slot %d", id);
    redraw();
}

static void touch_motion(void* data, struct wl_touch* touch,
                         uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)touch; (void)time;
    int px = wl_fixed_to_int(x);
    int py = wl_fixed_to_int(y);
    g_lastX = px;
    g_lastY = py;
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_touch (slot %d)", id);
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Drag Motion to (%d, %d)", px, py);
    redraw();
}

static void touch_frame(void* data, struct wl_touch* touch) { (void)data; (void)touch; }
static void touch_cancel(void* data, struct wl_touch* touch) { (void)data; (void)touch; }
// wl_touch v6+ events — must be non-NULL or libwayland aborts on receipt.
static void touch_shape(void* data, struct wl_touch* touch, int32_t id, wl_fixed_t major, wl_fixed_t minor) {
    (void)data; (void)touch; (void)id; (void)major; (void)minor;
}
static void touch_orientation(void* data, struct wl_touch* touch, int32_t id, wl_fixed_t orientation) {
    (void)data; (void)touch; (void)id; (void)orientation;
}

static const struct wl_touch_listener touch_listener = {
    .down        = touch_down,
    .up          = touch_up,
    .motion      = touch_motion,
    .frame       = touch_frame,
    .cancel      = touch_cancel,
    .shape       = touch_shape,
    .orientation = touch_orientation,
};

// ── wl_pointer listener ──────────────────────────────────────────────────────
static void pointer_enter(void* data, struct wl_pointer* pointer,
                          uint32_t serial, struct wl_surface* surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)pointer; (void)serial; (void)surface;
    int px = wl_fixed_to_int(sx);
    int py = wl_fixed_to_int(sy);
    printf(">> [POINTER ENTER] at (%d, %d)\n", px, py);
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_pointer");
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Pointer Entered at (%d, %d)", px, py);
    redraw();
}

static void pointer_leave(void* data, struct wl_pointer* pointer,
                          uint32_t serial, struct wl_surface* surface) {
    (void)data; (void)pointer; (void)serial; (void)surface;
    printf(">> [POINTER LEAVE]\n");
}

static void pointer_motion(void* data, struct wl_pointer* pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)pointer; (void)time;
    int px = wl_fixed_to_int(sx);
    int py = wl_fixed_to_int(sy);
    g_lastX = px;
    g_lastY = py;
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_pointer");
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Pointer Motion (%d, %d)", px, py);
    redraw();
}

static void pointer_button(void* data, struct wl_pointer* pointer,
                           uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)pointer; (void)serial; (void)time;
    printf(">> [POINTER BUTTON] button=0x%x, state=%u\n", button, state);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        g_tapCount++;
        snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_pointer (BTN 0x%x)", button);
        snprintf(g_lastEvent, sizeof(g_lastEvent), "Pointer Click (btn 0x%x)", button);
        redraw();
    }
}

static void pointer_axis(void* data, struct wl_pointer* pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

// wl_pointer v5+ events. The compositor binds wl_seat at v7, so these
// opcodes can arrive and libwayland aborts if the listener leaves them NULL.
static void pointer_frame(void* data, struct wl_pointer* pointer) { (void)data; (void)pointer; }
static void pointer_axis_source(void* data, struct wl_pointer* pointer, uint32_t source) {
    (void)data; (void)pointer; (void)source;
}
static void pointer_axis_stop(void* data, struct wl_pointer* pointer,
                              uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void* data, struct wl_pointer* pointer,
                                  uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}
static void pointer_axis_value120(void* data, struct wl_pointer* pointer,
                                  uint32_t axis, int32_t value120) {
    (void)data; (void)pointer; (void)axis; (void)value120;
}
static void pointer_axis_relative_direction(void* data, struct wl_pointer* pointer,
                                            uint32_t axis, uint32_t direction) {
    (void)data; (void)pointer; (void)axis; (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter                  = pointer_enter,
    .leave                  = pointer_leave,
    .motion                 = pointer_motion,
    .button                 = pointer_button,
    .axis                   = pointer_axis,
    .frame                  = pointer_frame,
    .axis_source            = pointer_axis_source,
    .axis_stop              = pointer_axis_stop,
    .axis_discrete          = pointer_axis_discrete,
    .axis_value120          = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

// ── wl_keyboard listener ────────────────────────────────────────────────────
static void keyboard_keymap(void* data, struct wl_keyboard* keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)data; (void)keyboard; (void)format;
    printf(">> [KEYBOARD KEYMAP] format=%u, size=%u bytes\n", format, size);
    close(fd);
}

static void keyboard_enter(void* data, struct wl_keyboard* keyboard,
                           uint32_t serial, struct wl_surface* surface, struct wl_array* keys) {
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
    printf(">> [KEYBOARD ENTER] surface focused\n");
}

static void keyboard_leave(void* data, struct wl_keyboard* keyboard,
                           uint32_t serial, struct wl_surface* surface) {
    (void)data; (void)keyboard; (void)serial; (void)surface;
    printf(">> [KEYBOARD LEAVE]\n");
}

static void keyboard_key(void* data, struct wl_keyboard* keyboard,
                         uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)keyboard; (void)serial; (void)time;
    printf(">> [KEYBOARD KEY] scancode=%u, state=%u\n", key, state);
    snprintf(g_lastInputType, sizeof(g_lastInputType), "wl_keyboard");
    snprintf(g_lastEvent, sizeof(g_lastEvent), "Key %u %s", key, state ? "DOWN" : "UP");
    redraw();
}

static void keyboard_modifiers(void* data, struct wl_keyboard* keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)data; (void)keyboard; (void)serial;
    (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
}

static void keyboard_repeat_info(void* data, struct wl_keyboard* keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data; (void)keyboard;
    printf(">> [KEYBOARD REPEAT INFO] rate=%d Hz, delay=%d ms\n", rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// ── wl_seat listener ────────────────────────────────────────────────────────
static void seat_capabilities(void* data, struct wl_seat* seat, uint32_t caps) {
    (void)data;
    printf(">> [SEAT CAPS] caps=0x%x\n", caps);

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !g_touch) {
        g_touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(g_touch, &touch_listener, NULL);
        printf(">> Bound wl_touch!\n");
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_pointer) {
        g_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer, &pointer_listener, NULL);
        printf(">> Bound wl_pointer!\n");
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_keyboard) {
        g_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_keyboard, &keyboard_listener, NULL);
        printf(">> Bound wl_keyboard!\n");
    }
}

static void seat_name(void* data, struct wl_seat* seat, const char* name) {
    (void)data; (void)seat;
    printf(">> [SEAT NAME] '%s'\n", name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

// ── wl_output listener — used to resolve --fullscreen to the real panel size ──
static void output_geometry(void* data, struct wl_output* output, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char* make, const char* model, int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}
static void output_mode(void* data, struct wl_output* output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)refresh;
    if (width > 0 && height > 0) {
        g_outputWidth  = width;
        g_outputHeight = height;
    }
}
static void output_done(void* data, struct wl_output* output) { (void)data; (void)output; }
static void output_scale(void* data, struct wl_output* output, int32_t factor) {
    (void)data; (void)output; (void)factor;
}
static void output_name(void* data, struct wl_output* output, const char* name) {
    (void)data; (void)output;
    printf(">> [OUTPUT NAME] '%s'\n", name);
}
static void output_description(void* data, struct wl_output* output, const char* desc) {
    (void)data; (void)output;
    printf(">> [OUTPUT DESC] '%s'\n", desc);
}

static const struct wl_output_listener output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

// ── Registry listener ───────────────────────────────────────────────────────
static void registry_handler(void* data, struct wl_registry* registry,
                             uint32_t id, const char* interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_compositor") == 0) {
        g_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        g_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        g_seat = wl_registry_bind(registry, id, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    } else if (strcmp(interface, "wl_output") == 0 && !g_output) {
        g_output = wl_registry_bind(registry, id, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(g_output, &output_listener, NULL);
    }
}

static void registry_remover(void* data, struct wl_registry* registry, uint32_t id) {
    (void)data; (void)registry; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_handler,
    .global_remove = registry_remover,
};

static int create_shm_file(off_t size) {
    int fd = syscall(__NR_memfd_create, "andwayland-input-shm", MFD_CLOEXEC);
    if (fd < 0) {
        char template[] = "/data/local/tmp/shm-XXXXXX";
        fd = mkstemp(template);
        if (fd >= 0) unlink(template);
    }
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

int main(int argc, char* argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int timeout_sec = 60;
    int argi = 1;

    // Geometry flags may appear before the positional timeout.
    for (; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--fullscreen") == 0) {
            g_fullscreen = 1;
        } else if (strcmp(argv[argi], "--size") == 0 && argi + 2 < argc) {
            W = atoi(argv[argi + 1]);
            H = atoi(argv[argi + 2]);
            argi += 2;
        } else {
            break;
        }
    }
    if (argi < argc) {
        timeout_sec = atoi(argv[argi]);
        if (timeout_sec <= 0) timeout_sec = 60;
    }

    // ── Connect, then resolve fullscreen against the advertised output ────────
    const char* display_name = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0";
    struct wl_display* display = wl_display_connect(display_name);
    if (!display) {
        fprintf(stderr, "ERROR: Cannot connect to Wayland display '%s'\n", display_name);
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!g_compositor || !g_shm || !g_seat) {
        fprintf(stderr, "ERROR: Missing required globals (compositor=%p, shm=%p, seat=%p)\n",
                g_compositor, g_shm, g_seat);
        return 1;
    }

    wl_display_roundtrip(display); // ensure seat listener fires capabilities

    if (g_fullscreen && g_outputWidth > 0 && g_outputHeight > 0) {
        W = g_outputWidth;
        H = g_outputHeight;
    }
    printf(">> Surface size: %dx%d%s\n", W, H, g_fullscreen ? " (fullscreen)" : "");

    const int size = W * H * 4;
    int fd = create_shm_file(size);
    if (fd < 0) {
        perror("create_shm_file");
        return 1;
    }

    g_pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, size);
    g_buffer = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    g_surface = wl_compositor_create_surface(g_compositor);
    drawStaticChrome();
    // First frame must be a full repaint so the compositor copies everything.
    wl_surface_attach(g_surface, g_buffer, 0, 0);
    wl_surface_damage(g_surface, 0, 0, W, H);
    wl_surface_commit(g_surface);
    wl_display_roundtrip(display);

    printf("✓ Interactive Window posted (%dx%d)! Listening for input for %d seconds...\n",
           W, H, timeout_sec);

    // wl_display_dispatch() already blocks until the socket has something to
    // read, so no polling sleep is needed — sleeping here would just add
    // latency to every event. The deadline is wall-clock, not iteration count.
    struct timespec start_ts, now_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    while (g_running) {
        if (wl_display_dispatch(display) < 0) break;

        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed_sec = (now_ts.tv_sec - start_ts.tv_sec) +
                           (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000000L;
        if (elapsed_sec >= timeout_sec) break;
    }

    printf("Cleaning up interactive window...\n");
    if (g_touch)    wl_touch_release(g_touch);
    if (g_pointer)  wl_pointer_release(g_pointer);
    if (g_keyboard) wl_keyboard_release(g_keyboard);
    if (g_seat)     wl_seat_release(g_seat);
    if (g_output)   wl_output_release(g_output);
    if (g_surface)  wl_surface_destroy(g_surface);
    if (g_buffer)   wl_buffer_destroy(g_buffer);
    if (registry)   wl_registry_destroy(registry);
    munmap(g_pixels, size);
    wl_display_disconnect(display);
    printf("Done.\n");
    return 0;
}
