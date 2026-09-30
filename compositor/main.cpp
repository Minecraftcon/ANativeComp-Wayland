/**
 * main.cpp
 *
 * ANativeDrawer — Wayland compositor on Android SurfaceFlinger
 *
 * Entry point. Initializes all subsystems in order and starts the
 * Wayland event loop.
 *
 * Run as:
 *   su -c 'WAYLAND_DEBUG=1 XDG_RUNTIME_DIR=/data/wayland andwayland'
 */

#include "WaylandServer.h"
#include "SurfaceBridge.h"
#include "SeatManager.h"
#include "ExtensionRegistry.h"
#include "../platform/SurfaceFlingerBridge.h"
#include "../platform/DisplayManager.h"

#include <android/log.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#define LOG_TAG "andwayland"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Global server pointer for signal handler
// ─────────────────────────────────────────────────────────────────────────────
static andwayland::WaylandServer* gServer = nullptr;
static pid_t gSatellitePid = -1;

static void signalHandler(int sig) {
    ALOGI("Received signal %d (%s), shutting down...", sig, strsignal(sig));
    if (gSatellitePid > 0) {
        kill(gSatellitePid, SIGTERM);
    }
    if (gServer) gServer->stop();
}

static uid_t detectTermuxUid() {
    struct stat st;
    if (stat("/data/data/com.termux/files", &st) == 0 && st.st_uid > 0) {
        return st.st_uid;
    }
    FILE* fp = popen("pm list packages -U com.termux 2>/dev/null", "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            unsigned int u = 0;
            if (sscanf(line, "package:com.termux uid:%u", &u) == 1) {
                pclose(fp);
                return static_cast<uid_t>(u);
            }
        }
        pclose(fp);
    }
    return 10296;
}

static pid_t startXwaylandSatellite(const char* socketName) {
    const char* satelliteBin = nullptr;
    if (access("/data/data/com.termux/files/usr/bin/xwayland-satellite", X_OK) == 0) {
        satelliteBin = "/data/data/com.termux/files/usr/bin/xwayland-satellite";
    } else if (access("/system/bin/xwayland-satellite", X_OK) == 0) {
        satelliteBin = "/system/bin/xwayland-satellite";
    } else if (access("/data/adb/modules/andwayland/system/bin/xwayland-satellite", X_OK) == 0) {
        satelliteBin = "/data/adb/modules/andwayland/system/bin/xwayland-satellite";
    }

    if (!satelliteBin) {
        ALOGI("xwayland-satellite binary not found; skipping automatic X11 bridge");
        return -1;
    }

    // Clean up stale locks/sockets before starting
    unlink("/data/wayland/xwls-1");
    unlink("/data/wayland/xwls-1.lock");
    unlink("/data/data/com.termux/files/usr/tmp/.X11-unix/X1");
    unlink("/data/data/com.termux/files/usr/tmp/.X11-unix/X1-lock");

    uid_t termuxUid = detectTermuxUid();

    pid_t pid = fork();
    if (pid < 0) {
        ALOGE("Failed to fork xwayland-satellite: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        if (getuid() == 0 && termuxUid > 0) {
            setgid(termuxUid);
            setuid(termuxUid);
        }

        setenv("PATH", "/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin", 1);
        setenv("LD_LIBRARY_PATH", "/data/data/com.termux/files/usr/lib:/system/lib64", 1);
        setenv("XDG_RUNTIME_DIR", "/data/wayland", 1);
        setenv("WAYLAND_DISPLAY", socketName, 1);
        setenv("DISPLAY", ":1", 1);

        int logFd = open("/data/data/com.termux/files/home/satellite.log",
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }

        execl(satelliteBin, "xwayland-satellite", ":1", nullptr);
        _exit(127);
    }

    FILE* fpX = fopen("/data/wayland/.xdisplay", "w");
    if (fpX) {
        fputs(":1\n", fpX);
        fclose(fpX);
        chmod("/data/wayland/.xdisplay", 0666);
    }

    ALOGI("Started xwayland-satellite supervisor (PID %d, UID %u) for DISPLAY=:1", pid, termuxUid);
    return pid;
}

