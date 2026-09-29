/**
 * tools/hello_window.c
 *
 * Renders a desktop-style window containing "HELLO"
 * on Android SurfaceFlinger via ANativeDrawer.
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
        cur_x += 6 * scale; // 5 + 1 spacing
        str++;
    }
}

static int text_width(const char* str, int scale) {
    return (int)strlen(str) * 6 * scale;
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
                buf[(cy + y) * stride + (cx + x)] = color;
            }
        }
    }
}

static struct wl_compositor* g_compositor = NULL;
static struct wl_shm*        g_shm        = NULL;
static volatile sig_atomic_t g_running    = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void registry_handler(void* data, struct wl_registry* registry,
                             uint32_t id, const char* interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) {
        g_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        g_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
}

static void registry_remover(void* data, struct wl_registry* registry, uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    registry_handler,
    registry_remover
};

static int create_shm_file(off_t size) {
    int fd = syscall(__NR_memfd_create, "andwayland-hello-shm", MFD_CLOEXEC);
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

    int display_duration = 30; // seconds
    if (argc > 1) {
        display_duration = atoi(argv[1]);
        if (display_duration <= 0) display_duration = 30;
    }

    printf("=== ANativeDrawer Hello Window Client ===\n");
    const char* display_name = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0";
    struct wl_display* display = wl_display_connect(display_name);
    if (!display) {
        fprintf(stderr, "ERROR: Cannot connect to Wayland display '%s'\n", display_name);
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!g_compositor || !g_shm) {
        fprintf(stderr, "ERROR: Compositor does not support wl_compositor or wl_shm\n");
        return 1;
    }

    // ── Window Dimensions ────────────────────────────────────────────────────
    const int W = 560;
    const int H = 400;
    const int stride = W;
    const int size = W * H * 4;

    int fd = create_shm_file(size);
    if (fd < 0) {
        perror("create_shm_file");
        return 1;
    }

    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    // ── Draw Aesthetic Desktop Window ────────────────────────────────────────
    // 1. Dark Acrylic Background
    fill_rect(pixels, stride, 0, 0, W, H, 0xFF10121A);

    // 2. Window Border (2px Glowing Cyan/Indigo)
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) {
                pixels[y * stride + x] = 0xFF00E5FF; // Neon Cyan
            }
        }
    }

    // 3. Titlebar (Height: 46px, Darker Slate)
    fill_rect(pixels, stride, 2, 2, W - 4, 44, 0xFF1A1D28);

    // Titlebar bottom separator line
    for (int x = 2; x < W - 2; ++x) {
        pixels[46 * stride + x] = 0xFF2A2E3D;
    }

    // Window traffic lights
    draw_circle(pixels, stride, 22, 24, 6, 0xFFFF5F56); // Red Close
    draw_circle(pixels, stride, 42, 24, 6, 0xFFFFBD2E); // Yellow Min
    draw_circle(pixels, stride, 62, 24, 6, 0xFF27C93F); // Green Max

    // Titlebar Text
    const char* title = "ANativeDrawer  Wayland Window";
    draw_text(pixels, stride, 86, 19, title, 2, 0xFFE2E8F0);

    // 4. Hero Content Area: "HELLO"
    // Render "HELLO" in big bold font (scale = 7 -> 35x49 px per char!)
    const char* hello_text = "HELLO";
    int hello_w = text_width(hello_text, 7);
    int hello_x = (W - hello_w) / 2;
    int hello_y = 95;

    // Subtle drop shadow for "HELLO"
    draw_text(pixels, stride, hello_x + 3, hello_y + 3, hello_text, 7, 0xFF003847);
    // Vibrant Cyan "HELLO"
    draw_text(pixels, stride, hello_x, hello_y, hello_text, 7, 0xFF00E5FF);

    // 5. Subtitle: "Native Wayland on Android"
    const char* sub_text = "Native Wayland on Android";
    int sub_w = text_width(sub_text, 3);
    int sub_x = (W - sub_w) / 2;
    int sub_y = 190;
    draw_text(pixels, stride, sub_x, sub_y, sub_text, 3, 0xFFF8FAFC);

    // 6. Feature Badge Card
    fill_rect(pixels, stride, 40, 245, W - 80, 52, 0xFF161A26);
    // Border for badge
    for (int y = 245; y < 297; ++y) {
        for (int x = 40; x < W - 40; ++x) {
            if (x == 40 || x == W - 41 || y == 245 || y == 296) {
                pixels[y * stride + x] = 0xFF283045;
            }
        }
    }
    const char* badge1 = "Zero Java Overhead  Direct SurfaceFlinger";
    int b1_w = text_width(badge1, 2);
    draw_text(pixels, stride, (W - b1_w) / 2, 256, badge1, 2, 0xFF38BDF8);

    const char* badge2 = "60Hz Hardware Composited Layer";
    int b2_w = text_width(badge2, 2);
    draw_text(pixels, stride, (W - b2_w) / 2, 276, badge2, 2, 0xFF94A3B8);

    // 7. Footer Status
    draw_circle(pixels, stride, 48, 345, 5, 0xFF22C55E); // Green Status LED
    const char* footer = "WAYLAND_DISPLAY=wayland-0  LIVE";
    draw_text(pixels, stride, 62, 340, footer, 2, 0xFF4ADE80);

    // ── Create Wayland Buffer & Surface ──────────────────────────────────────
    struct wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, W, H, stride * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface* surface = wl_compositor_create_surface(g_compositor);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, W, H);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    printf("✓ Window 'HELLO' (560x400) successfully posted to Wayland & SurfaceFlinger!\n");
    printf("  Display duration: %d seconds (Press Ctrl+C to exit)\n", display_duration);

    for (int sec = 0; sec < display_duration && g_running; ++sec) {
        wl_display_dispatch_pending(display);
        sleep(1);
    }

    printf("Exiting hello window...\n");
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    munmap(pixels, size);
    wl_display_disconnect(display);
    return 0;
}
