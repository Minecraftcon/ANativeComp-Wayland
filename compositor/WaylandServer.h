#pragma once
/**
 * WaylandServer.h
 *
 * Creates and manages the Wayland display socket.
 * Registers all global protocol objects (wl_compositor, wl_shm, xdg_wm_base,
 * wl_seat, etc.) and runs the main event loop.
 */

#include <functional>
#include <memory>
#include <string>

// Forward-declare libwayland types to avoid leaking them into other headers
struct wl_display;
struct wl_event_loop;
struct wl_event_source;
struct wl_client;

namespace andwayland {

class SurfaceBridge;
class SeatManager;
class ExtensionRegistry;

// ────────────────────────────────────────────────────────────────────────────

class WaylandServer {
public:
    WaylandServer();
    ~WaylandServer();

    // Non-copyable
    WaylandServer(const WaylandServer&) = delete;
    WaylandServer& operator=(const WaylandServer&) = delete;

    /**
     * Initialize the server.
     *
     * @param socketName  Name of the Unix socket, e.g. "wayland-0".
     *                    The actual socket will be created at
     *                    $XDG_RUNTIME_DIR/<socketName> or /data/wayland/<socketName>.
     * @param bridge      Pre-initialized SurfaceFlingerBridge facade.
     */
    bool init(const std::string& socketName,
              std::shared_ptr<SurfaceBridge>      bridge,
              std::shared_ptr<SeatManager>        seat,
              std::shared_ptr<ExtensionRegistry>  ext);

    /**
     * Run the Wayland event loop (blocks until stop() is called).
     * Typically called from a dedicated thread.
     */
    void run();

    /**
     * Signal the event loop to exit cleanly.
     * Thread-safe.
     */
    void stop();

    /**
     * Return the socket name currently in use (e.g., "wayland-0").
     */
    const std::string& socketName() const { return mSocketName; }

    /**
     * Return the underlying wl_display* for use by subsystems that need it
     * (e.g., XWayland, protocol implementations).
     */
    wl_display* display() const { return mDisplay; }

private:
    // ── Protocol global registration ──────────────────────────────────────────
    void registerCompositorGlobal();
    void registerShmGlobal();
    void registerXdgWmBaseGlobal();
    void registerSeatGlobal();
    void registerOutputGlobal();
    void registerSubcompositorGlobal();
    void registerDataDeviceManagerGlobal();
    void registerViewporterGlobal();
    void registerXdgDecorationGlobal();
    void registerXwaylandShellGlobal();
#ifdef ENABLE_DMABUF
    void registerLinuxDmaBufGlobal();
#endif
#ifdef ENABLE_LAYER_SHELL
    void registerLayerShellGlobal();
#endif

    // ── Client lifecycle ─────────────────────────────────────────────────────
    static void onClientCreated(wl_display* display, void* data, wl_client* client);
    static void onClientDestroyed(wl_client* client, void* data);

    wl_display*    mDisplay    = nullptr;
    wl_event_loop* mEventLoop  = nullptr;
    std::string    mSocketName;
    bool           mRunning    = false;

    // Pipe used to wake the event loop from stop()
    int mWakeReadFd  = -1;
    int mWakeWriteFd = -1;
    struct wl_event_source* mWakeSource = nullptr;
    std::shared_ptr<SurfaceBridge>     mBridge;
    std::shared_ptr<SeatManager>       mSeat;
    std::shared_ptr<ExtensionRegistry> mExt;
};

} // namespace andwayland
