/**
 * SurfaceFlingerBridge.cpp
 *
 * Implementation of the SurfaceFlinger abstraction layer.
 *
 * This file includes Android private platform headers (libgui, libui).
 * It is the ONLY file in the project that touches these APIs.
 *
 * Linking note:
 *   libgui.so and libui.so are linked at runtime from /system/lib64/.
 *   They are NOT part of the public NDK. We reference them via
 *   -Wl,--allow-shlib-undefined during build and rely on them being
 *   present on any Android 12+ device with a SurfaceFlinger.
 */

// ── Android Platform Headers (private / AOSP internal) ───────────────────────
// These come from the AOSP tree or a platform sysroot, not the public NDK.
#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayState.h>
#include <ui/DisplayMode.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <android/native_window.h>
// ─────────────────────────────────────────────────────────────────────────────

#include "SurfaceFlingerBridge.h"

#include <android/log.h>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "andwayland:SF"

#ifdef ALOGI
#undef ALOGI
#endif
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

#ifdef ALOGE
#undef ALOGE
#endif
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef ALOGW
#undef ALOGW
#endif
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace andwayland {

// ────────────────────────────────────────────────────────────────────────────
// SFLayer — concrete type behind SFLayerHandle
// ────────────────────────────────────────────────────────────────────────────
struct SFLayer {
    android::sp<android::SurfaceControl> surfaceControl;
    android::sp<android::Surface>        surface;

    // For CPU lock/post path (wl_shm)
    ANativeWindow_Buffer lockedBuffer{};
    bool                 isLocked = false;

    // For DMA-BUF path
    AHardwareBuffer*     dmaBufHwb = nullptr;

    ~SFLayer() {
        if (dmaBufHwb) {
            AHardwareBuffer_release(dmaBufHwb);
            dmaBufHwb = nullptr;
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
// SurfaceFlingerBridge::Impl
// ────────────────────────────────────────────────────────────────────────────
struct SurfaceFlingerBridge::Impl {
    android::sp<android::SurfaceComposerClient> client;
    android::PhysicalDisplayId                  primaryDisplayId;
    std::mutex                                  mu;
};

// ────────────────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────────────────
SurfaceFlingerBridge& SurfaceFlingerBridge::get() {
    static SurfaceFlingerBridge instance;
    return instance;
}

SurfaceFlingerBridge::~SurfaceFlingerBridge() {
    shutdown();
}

// ────────────────────────────────────────────────────────────────────────────
// init()
// ────────────────────────────────────────────────────────────────────────────
bool SurfaceFlingerBridge::init() {
    mImpl = std::make_unique<Impl>();

    mImpl->client = new android::SurfaceComposerClient();

    android::status_t status = mImpl->client->initCheck();
    if (status != android::NO_ERROR) {
        ALOGE("SurfaceComposerClient::initCheck() failed: %d", status);
        mImpl.reset();
        return false;
    }

    // Grab the primary physical display token
    const auto displayIds = android::SurfaceComposerClient::getPhysicalDisplayIds();
    if (displayIds.empty()) {
        ALOGE("No physical displays found via SurfaceFlinger");
        mImpl.reset();
        return false;
    }
    mImpl->primaryDisplayId = displayIds[0];

    ALOGI("Connected to SurfaceFlinger. Primary display: %" PRIu64,
          mImpl->primaryDisplayId.value);
    return true;
}

void SurfaceFlingerBridge::shutdown() {
    if (!mImpl) return;
    mImpl->client.clear();
    mImpl.reset();
    ALOGI("SurfaceFlinger connection closed.");
}

// ────────────────────────────────────────────────────────────────────────────
// getDisplayInfo()
// ────────────────────────────────────────────────────────────────────────────
DisplayInfo SurfaceFlingerBridge::getDisplayInfo() const {
    DisplayInfo info{};
    if (!mImpl) return info;

    android::sp<android::IBinder> token =
        android::SurfaceComposerClient::getPhysicalDisplayToken(mImpl->primaryDisplayId);

    android::ui::DisplayState state;
    int32_t layerStackW = 0, layerStackH = 0;
    if (android::SurfaceComposerClient::getDisplayState(token, &state) == android::NO_ERROR) {
        layerStackW      = state.layerStackSpaceRect.getWidth();
        layerStackH      = state.layerStackSpaceRect.getHeight();
        info.orientation = static_cast<int32_t>(state.orientation);
    }

    android::ui::DisplayMode activeMode;
    if (android::SurfaceComposerClient::getActiveDisplayMode(token, &activeMode) == android::NO_ERROR) {
        info.width       = (layerStackW > 0) ? layerStackW : activeMode.resolution.width;
        info.height      = (layerStackH > 0) ? layerStackH : activeMode.resolution.height;
        info.xdpi        = activeMode.xDpi;
        info.ydpi        = activeMode.yDpi;
        info.refreshRate = activeMode.refreshRate;
    }

    return info;
}

// ────────────────────────────────────────────────────────────────────────────
// createLayer()
// ────────────────────────────────────────────────────────────────────────────
SFLayerHandle SurfaceFlingerBridge::createLayer(const std::string& name,
                                                int32_t width, int32_t height,
                                                int32_t zOrder,
                                                SFLayerHandle parentHandle) {
    if (!mImpl) return nullptr;

    android::sp<android::IBinder> parentHandleBinder = nullptr;
    if (parentHandle && parentHandle->surfaceControl) {
        parentHandleBinder = parentHandle->surfaceControl->getHandle();
    }

    android::sp<android::SurfaceControl> sc =
        mImpl->client->createSurface(
            android::String8(name.c_str()),
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            android::PIXEL_FORMAT_RGBA_8888,
            0,      // flags
            parentHandleBinder
        );

    if (!sc) {
        ALOGE("createSurface() returned null for '%s'", name.c_str());
        return nullptr;
    }

    // Apply initial Z-order and make visible with full crop
    android::SurfaceComposerClient::Transaction{}
        .setLayer(sc, zOrder)
        .setCrop(sc, android::Rect(0, 0, width, height))
        .show(sc)
        .apply();

    auto layer = std::make_shared<SFLayer>();
    layer->surfaceControl = sc;
    layer->surface = sc->getSurface();

    ALOGI("Layer '%s' created: %dx%d z=%d", name.c_str(), width, height, zOrder);
    return layer;
}

void SurfaceFlingerBridge::destroyLayer(SFLayerHandle& layer) {
    if (!layer) return;
    if (layer->surfaceControl) {
        android::SurfaceComposerClient::Transaction{}
            .hide(layer->surfaceControl)
            .apply();
        layer->surfaceControl.clear();
    }
    layer.reset();
}

bool SurfaceFlingerBridge::resizeLayer(SFLayerHandle layer, int32_t width, int32_t height) {
    if (!layer || !layer->surfaceControl) return false;
    layer->surfaceControl->updateDefaultBufferSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    android::SurfaceComposerClient::Transaction{}
        .setSize(layer->surfaceControl, static_cast<uint32_t>(width), static_cast<uint32_t>(height))
        .setCrop(layer->surfaceControl, android::Rect(0, 0, width, height))
        .apply();
    if (layer->surface) {
        layer->surface->setBuffersDimensions(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
    ALOGI("Layer resized to %dx%d (crop and BBQ updated)", width, height);
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Transaction
// ────────────────────────────────────────────────────────────────────────────
SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::setParams(SFLayerHandle layer, const LayerParams& p) {
    Change c;
    c.layer = layer;
    c.params = p;
    c.hasPosition = (p.x != 0 || p.y != 0);
    c.hasSize     = (p.width > 0 && p.height > 0);
    c.hasZOrder   = (p.zOrder >= 0);
    c.hasAlpha    = (p.alpha < 1.0f);
    c.hasVisible  = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::setPosition(SFLayerHandle layer, int32_t x, int32_t y) {
    Change c;
    c.layer = layer;
    c.params.x = x;
    c.params.y = y;
    c.hasPosition = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::setSize(SFLayerHandle layer, int32_t w, int32_t h) {
    Change c;
    c.layer = layer;
    c.params.width = w;
    c.params.height = h;
    c.hasSize = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::setZOrder(SFLayerHandle layer, int32_t z) {
    Change c;
    c.layer = layer;
    c.params.zOrder = z;
    c.hasZOrder = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::setAlpha(SFLayerHandle layer, float alpha) {
    Change c;
    c.layer = layer;
    c.params.alpha = alpha;
    c.hasAlpha = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::show(SFLayerHandle layer) {
    Change c;
    c.layer = layer;
    c.params.visible = true;
    c.hasVisible = true;
    mChanges.push_back(std::move(c));
    return *this;
}

SurfaceFlingerBridge::Transaction&
SurfaceFlingerBridge::Transaction::hide(SFLayerHandle layer) {
    Change c;
    c.layer = layer;
    c.params.visible = false;
    c.hasVisible = true;
    mChanges.push_back(std::move(c));
    return *this;
}

void SurfaceFlingerBridge::Transaction::apply() {
    android::SurfaceComposerClient::Transaction sfTx;

    for (auto& ch : mChanges) {
        if (!ch.layer || !ch.layer->surfaceControl) continue;
        auto& sc = ch.layer->surfaceControl;
        auto& p  = ch.params;

        if (ch.hasPosition)
            sfTx.setPosition(sc, static_cast<float>(p.x), static_cast<float>(p.y));
        if (ch.hasSize && p.width > 0 && p.height > 0) {
            ch.layer->surfaceControl->updateDefaultBufferSize(static_cast<uint32_t>(p.width),
                                                              static_cast<uint32_t>(p.height));
            sfTx.setSize(sc, static_cast<uint32_t>(p.width), static_cast<uint32_t>(p.height));
            sfTx.setCrop(sc, android::Rect(0, 0, p.width, p.height));
            if (ch.layer->surface) {
                ch.layer->surface->setBuffersDimensions(static_cast<uint32_t>(p.width),
                                                        static_cast<uint32_t>(p.height));
            }
        }
        if (ch.hasZOrder && p.zOrder >= 0)
            sfTx.setLayer(sc, p.zOrder);
        if (ch.hasAlpha && p.alpha < 1.0f)
            sfTx.setAlpha(sc, p.alpha);
        if (ch.hasVisible) {
            if (p.visible) sfTx.show(sc);
            else           sfTx.hide(sc);
        }
    }

    sfTx.apply();
    mChanges.clear();
}

// ────────────────────────────────────────────────────────────────────────────
// CPU Buffer: lock / unlockAndPost  (wl_shm path)
// ────────────────────────────────────────────────────────────────────────────
bool SurfaceFlingerBridge::lockBuffer(SFLayerHandle layer, SFLockedBuffer& outBuffer,
                                      const android_rect_t* dirty, DirtyRect* outClipped) {
    if (!layer || !layer->surface) return false;
    if (layer->isLocked) {
        ALOGW("lockBuffer called on already-locked layer");
        return false;
    }

    // Translate the damage rect into an android::Rect. Supplying it lets
    // BufferQueue hand back a buffer whose untouched regions still hold the
    // previous frame, so SurfaceFlinger only recomposites the damaged area.
    android::Rect inRect;
    const bool haveDirty = dirty && (dirty->right > dirty->left) &&
                           (dirty->bottom > dirty->top);
    if (haveDirty) {
        inRect.left   = dirty->left;
        inRect.top    = dirty->top;
        inRect.right  = dirty->right;
        inRect.bottom = dirty->bottom;
    }

    ANativeWindow_Buffer nwb{};
    android::status_t ret = layer->surface->lock(&nwb, haveDirty ? &inRect : nullptr);
    if (ret != android::NO_ERROR) {
        ALOGE("Surface::lock() failed: %d", ret);
        return false;
    }

    // Clamp the requested damage to the buffer we were actually given, so the
    // caller can restrict its copy to a region that really exists.
    if (outClipped) {
        outClipped->valid = false;
        if (haveDirty) {
            const int32_t l = std::max<int32_t>(dirty->left, 0);
            const int32_t t = std::max<int32_t>(dirty->top, 0);
            const int32_t r = std::min<int32_t>(dirty->right, nwb.width);
            const int32_t b = std::min<int32_t>(dirty->bottom, nwb.height);
            if (r > l && b > t) {
                outClipped->left = l; outClipped->top = t;
                outClipped->right = r; outClipped->bottom = b;
                outClipped->valid = true;
            }
        }
    }

    layer->lockedBuffer = nwb;
    layer->isLocked     = true;

    outBuffer.bits   = nwb.bits;
    outBuffer.width  = nwb.width;
    outBuffer.height = nwb.height;
    outBuffer.stride = nwb.stride;
    outBuffer.format = nwb.format;
    return true;
}

bool SurfaceFlingerBridge::lockBuffer(SFLayerHandle layer, SFLockedBuffer& outBuffer,
                                      const android_rect_t* dirty) {
    return lockBuffer(layer, outBuffer, dirty, nullptr);
}

bool SurfaceFlingerBridge::unlockAndPost(SFLayerHandle layer) {
    if (!layer || !layer->surface) return false;
    if (!layer->isLocked) {
        ALOGW("unlockAndPost on non-locked layer");
        return false;
    }

    android::status_t ret = layer->surface->unlockAndPost();
    layer->isLocked = false;
    if (ret != android::NO_ERROR) {
        ALOGE("Surface::unlockAndPost() failed: %d", ret);
        return false;
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// DMA-BUF path  (zwp_linux_dmabuf_v1)
// ────────────────────────────────────────────────────────────────────────────
bool SurfaceFlingerBridge::attachDmaBuf(SFLayerHandle layer,
                                        int dmaBufFd,
                                        int32_t width, int32_t height,
                                        uint32_t format, uint64_t /*modifier*/) {
    if (!layer) return false;

    // Release previous HW buffer if any
    if (layer->dmaBufHwb) {
        AHardwareBuffer_release(layer->dmaBufHwb);
        layer->dmaBufHwb = nullptr;
    }

    // Build AHardwareBuffer description
    AHardwareBuffer_Desc desc{};
    desc.width  = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.layers = 1;
    // Map DRM format → AHardwareBuffer format
    // Common mapping: DRM_FORMAT_ABGR8888 → AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage  = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    AHardwareBuffer* hwb = nullptr;
    int ret = AHardwareBuffer_createFromHandle(
        &desc,
        // Wrap the dma-buf fd into a native_handle_t
        // NOTE: This path varies by Android version and driver.
        // A more robust approach is to use the gralloc mapper HAL directly.
        nullptr, // native_handle_t* — see note above
        AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE,
        &hwb
    );

    if (ret != 0 || !hwb) {
        ALOGE("AHardwareBuffer_createFromHandle() failed: %d (fd=%d)", ret, dmaBufFd);
        // Fallback: mark for software blit path
        return false;
    }

    layer->dmaBufHwb = hwb;

    // Attach as a buffer to the ANativeWindow
    // (full zero-copy path; actual implementation depends on gralloc version)
    ALOGI("DMA-BUF attached: %dx%d fmt=0x%x fd=%d", width, height, format, dmaBufFd);
    return true;
}

} // namespace andwayland
