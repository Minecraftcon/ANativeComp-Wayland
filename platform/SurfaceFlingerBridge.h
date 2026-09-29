#pragma once
/**
 * SurfaceFlingerBridge.h
 *
 * Wraps Android's SurfaceComposerClient / SurfaceControl API.
 * This is the single point of contact between the Wayland compositor
 * and SurfaceFlinger.
 *
 * All SurfaceFlinger-specific types are kept inside this file so
 * the rest of the codebase never includes libgui/libui headers directly.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace andwayland {

/**
 * Integer rectangle in layer pixels, half-open on the right/bottom edge.
 * Mirrors android::Rect but keeps AOSP types out of this header.
 */
struct android_rect_t {
    int32_t left = 0;
    int32_t top  = 0;
    int32_t right  = 0;
    int32_t bottom = 0;
};

/**
 * Display geometry as reported by SurfaceFlinger.
 */
struct DisplayInfo {
    int32_t width;
    int32_t height;
    float   xdpi;
    float   ydpi;
    float   refreshRate;
    int32_t orientation; // Surface::ROTATION_0/90/180/270
};

/**
 * Opaque handle to a SurfaceFlinger layer (SurfaceControl).
 * Obtained via SurfaceFlingerBridge::createLayer().
 * Must be destroyed via SurfaceFlingerBridge::destroyLayer().
 */
struct SFLayer;
using SFLayerHandle = std::shared_ptr<SFLayer>;

/**
 * Buffer locked for CPU write access.
 * Matches ANativeWindow_Buffer layout so callers can memcpy directly.
 */
struct SFLockedBuffer {
    void*   bits;    ///< Pointer to the first pixel
    int32_t width;
    int32_t height;
    int32_t stride;  ///< Row stride in pixels (may be > width)
    int32_t format;  ///< PIXEL_FORMAT_RGBA_8888 etc.
};

// ────────────────────────────────────────────────────────────────────────────

/**
 * SurfaceFlingerBridge
 *
 * Singleton-style facade over SurfaceComposerClient.
 * Call init() once at startup; all other methods are thread-safe after that.
 */
class SurfaceFlingerBridge {
public:
    static SurfaceFlingerBridge& get();

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /** Connect to SurfaceFlinger. Must be called before anything else. */
    bool init();

    /** Disconnect and release all layers. */
    void shutdown();

    // ── Display ──────────────────────────────────────────────────────────────

    /** Get primary display geometry. */
    DisplayInfo getDisplayInfo() const;

    // ── Layer Management ─────────────────────────────────────────────────────

    /**
     * Create a new composited layer.
     *
     * @param name    Debug label visible in `dumpsys SurfaceFlinger`
     * @param width   Initial width  (can be changed later via setGeometry)
     * @param height  Initial height
     * @param zOrder  Z-order; higher = above other layers
     * @param parent  Optional parent layer for child/popup surfaces
     */
    SFLayerHandle createLayer(const std::string& name,
                              int32_t width, int32_t height,
                              int32_t zOrder,
                              SFLayerHandle parent = nullptr);

    /** Destroy a layer and remove it from the display. */
    void destroyLayer(SFLayerHandle& layer);

    /** Resize layer and its internal BufferQueue dimensions */
    bool resizeLayer(SFLayerHandle layer, int32_t width, int32_t height);

    // ── Layer Geometry Transactions ───────────────────────────────────────────

    struct LayerParams {
        int32_t x       = 0;
        int32_t y       = 0;
        int32_t width   = -1; // -1 = keep current
        int32_t height  = -1;
        int32_t zOrder  = -1;
        float   alpha   = 1.0f;
        bool    visible = true;
    };

    /**
     * Apply geometry/visibility changes to one or more layers atomically.
     * Collects changes and submits a single Transaction::apply() call.
     */
    class Transaction {
    public:
        Transaction& setParams(SFLayerHandle layer, const LayerParams& p);
        Transaction& setPosition(SFLayerHandle layer, int32_t x, int32_t y);
        Transaction& setSize(SFLayerHandle layer, int32_t w, int32_t h);
        Transaction& setZOrder(SFLayerHandle layer, int32_t z);
        Transaction& setAlpha(SFLayerHandle layer, float alpha);
        Transaction& show(SFLayerHandle layer);
        Transaction& hide(SFLayerHandle layer);
        void apply();

    private:
        struct Change {
            SFLayerHandle layer;
            LayerParams   params;
            bool hasPosition = false;
            bool hasSize     = false;
            bool hasZOrder   = false;
            bool hasAlpha    = false;
            bool hasVisible  = false;
        };
        std::vector<Change> mChanges;
    };

    // ── CPU Buffer Operations (wl_shm path) ──────────────────────────────────

    /**
     * Lock the layer's front buffer for CPU write access.
     * After writing, call unlockAndPost() to push the frame to SurfaceFlinger.
     *
     * Only one lock per layer at a time.
     *
     * @param dirty  Optional damage region in layer pixels. Passing the actual
     *               damaged rect lets BufferQueue preserve the rest of the
     *               buffer, so SurfaceFlinger only recomposites that region.
     *               Pass nullptr for a full-surface redraw.
     */
    bool lockBuffer(SFLayerHandle layer, SFLockedBuffer& outBuffer,
                    const android_rect_t* dirty = nullptr);

    /**
     * Like lockBuffer(), but also reports the region that actually changed
     * after the copy (intersection of @p dirty with the destination bounds).
     * Lets the caller clamp a damage rect that overruns the layer.
     */
    struct DirtyRect {
        int32_t left = 0, top = 0, right = 0, bottom = 0;  ///< half-open
        bool    valid = false;
    };
    bool lockBuffer(SFLayerHandle layer, SFLockedBuffer& outBuffer,
                    const android_rect_t* dirty, DirtyRect* outClipped);

    /**
     * Unlock and post the buffer to SurfaceFlinger.
     * The frame becomes visible on the next vsync.
     */
    bool unlockAndPost(SFLayerHandle layer);

    // ── GPU/DMA-BUF Buffer Operations (zwp_linux_dmabuf_v1 path) ────────────

    /**
     * Import a DMA-BUF fd as an AHardwareBuffer and attach it to a layer.
     * Zero-copy: SurfaceFlinger reads the buffer directly from the GPU.
     *
     * @param dmaBufFd   File descriptor from the Wayland DMA-BUF protocol
     * @param width      Buffer width
     * @param height     Buffer height
     * @param format     HAL_PIXEL_FORMAT_RGBA_8888 etc.
     * @param modifier   DRM format modifier (DRM_FORMAT_MOD_LINEAR etc.)
     */
    bool attachDmaBuf(SFLayerHandle layer,
                      int           dmaBufFd,
                      int32_t       width,
                      int32_t       height,
                      uint32_t      format,
                      uint64_t      modifier);

private:
    SurfaceFlingerBridge() = default;
    ~SurfaceFlingerBridge();

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace andwayland
