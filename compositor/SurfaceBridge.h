#pragma once
/**
 * SurfaceBridge.h
 *
 * Core mapping layer: each Wayland wl_surface becomes one SurfaceFlinger layer.
 *
 * Lifecycle:
 *   1. Client calls wl_compositor.create_surface → createSurface()
 *   2. Client attaches a wl_buffer and calls commit → commitSurface()
 *   3. SurfaceBridge blits the buffer into the SurfaceControl layer
 *   4. SurfaceFlinger composites the layer onto the display
 */

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

struct wl_client;
struct wl_resource;
struct wl_display;

namespace andwayland {

class SeatManager;
class SurfaceBridge;
class SurfaceFlingerBridge;
struct SFLayer;
using SFLayerHandle = std::shared_ptr<SFLayer>;

// Positioner data for xdg_positioner
struct PositionerData {
    int32_t width = 0;
    int32_t height = 0;
    int32_t anchorX = 0;
    int32_t anchorY = 0;
    int32_t anchorW = 0;
    int32_t anchorH = 0;
    uint32_t anchor = 0;
    uint32_t gravity = 0;
    uint32_t constraintAdjustment = 0;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    bool reactive = false;
};

// ────────────────────────────────────────────────────────────────────────────
// WaylandSurface — internal state per wl_surface
// ────────────────────────────────────────────────────────────────────────────
struct WaylandSurface {
    uint32_t      id;
    wl_resource*  resource = nullptr;
    SFLayerHandle sfLayer;

    // Pending state (applied on commit)
    struct Pending {
        wl_resource* buffer   = nullptr; // wl_buffer currently attached
        int32_t      dx       = 0;
        int32_t      dy       = 0;
        bool         hasBuffer = false;

        // Accumulated damage since the last commit, in buffer pixels.
        // Union of all wl_surface.damage / damage_buffer calls. An empty rect
        // means "full surface", which is also what an uncached buffer needs.
        struct Rect {
            int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            bool    valid = false;

            void add(int32_t x, int32_t y, int32_t w, int32_t h) {
                if (w <= 0 || h <= 0) return;
                const int32_t rx = x + w, ry = y + h;
                if (!valid) { x1 = x; y1 = y; x2 = rx; y2 = ry; valid = true; return; }
                x1 = std::min(x1, x);  y1 = std::min(y1, y);
                x2 = std::max(x2, rx); y2 = std::max(y2, ry);
            }
            void clear() { valid = false; }
        } damage;
    } pending;

    // Current committed state
    struct Committed {
        int32_t x       = 0;
        int32_t y       = 0;
        int32_t width   = 0;
        int32_t height  = 0;
        bool    mapped  = false;
    } committed;

    // XDG shell state
    wl_resource*  xdgSurface  = nullptr;
    wl_resource*  xdgToplevel = nullptr;
    wl_resource*  xdgPopup    = nullptr;
    std::string   title;
    std::string   appId;
    bool          isFullscreen = false;

    // Xwayland shell state
    bool          isXwayland      = false;
    uint64_t      xwaylandSerial  = 0;
    wl_resource*  xwaylandSurface = nullptr;

    // Subsurface relationship
    WaylandSurface*              parentSurface = nullptr;
    std::vector<WaylandSurface*> subsurfaces;
    int32_t                      subX = 0;
    int32_t                      subY = 0;
    bool                         isSubsurface = false;

    // Popup relationship
    bool                         isPopup = false;
    int32_t                      popupX = 0;
    int32_t                      popupY = 0;
    int32_t                      popupW = 0;
    int32_t                      popupH = 0;
    std::vector<WaylandSurface*> popups;

    // Frame callbacks (for client-side frame pacing)
    std::vector<wl_resource*> frameCallbacks;

    // Z-order assigned by window manager
    int32_t zOrder = 0x1000;

    // Server-side decoration state
    SFLayerHandle         decorLayer;
    int32_t               decorHeight = 36;
    bool                  hasDecor    = false;
    bool                  isCursor    = false;
    SurfaceBridge*        bridge      = nullptr;
    std::vector<uint32_t> decorBuffer;

    // Compositor-owned copy of the last presented frame.
    //
    // Surface::lock() hands back an arbitrary buffer from BufferQueue whose
    // contents are undefined, so a partial copy into it leaves the untouched
    // region filled with stale garbage (visible as flickering/clipping). We
    // instead keep a persistent image here, merge only the damaged rect into
    // it, and present the whole thing — so damage tracking stays cheap while
    // the buffer handed to SurfaceFlinger is always complete.
    std::vector<uint32_t> backBuffer;
    int32_t  backStride  = 0;   ///< in pixels
    int32_t  backWidth   = 0;
    int32_t  backHeight  = 0;
};

// ────────────────────────────────────────────────────────────────────────────

class SurfaceBridge {
public:
    explicit SurfaceBridge(SurfaceFlingerBridge& sfBridge);
    ~SurfaceBridge();

    // ── Called by WaylandServer global handlers ───────────────────────────────

    /** wl_compositor.create_surface */
    void createSurface(wl_client* client, uint32_t id, int version = 5);