// ─────────────────────────────────────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    ALOGI("ANativeDrawer v" ANDWAYLAND_VERSION " starting");
    ALOGI("Built: %s %s", __DATE__, __TIME__);

    // ── Parse arguments ───────────────────────────────────────────────────────
    const char* socketName = "wayland-0";
    bool testMode = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socketName = argv[++i];
        } else if (strcmp(argv[i], "--test") == 0) {
            testMode = true;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("ANativeDrawer %s\n", ANDWAYLAND_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: andwayland [--socket <name>] [--test] [--version] [--help]\n");
            printf("\n");
            printf("  --socket <name>  Wayland socket name (default: wayland-0)\n");
            printf("  --test           Display an on-screen test pattern layer\n");
            printf("                   Socket is created in XDG_RUNTIME_DIR\n");
            printf("                   (default: /data/wayland)\n");
            return 0;
        }
    }

    // ── Signal handling ───────────────────────────────────────────────────────
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGHUP,  signalHandler);

    // ── 1. Connect to SurfaceFlinger ──────────────────────────────────────────
    auto& sfBridge = andwayland::SurfaceFlingerBridge::get();
    if (!sfBridge.init()) {
        ALOGE("Failed to connect to SurfaceFlinger. "
              "Make sure the process runs as root or with 'graphics' group.");
        return 1;
    }

    // ── 2. Create subsystems ──────────────────────────────────────────────────
    auto bridge = std::make_shared<andwayland::SurfaceBridge>(sfBridge);
    auto seat   = std::make_shared<andwayland::SeatManager>();
    auto ext    = std::make_shared<andwayland::ExtensionRegistry>();

    if (!seat->init()) {
        ALOGE("SeatManager init failed — no input will be forwarded");
        // Non-fatal: compositor still works without input
    }

    // ── 3. Initialize Wayland server ──────────────────────────────────────────
    andwayland::WaylandServer server;
    gServer = &server;

    if (!server.init(socketName, bridge, seat, ext)) {
        ALOGE("Failed to initialize Wayland server");
        sfBridge.shutdown();
        return 1;
    }

    // ── Optional on-screen test pattern ──────────────────────────────────────
    andwayland::SFLayerHandle testLayer;
    if (testMode) {
        ALOGI("=== Running SurfaceFlinger Test Layer ===");
        auto info = sfBridge.getDisplayInfo();
        ALOGI("Display: %dx%d (orientation=%d)", info.width, info.height, info.orientation);

        int32_t layerW = (info.width > 0) ? std::min(info.width, 600) : 500;
        int32_t layerH = (info.height > 0) ? std::min(info.height, 600) : 500;
        testLayer = sfBridge.createLayer("ANativeDrawer-TestPattern", layerW, layerH, 2000000);
        if (testLayer) {
            andwayland::SFLockedBuffer buf{};
            if (sfBridge.lockBuffer(testLayer, buf)) {
                ALOGI("Locked test buffer: %dx%d stride=%d fmt=%d", buf.width, buf.height, buf.stride, buf.format);
                uint32_t* pixels = static_cast<uint32_t*>(buf.bits);
                for (int y = 0; y < buf.height; ++y) {
                    for (int x = 0; x < buf.width; ++x) {
                        if (x < 14 || x >= buf.width - 14 || y < 14 || y >= buf.height - 14) {
                            pixels[y * buf.stride + x] = 0xFFFF0044; // Bright Red Border
                        } else if (x < 24 || x >= buf.width - 24 || y < 24 || y >= buf.height - 24) {
                            pixels[y * buf.stride + x] = 0xFFFFFFFF; // White inner border
                        } else {
                            // Cyan to Blue Gradient
                            uint8_t g = static_cast<uint8_t>(200 + (y * 55) / buf.height);
                            uint8_t b = static_cast<uint8_t>(255);
                            uint8_t r = static_cast<uint8_t>((x * 128) / buf.width);
                            pixels[y * buf.stride + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
                        }
                    }
                }
                sfBridge.unlockAndPost(testLayer);
                ALOGI("✓ Visual test layer successfully posted to SurfaceFlinger!");
            } else {
                ALOGE("Failed to lock buffer for test layer");
            }
        } else {
            ALOGE("Failed to create test layer");
        }
    }

    ALOGI("Wayland compositor ready.");
    ALOGI("  Socket:  %s/%s", getenv("XDG_RUNTIME_DIR") ?: "/data/wayland", socketName);
    ALOGI("  Display: %dx%d", bridge->displayWidth(), bridge->displayHeight());
    ALOGI("Set WAYLAND_DISPLAY=%s in client environment.", socketName);

    // Export telemetry for system companion UI
    mkdir("/data/wayland", 0777);
    FILE* fpStatus = fopen("/data/wayland/status.json", "w");
    if (fpStatus) {
        fprintf(fpStatus, "{\n  \"pid\": %d,\n  \"socket\": \"%s\",\n  \"width\": %d,\n  \"height\": %d,\n  \"activeSurfaces\": 0\n}\n",
                getpid(), socketName, bridge->displayWidth(), bridge->displayHeight());
        fclose(fpStatus);
        chmod("/data/wayland/status.json", 0666);
    }

    // ── 3.5. Start rootless Xwayland satellite bridge ────────────────────────
    gSatellitePid = startXwaylandSatellite(socketName);

    // ── 4. Run Wayland event loop (main thread blocks here; evdev events are dispatched via wl_event_loop fds) ──
    server.run();

    // ── 5. Cleanup ────────────────────────────────────────────────────────────
    ALOGI("Shutting down...");
    if (gSatellitePid > 0) {
        ALOGI("Stopping xwayland-satellite (PID %d)...", gSatellitePid);
        kill(gSatellitePid, SIGTERM);
        waitpid(gSatellitePid, nullptr, WNOHANG);
        gSatellitePid = -1;
    }
    unlink("/data/wayland/status.json");
    unlink("/data/wayland/.xdisplay");
    if (testLayer) sfBridge.destroyLayer(testLayer);
    seat->stop();
    sfBridge.shutdown();

    ALOGI("Goodbye.");
    return 0;
}
