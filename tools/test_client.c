/**
 * tools/test_client.c
 *
 * Standalone minimal Wayland client test for ANativeDrawer.
 * Connects to wayland-0, creates a shm buffer, renders a colored surface,
 * and commits it to verify end-to-end Wayland -> SurfaceFlinger display.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <wayland-client.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static struct wl_compositor* g_compositor = NULL;
static struct wl_shm*        g_shm        = NULL;

static void registry_handler(void* data, struct wl_registry* registry,
                             uint32_t id, const char* interface, uint32_t version) {
    printf("[Client] Discovered global: %s (v%u, id=%u)\n", interface, version, id);
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
    int fd = syscall(__NR_memfd_create, "andwayland-client-shm", MFD_CLOEXEC);
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
    printf("[Client] Connecting to Wayland compositor...\n");
    const char* display_name = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0";
    struct wl_display* display = wl_display_connect(display_name);
    if (!display) {
        fprintf(stderr, "[Client] ERROR: Failed to connect to Wayland display '%s'\n", display_name);
        return 1;
    }
    printf("[Client] Connected successfully!\n");

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!g_compositor || !g_shm) {
        fprintf(stderr, "[Client] ERROR: Missing wl_compositor or wl_shm\n");
        return 1;
    }

    const int width  = 400;
    const int height = 400;
    const int stride = width * 4;
    const int size   = stride * height;

    int fd = create_shm_file(size);
    if (fd < 0) {
        perror("[Client] create_shm_file failed");
        return 1;
    }

    uint32_t* pixel_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixel_data == MAP_FAILED) {
        perror("[Client] mmap failed");
        close(fd);
        return 1;
    }

    // Paint an Amber/Orange window with a golden border and inner cross
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x < 10 || x >= width - 10 || y < 10 || y >= height - 10) {
                pixel_data[y * width + x] = 0xFFFFD700; // Gold border
            } else if (abs(x - y) < 4 || abs((width - x) - y) < 4) {
                pixel_data[y * width + x] = 0xFFFFFFFF; // White 'X' pattern
            } else {
                pixel_data[y * width + x] = 0xFFFF6F00; // Vibrant Amber
            }
        }
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface* surface = wl_compositor_create_surface(g_compositor);
    printf("[Client] Attaching buffer to wl_surface...\n");
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);

    printf("[Client] Surface committed! Roundtripping events...\n");
    wl_display_roundtrip(display);

    printf("[Client] Test surface displayed. Keeping alive for 4 seconds...\n");
    sleep(4);

    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    munmap(pixel_data, size);
    wl_display_disconnect(display);
    printf("[Client] Finished test successfully.\n");
    return 0;
}