    /** wl_shm.create_pool */
    void createShmPool(wl_client* client, uint32_t id, int fd, int32_t size);

    /** xdg_wm_base.get_xdg_surface */
    void getXdgSurface(wl_client* client, uint32_t id, wl_resource* surfaceResource, int version = 5);

    /** wl_output binding */
    void bindOutput(wl_client* client, uint32_t version, uint32_t id);

    // ── Called internally by protocol handlers ────────────────────────────────

    /** Called when wl_surface.commit is received */
    void commitSurface(WaylandSurface* surface);

    /** Called when xdg_toplevel title/app_id are set */
    void setToplevelTitle(WaylandSurface* surface, const std::string& title);

    /** Called when xdg_toplevel requests fullscreen */
    void setFullscreen(WaylandSurface* surface, bool fullscreen);

    /** Destroy a wl_surface and its SurfaceFlinger layer */
    void destroySurface(WaylandSurface* surface);

    /** Lookup a WaylandSurface by wl_resource */
    WaylandSurface* surfaceFromResource(wl_resource* resource);

    /**
     * Hit-test: find the topmost mapped surface containing the given screen coordinates.
     * Optionally returns the coordinates relative to the surface top-left.
     */
    WaylandSurface* surfaceAt(int32_t screenX, int32_t screenY,
                              int32_t* outLocalX = nullptr,
                              int32_t* outLocalY = nullptr);

    void setSeatManager(SeatManager* seat) { mSeat = seat; }
    SeatManager* getSeatManager() const { return mSeat; }

    // ── Server-side Window Decorations (Material/Windows Style) ───────────────
    void bindXdgDecoration(wl_client* client, uint32_t version, uint32_t id);
    void getToplevelDecoration(wl_client* client, uint32_t id, wl_resource* toplevel);
    void updateDecor(WaylandSurface* surface);

    // ── Xwayland Shell (Rootless X11 Window Integration) ─────────────────────
    void bindXwaylandShell(wl_client* client, uint32_t version, uint32_t id);
    void getXwaylandSurface(wl_client* client, uint32_t id, wl_resource* surfaceResource, int version = 1);

    // ── Window Controls ───────────────────────────────────────────────────────
    void toggleMaximize(WaylandSurface* surface);
    void minimizeSurface(WaylandSurface* surface);
    void closeSurface(WaylandSurface* surface);

    // ── Window Focus & Activation (Wayland xdg-shell standard) ───────────────
    void activateSurface(WaylandSurface* surface);
    WaylandSurface* getActiveSurface() const { return mActiveSurface; }

    // ── Window Movement (Touch & Pointer Drag Grab) ───────────────────────────
    void moveSurface(WaylandSurface* surface, int32_t newX, int32_t newY);
    void startMoveGrab(WaylandSurface* surface, int32_t screenX, int32_t screenY);
    void updateMoveGrab(int32_t screenX, int32_t screenY);
    void endMoveGrab();
    bool isMoveGrabActive() const { return mGrabSurface != nullptr; }

    // Display info (forwarded from SurfaceFlingerBridge)
    int32_t displayWidth()    const { return mDisplayWidth; }
    int32_t displayHeight()   const { return mDisplayHeight; }
    int32_t displayWidthMm()  const { return mDisplayWidthMm; }
    int32_t displayHeightMm() const { return mDisplayHeightMm; }
    float   refreshRate()     const { return mRefreshRate; }
    SurfaceFlingerBridge& getSurfaceFlingerBridge() { return mSfBridge; }
    void destroyLayerForSurface(WaylandSurface* surface);

private:
    // ── Buffer handling ───────────────────────────────────────────────────────

    /** Blit a wl_shm_buffer into the SurfaceFlinger layer (CPU path) */
    bool blitShmBuffer(WaylandSurface* surface, wl_resource* buffer);

    /** Attach a DMA-BUF buffer to the layer (GPU zero-copy path) */
    bool attachDmaBuf(WaylandSurface* surface, wl_resource* buffer);

    // ── Z-order management ────────────────────────────────────────────────────
    int32_t allocateZOrder();

    SurfaceFlingerBridge& mSfBridge;
    int32_t               mDisplayWidth    = 0;
    int32_t               mDisplayHeight   = 0;
    int32_t               mDisplayWidthMm  = 0;
    int32_t               mDisplayHeightMm = 0;
    float                 mRefreshRate     = 60.0f;
    int32_t               mNextZOrder      = 2000000;

    SeatManager*          mSeat          = nullptr;
    WaylandSurface*       mActiveSurface = nullptr;

    // Active drag grab state
    WaylandSurface*       mGrabSurface   = nullptr;
    int32_t               mGrabStartX    = 0;
    int32_t               mGrabStartY    = 0;
    int32_t               mWindowStartX  = 0;
    int32_t               mWindowStartY  = 0;

    // Map: wl_resource* → WaylandSurface
    std::unordered_map<wl_resource*, std::unique_ptr<WaylandSurface>> mSurfaces;
};

} // namespace andwayland
