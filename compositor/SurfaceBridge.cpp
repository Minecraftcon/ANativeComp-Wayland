/**
 * SurfaceBridge.cpp
 *
 * Implements the core Wayland → SurfaceFlinger mapping.
 *
 * Key flows:
 *   wl_compositor.create_surface → allocate WaylandSurface (lazy SF layer)
 *   wl_surface.attach(buffer) → store pending buffer
 *   wl_surface.commit → call commitSurface() which:
 *       1. Creates a SurfaceFlinger layer if not yet created (lazy init)
 *       2. Blits the wl_shm_buffer into the layer's ANativeWindow_Buffer
 *          (or imports DMA-BUF zero-copy for GPU clients)
 *       3. Calls surface->unlockAndPost() → SurfaceFlinger composites the frame
 *       4. Sends wl_buffer.release() back to the client
 *       5. Fires frame callbacks so the client knows when to render again
 */

#include "SurfaceBridge.h"
#include "SeatManager.h"
#include "../platform/SurfaceFlingerBridge.h"

// libwayland-server
#include <wayland-server.h>
#include <wayland-server-protocol.h>

// xdg-shell generated bindings
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "xwayland-shell-protocol.h"
#include "SimpleFont.h"

#include <android/log.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cassert>
#include <cmath>

#define LOG_TAG "andwayland:Bridge"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace andwayland {

// ─────────────────────────────────────────────────────────────────────────────
// wl_surface interface implementation (static C callbacks)
// ─────────────────────────────────────────────────────────────────────────────

static void surface_destroy(wl_client*, wl_resource* resource) {
    auto* bridge = static_cast<SurfaceBridge*>(
        wl_resource_get_user_data(resource));
    WaylandSurface* surface = bridge->surfaceFromResource(resource);
    if (surface) bridge->destroySurface(surface);
    wl_resource_destroy(resource);
}

static void surface_attach(wl_client*, wl_resource* resource,
                            wl_resource* buffer, int32_t dx, int32_t dy) {
    auto* bridge   = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    auto* surface  = bridge->surfaceFromResource(resource);
    if (!surface) return;

    surface->pending.buffer    = buffer;
    surface->pending.dx        = dx;
    surface->pending.dy        = dy;
    surface->pending.hasBuffer = (buffer != nullptr);
}

static void surface_damage(wl_client*, wl_resource* resource,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* bridge  = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    auto* surface = bridge->surfaceFromResource(resource);
    if (surface) surface->pending.damage.add(x, y, w, h);
}

static void surface_damage_buffer(wl_client*, wl_resource* resource,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
    // Same as damage() in surface coordinates; buffer coordinates match here
    // because we ignore buffer scale/transform.
    auto* bridge  = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    auto* surface = bridge->surfaceFromResource(resource);
    if (surface) surface->pending.damage.add(x, y, w, h);
}

static void surface_frame(wl_client* client, wl_resource* resource, uint32_t callback_id) {
    auto* bridge  = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    auto* surface = bridge->surfaceFromResource(resource);
    if (!surface) return;

    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (cb) {
        surface->frameCallbacks.push_back(cb);
    }
}

static void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
static void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}

static void surface_commit(wl_client*, wl_resource* resource) {
    auto* bridge  = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    auto* surface = bridge->surfaceFromResource(resource);
    if (!surface) return;
    bridge->commitSurface(surface);
}

static void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
static void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
static void surface_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

static const struct wl_surface_interface surface_interface = {
    .destroy               = surface_destroy,
    .attach                = surface_attach,
    .damage                = surface_damage,
    .frame                 = surface_frame,
    .set_opaque_region     = surface_set_opaque_region,
    .set_input_region      = surface_set_input_region,
    .commit                = surface_commit,
    .set_buffer_transform  = surface_set_buffer_transform,
    .set_buffer_scale      = surface_set_buffer_scale,
    .damage_buffer         = surface_damage_buffer,
    .offset                = surface_offset,
};

static void surface_resource_destructor(wl_resource* resource) {
    // Runs whenever the wl_resource dies — including implicitly when a client
    // disconnects and libwayland reaps its resources. destroySurface() is
    // idempotent here: an explicit destroy request removes the map entry first,
    // so surfaceFromResource() then returns nullptr and this is a no-op.
    auto* bridge  = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    if (!bridge) return;
    WaylandSurface* surface = bridge->surfaceFromResource(resource);
    if (surface) bridge->destroySurface(surface);
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_shm_pool / wl_buffer (CPU shared memory path)
// ─────────────────────────────────────────────────────────────────────────────

struct ShmPool {
    void*   data = nullptr;
    int32_t size = 0;
    int     fd   = -1;
};

struct ShmBuffer {
    ShmPool* pool   = nullptr;
    int32_t  offset = 0;
    int32_t  width  = 0;
    int32_t  height = 0;
    int32_t  stride = 0;
    uint32_t format = WL_SHM_FORMAT_ARGB8888;
};

static void shm_pool_create_buffer(wl_client* client, wl_resource* resource,
                                   uint32_t id,
                                   int32_t offset, int32_t width, int32_t height,
                                   int32_t stride, uint32_t format);
static void shm_pool_destroy(wl_client*, wl_resource* resource) {
    auto* pool = static_cast<ShmPool*>(wl_resource_get_user_data(resource));
    if (pool && pool->data) {
        munmap(pool->data, static_cast<size_t>(pool->size));
    }
    delete pool;
}
static void shm_pool_resize(wl_client*, wl_resource* resource, int32_t size) {
    auto* pool = static_cast<ShmPool*>(wl_resource_get_user_data(resource));
    if (!pool) return;
    void* newData = mremap(pool->data, static_cast<size_t>(pool->size),
                           static_cast<size_t>(size), MREMAP_MAYMOVE);
    if (newData != MAP_FAILED) {
        pool->data = newData;
        pool->size = size;
    }
}

static const struct wl_shm_pool_interface shm_pool_interface = {
    .create_buffer = shm_pool_create_buffer,
    .destroy       = shm_pool_destroy,
    .resize        = shm_pool_resize,
};

static void shm_pool_destructor(wl_resource* resource) {
    // pool already destroyed via shm_pool_destroy
    (void)resource;
}

static void buffer_destroy(wl_client*, wl_resource* resource) {
    // The ShmBuffer is owned by the resource destroy handler installed in
    // shm_pool_create_buffer(); wl_resource_destroy() invokes it exactly once.
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface shm_buffer_interface = {
    .destroy = buffer_destroy,
};

static void shm_pool_create_buffer(wl_client* client, wl_resource* resource,
                                   uint32_t id,
                                   int32_t offset, int32_t width, int32_t height,
                                   int32_t stride, uint32_t format) {
    auto* pool = static_cast<ShmPool*>(wl_resource_get_user_data(resource));

    auto* buf   = new ShmBuffer();
    buf->pool   = pool;
    buf->offset = offset;
    buf->width  = width;
    buf->height = height;
    buf->stride = stride;
    buf->format = format;

    wl_resource* bufResource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(bufResource, &shm_buffer_interface,
                                   buf,
                                   [](wl_resource* r) {
                                       delete static_cast<ShmBuffer*>(
                                           wl_resource_get_user_data(r));
                                   });
}

// ─────────────────────────────────────────────────────────────────────────────
// xdg_surface / xdg_toplevel / xdg_popup
// ─────────────────────────────────────────────────────────────────────────────

struct XdgSurfaceData {
    SurfaceBridge*  bridge  = nullptr;
    WaylandSurface* surface = nullptr;
};

static void xdg_surface_ack_configure(wl_client*, wl_resource*, uint32_t) {}
static void xdg_surface_set_window_geometry(wl_client*, wl_resource*,
                                             int32_t, int32_t, int32_t, int32_t) {}

static void xdg_surface_get_toplevel(wl_client* client, wl_resource* xdgSurfaceRes, uint32_t id);
static void xdg_surface_get_popup(wl_client* client, wl_resource* xdgSurfaceRes, uint32_t id,
                                   wl_resource* parentRes, wl_resource* positionerRes);

static const struct xdg_surface_interface xdg_surface_interface_impl = {
    .destroy             = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    .get_toplevel        = xdg_surface_get_toplevel,
    .get_popup           = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure       = xdg_surface_ack_configure,
};

// xdg_popup
static void xdg_popup_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void xdg_popup_grab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}

static void calculatePopupLayout(const PositionerData* pos, int32_t& posX, int32_t& posY, int32_t& confW, int32_t& confH) {
    if (!pos) {
        posX = 0; posY = 0; confW = 0; confH = 0;
        return;
    }

    int32_t anchorPtX = pos->anchorX;
    int32_t anchorPtY = pos->anchorY;

    // 1. Anchor rectangle point
    switch (pos->anchor) {
        case 1: // TOP
            anchorPtX += pos->anchorW / 2;
            break;
        case 2: // BOTTOM
            anchorPtX += pos->anchorW / 2;
            anchorPtY += pos->anchorH;
            break;
        case 3: // LEFT
            anchorPtY += pos->anchorH / 2;
            break;
        case 4: // RIGHT
            anchorPtX += pos->anchorW;
            anchorPtY += pos->anchorH / 2;
            break;
        case 5: // TOP_LEFT
            break;
        case 6: // BOTTOM_LEFT
            anchorPtY += pos->anchorH;
            break;
        case 7: // TOP_RIGHT
            anchorPtX += pos->anchorW;
            break;
        case 8: // BOTTOM_RIGHT
            anchorPtX += pos->anchorW;
            anchorPtY += pos->anchorH;
            break;
        default: // NONE / default: attach below anchor
            anchorPtY += pos->anchorH;
            break;
    }

    int32_t popW = pos->width > 0 ? pos->width : 200;
    int32_t popH = pos->height > 0 ? pos->height : 200;

    int32_t x = anchorPtX;
    int32_t y = anchorPtY;

    // 2. Gravity
    switch (pos->gravity) {
        case 1: // TOP
            x -= popW / 2;
            y -= popH;
            break;
        case 2: // BOTTOM
            x -= popW / 2;
            break;
        case 3: // LEFT
            x -= popW;
            y -= popH / 2;
            break;
        case 4: // RIGHT
            y -= popH / 2;
            break;
        case 5: // TOP_LEFT
            x -= popW;
            y -= popH;
            break;
        case 6: // BOTTOM_LEFT
            x -= popW;
            break;
        case 7: // TOP_RIGHT
            y -= popH;
            break;
        case 8: // BOTTOM_RIGHT
        default:
            // Top-left of popup is at anchor point
            break;
    }

    x += pos->offsetX;
    y += pos->offsetY;

    posX = x;
    posY = y;
    confW = pos->width;
    confH = pos->height;
}

static void xdg_popup_reposition(wl_client* client, wl_resource* resource, wl_resource* positionerRes, uint32_t token) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface) return;

    if (positionerRes) {
        auto* pos = static_cast<PositionerData*>(wl_resource_get_user_data(positionerRes));
        int32_t posX = 0, posY = 0, confW = 0, confH = 0;
        calculatePopupLayout(pos, posX, posY, confW, confH);
        data->surface->popupX = posX;
        data->surface->popupY = posY;
        data->surface->popupW = confW;
        data->surface->popupH = confH;

        xdg_popup_send_repositioned(resource, token);
        xdg_popup_send_configure(resource, posX, posY, std::max(0, confW), std::max(0, confH));
        xdg_surface_send_configure(data->surface->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
    }
}

static const struct xdg_popup_interface xdg_popup_interface_impl = {
    .destroy    = xdg_popup_destroy,
    .grab       = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_surface_get_popup(wl_client* client, wl_resource* xdgSurfaceRes, uint32_t id,
                                   wl_resource* parentRes, wl_resource* positionerRes) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(xdgSurfaceRes));
    if (!data || !data->surface) return;

    int version = wl_resource_get_version(xdgSurfaceRes);
    wl_resource* popupRes = wl_resource_create(client, &xdg_popup_interface, version, id);
    if (!popupRes) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(popupRes, &xdg_popup_interface_impl, data, nullptr);
    data->surface->xdgPopup = popupRes;
    data->surface->isPopup = true;

    // Link with parent surface
    if (parentRes) {
        auto* parentData = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(parentRes));
        if (parentData && parentData->surface) {
            data->surface->parentSurface = parentData->surface;
            parentData->surface->popups.push_back(data->surface);
        }
    }

    int32_t posX = 0;
    int32_t posY = 0;
    int32_t confW = 0;
    int32_t confH = 0;

    if (positionerRes) {
        auto* pos = static_cast<PositionerData*>(wl_resource_get_user_data(positionerRes));
        calculatePopupLayout(pos, posX, posY, confW, confH);
    }

    data->surface->popupX = posX;
    data->surface->popupY = posY;
    data->surface->popupW = confW;
    data->surface->popupH = confH;

    ALOGI("Popup surface %u assigned to parent %u at relative offset (%d, %d), size (%d, %d)",
          data->surface->id,
          data->surface->parentSurface ? data->surface->parentSurface->id : 0,
          posX, posY, confW, confH);

    // Send initial popup configure with calculated geometry.
    // If width/height are 0, client sizes itself naturally without clipping!
    xdg_popup_send_configure(popupRes, posX, posY, std::max(0, confW), std::max(0, confH));
    xdg_surface_send_configure(xdgSurfaceRes, wl_display_next_serial(wl_client_get_display(client)));
}

// xdg_toplevel
static void xdg_toplevel_set_title(wl_client*, wl_resource* resource, const char* title) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (data && data->surface) {
        data->surface->title = title ? title : "";
        ALOGI("Surface %u title: '%s'", data->surface->id, data->surface->title.c_str());
        if (data->surface->hasDecor && data->bridge) {
            data->bridge->updateDecor(data->surface);
        }
    }
}
static void xdg_toplevel_set_app_id(wl_client*, wl_resource* resource, const char* app_id) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (data && data->surface) {
        data->surface->appId = app_id ? app_id : "";
        ALOGI("Surface %u app_id: '%s'", data->surface->id, data->surface->appId.c_str());
        if (data->surface->hasDecor && data->bridge) {
            data->bridge->updateDecor(data->surface);
        }
    }
}
static void xdg_toplevel_set_fullscreen(wl_client* client, wl_resource* resource, wl_resource*) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface || !data->bridge) return;

    data->surface->isFullscreen = true;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
    uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *a = XDG_TOPLEVEL_STATE_ACTIVATED;

    xdg_toplevel_send_configure(resource, data->bridge->displayWidth(), data->bridge->displayHeight(), &states);
    wl_array_release(&states);
    if (data->surface->xdgSurface) {
        xdg_surface_send_configure(data->surface->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
    }
    ALOGI("Configured surface %u as FULLSCREEN (%dx%d)",
          data->surface->id, data->bridge->displayWidth(), data->bridge->displayHeight());
}
static void xdg_toplevel_unset_fullscreen(wl_client* client, wl_resource* resource) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface || !data->bridge) return;

    data->surface->isFullscreen = false;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *a = XDG_TOPLEVEL_STATE_ACTIVATED;

    xdg_toplevel_send_configure(resource, 0, 0, &states);
    wl_array_release(&states);
    if (data->surface->xdgSurface) {
        xdg_surface_send_configure(data->surface->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
    }
}
static void xdg_toplevel_set_maximized(wl_client* client, wl_resource* resource) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface || !data->bridge) return;

    struct wl_array states;
    wl_array_init(&states);
    uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *s = XDG_TOPLEVEL_STATE_MAXIMIZED;
    uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *a = XDG_TOPLEVEL_STATE_ACTIVATED;

    xdg_toplevel_send_configure(resource, data->bridge->displayWidth(), data->bridge->displayHeight(), &states);
    wl_array_release(&states);
    if (data->surface->xdgSurface) {
        xdg_surface_send_configure(data->surface->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
    }
}
static void xdg_toplevel_unset_maximized(wl_client* client, wl_resource* resource) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface) return;

    struct wl_array states;
    wl_array_init(&states);
    uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *a = XDG_TOPLEVEL_STATE_ACTIVATED;

    xdg_toplevel_send_configure(resource, 0, 0, &states);
    wl_array_release(&states);
    if (data->surface->xdgSurface) {
        xdg_surface_send_configure(data->surface->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
    }
}
static void xdg_toplevel_set_minimized(wl_client*, wl_resource*) {}
static void xdg_toplevel_move(wl_client*, wl_resource* resource, wl_resource*, uint32_t) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(resource));
    if (!data || !data->surface || !data->bridge) return;
    int32_t sx = 0, sy = 0;
    if (data->bridge->getSeatManager()) {
        sx = data->bridge->getSeatManager()->getLastTouchScreenX();
        sy = data->bridge->getSeatManager()->getLastTouchScreenY();
    }
    data->bridge->startMoveGrab(data->surface, sx, sy);
    ALOGI("xdg_toplevel.move initiated for surface %u at (%d, %d)", data->surface->id, sx, sy);
}
static void xdg_toplevel_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void xdg_toplevel_set_parent(wl_client*, wl_resource*, wl_resource*) {}
static void xdg_toplevel_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void xdg_toplevel_set_min_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void xdg_toplevel_set_max_size(wl_client*, wl_resource*, int32_t, int32_t) {}

static const struct xdg_toplevel_interface xdg_toplevel_interface_impl = {
    .destroy           = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    .set_parent        = xdg_toplevel_set_parent,
    .set_title         = xdg_toplevel_set_title,
    .set_app_id        = xdg_toplevel_set_app_id,
    .show_window_menu  = xdg_toplevel_show_window_menu,
    .move              = xdg_toplevel_move,
    .resize            = xdg_toplevel_resize,
    .set_max_size      = xdg_toplevel_set_max_size,
    .set_min_size      = xdg_toplevel_set_min_size,
    .set_maximized     = xdg_toplevel_set_maximized,
    .unset_maximized   = xdg_toplevel_unset_maximized,
    .set_fullscreen    = xdg_toplevel_set_fullscreen,
    .unset_fullscreen  = xdg_toplevel_unset_fullscreen,
    .set_minimized     = xdg_toplevel_set_minimized,
};

static void xdg_surface_get_toplevel(wl_client* client, wl_resource* xdgSurfaceRes, uint32_t id) {
    auto* data = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(xdgSurfaceRes));

    int version = wl_resource_get_version(xdgSurfaceRes);
    wl_resource* toplevelRes = wl_resource_create(client, &xdg_toplevel_interface, version, id);
    wl_resource_set_implementation(toplevelRes, &xdg_toplevel_interface_impl,
                                   data, nullptr);
    if (data && data->surface) {
        data->surface->xdgToplevel = toplevelRes;
        data->surface->hasDecor = false;
    }

    // Send configure with ACTIVATED state so GUI clients immediately render
    struct wl_array states;
    wl_array_init(&states);
    uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (s) *s = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(toplevelRes, 0, 0, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(xdgSurfaceRes, wl_display_next_serial(
        wl_client_get_display(client)));
}

// ─────────────────────────────────────────────────────────────────────────────
// zxdg_decoration_manager_v1
// ─────────────────────────────────────────────────────────────────────────────
static void toplevel_decoration_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void toplevel_decoration_set_mode(wl_client*, wl_resource* resource, uint32_t mode) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    if (surface) {
        surface->hasDecor = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        ALOGI("toplevel_decoration_set_mode: surface %u mode=%u (hasDecor=%d)",
              surface->id, mode, surface->hasDecor);
        if (!surface->hasDecor && surface->decorLayer) {
            surface->bridge->getSurfaceFlingerBridge().destroyLayer(surface->decorLayer);
            surface->decorLayer = nullptr;
        }
    }
    zxdg_toplevel_decoration_v1_send_configure(resource, mode);
}

static void toplevel_decoration_unset_mode(wl_client*, wl_resource* resource) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    if (surface) {
        surface->hasDecor = true;
        ALOGI("toplevel_decoration_unset_mode: surface %u defaulting to SERVER_SIDE", surface->id);
    }
    zxdg_toplevel_decoration_v1_send_configure(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_toplevel_decoration_v1_interface toplevel_decoration_impl = {
    .destroy    = toplevel_decoration_destroy,
    .set_mode   = toplevel_decoration_set_mode,
    .unset_mode = toplevel_decoration_unset_mode,
};

static void decoration_manager_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void decoration_manager_get_toplevel_decoration(wl_client* client, wl_resource*,
                                                      uint32_t id, wl_resource* toplevel) {
    auto* xdgData = static_cast<XdgSurfaceData*>(wl_resource_get_user_data(toplevel));
    WaylandSurface* surface = xdgData ? xdgData->surface : nullptr;

    wl_resource* res = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, 1, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    if (surface) {
        surface->hasDecor = true;
    }
    ALOGI("get_toplevel_decoration: surface %u requested decor", surface ? surface->id : 0);
    wl_resource_set_implementation(res, &toplevel_decoration_impl, surface, nullptr);
    zxdg_toplevel_decoration_v1_send_configure(res, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_impl = {
    .destroy                 = decoration_manager_destroy,
    .get_toplevel_decoration = decoration_manager_get_toplevel_decoration,
};

void SurfaceBridge::bindXdgDecoration(wl_client* client, uint32_t version, uint32_t id) {
    ALOGI("bindXdgDecoration: client=%p, version=%u, id=%u", client, version, id);
    wl_resource* resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &decoration_manager_impl, this, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// xwayland_shell_v1 implementation
// ─────────────────────────────────────────────────────────────────────────────
static void xwayland_surface_set_serial(wl_client*, wl_resource* resource,
                                        uint32_t serial_lo, uint32_t serial_hi) {
    auto* surface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    if (!surface) return;
    if (serial_lo == 0 && serial_hi == 0) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL, "Serial cannot be 0");
        return;
    }
    surface->xwaylandSerial = (static_cast<uint64_t>(serial_hi) << 32) | serial_lo;
    ALOGI("xwayland_surface_v1: set_serial for surface %u (serial=0x%llx)",
          surface->id, static_cast<unsigned long long>(surface->xwaylandSerial));
}

static void xwayland_surface_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct xwayland_surface_v1_interface xwayland_surface_impl = {
    .set_serial = xwayland_surface_set_serial,
    .destroy    = xwayland_surface_destroy,
};

static void xwayland_shell_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void xwayland_shell_get_xwayland_surface(wl_client* client, wl_resource* resource,
                                                uint32_t id, wl_resource* surfaceResource) {
    auto* bridge = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    if (bridge) {
        int version = wl_resource_get_version(resource);
        bridge->getXwaylandSurface(client, id, surfaceResource, version);
    }
}

static const struct xwayland_shell_v1_interface xwayland_shell_impl = {
    .destroy              = xwayland_shell_destroy,
    .get_xwayland_surface = xwayland_shell_get_xwayland_surface,
};

void SurfaceBridge::bindXwaylandShell(wl_client* client, uint32_t version, uint32_t id) {
    ALOGI("bindXwaylandShell: client=%p, version=%u, id=%u", client, version, id);
    wl_resource* resource = wl_resource_create(client, &xwayland_shell_v1_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xwayland_shell_impl, this, nullptr);
}

void SurfaceBridge::getXwaylandSurface(wl_client* client, uint32_t id,
                                      wl_resource* surfaceResource, int version) {
    WaylandSurface* surface = surfaceFromResource(surfaceResource);
    if (!surface) {
        wl_resource_post_error(surfaceResource, WL_DISPLAY_ERROR_INVALID_OBJECT, "Surface not found");
        return;
    }
    if (surface->xdgSurface || surface->xdgToplevel || surface->xdgPopup ||
        surface->isSubsurface || surface->isCursor || surface->isXwayland) {
        wl_resource_post_error(surfaceResource, XWAYLAND_SHELL_V1_ERROR_ROLE,
                               "wl_surface already has an assigned role");
        return;
    }

    wl_resource* res = wl_resource_create(client, &xwayland_surface_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }

    surface->isXwayland = true;
    surface->xwaylandSurface = res;
    surface->hasDecor = true;
    if (surface->title.empty()) {
        surface->title = "X11 Application";
    }
    surface->appId = "xwayland";

    ALOGI("getXwaylandSurface: assigned xwayland role to surface %u", surface->id);

    wl_resource_set_implementation(res, &xwayland_surface_impl, surface, [](wl_resource* r) {
        auto* s = static_cast<WaylandSurface*>(wl_resource_get_user_data(r));
        if (s) {
            s->xwaylandSurface = nullptr;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_output
// ─────────────────────────────────────────────────────────────────────────────
static const struct wl_output_interface output_interface = {
    .release = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

// ─────────────────────────────────────────────────────────────────────────────
// SurfaceBridge
// ─────────────────────────────────────────────────────────────────────────────

SurfaceBridge::SurfaceBridge(SurfaceFlingerBridge& sfBridge)
    : mSfBridge(sfBridge) {
    auto info         = sfBridge.getDisplayInfo();
    mDisplayWidth     = info.width;
    mDisplayHeight    = info.height;
    mDisplayWidthMm   = info.width_mm;
    mDisplayHeightMm  = info.height_mm;
    mRefreshRate      = (info.refreshRate > 0.0f) ? info.refreshRate : 60.0f;
    ALOGI("Display: %dx%d (physical: %dx%d, %dx%d mm, dpi: %.1fx%.1f) @ %.1fHz",
          mDisplayWidth, mDisplayHeight, info.physWidth, info.physHeight,
          mDisplayWidthMm, mDisplayHeightMm, info.xdpi, info.ydpi, mRefreshRate);
}

SurfaceBridge::~SurfaceBridge() = default;

static bool isClientXwayland(wl_client* client) {
    if (!client) return false;
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, nullptr, nullptr);
    if (pid <= 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') buf[i] = ' ';
    }
    std::string cmd(buf);
    return cmd.find("xwayland-satellite") != std::string::npos ||
           cmd.find("Xwayland") != std::string::npos;
}

void SurfaceBridge::createSurface(wl_client* client, uint32_t id, int version) {
    wl_resource* resource = wl_resource_create(client, &wl_surface_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    auto surface       = std::make_unique<WaylandSurface>();
    surface->id        = id;
    surface->resource  = resource;
    surface->zOrder    = allocateZOrder();
    surface->bridge    = this;

    if (isClientXwayland(client)) {
        surface->isXwayland = true;
        ALOGI("createSurface: surface %u identified as Xwayland (satellite) surface", id);
    }

    wl_resource_set_implementation(resource, &surface_interface,
                                   this, surface_resource_destructor);

    mSurfaces[resource] = std::move(surface);
    ALOGI("wl_surface created: id=%u (v%d)", id, version);
}

void SurfaceBridge::createShmPool(wl_client* client, uint32_t id, int fd, int32_t size) {
    void* data = mmap(nullptr, static_cast<size_t>(size),
                      PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ALOGE("mmap() failed for shm pool fd=%d size=%d: %m", fd, size);
        wl_client_post_no_memory(client);
        return;
    }

    auto* pool  = new ShmPool();
    pool->data  = data;
    pool->size  = size;
    pool->fd    = fd;

    wl_resource* poolRes = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    wl_resource_set_implementation(poolRes, &shm_pool_interface, pool,
                                   shm_pool_destructor);
}

void SurfaceBridge::getXdgSurface(wl_client* client, uint32_t id, wl_resource* surfaceResource, int version) {
    auto* surface = surfaceFromResource(surfaceResource);
    if (!surface) {
        ALOGE("getXdgSurface: unknown wl_surface resource");
        return;
    }

    wl_resource* xdgRes = wl_resource_create(client, &xdg_surface_interface, version, id);
    auto* data = new XdgSurfaceData{this, surface};
    wl_resource_set_implementation(xdgRes, &xdg_surface_interface_impl, data, [](wl_resource* r) {
        delete static_cast<XdgSurfaceData*>(wl_resource_get_user_data(r));
    });
    surface->xdgSurface = xdgRes;
}

void SurfaceBridge::bindOutput(wl_client* client, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_output_interface,
                                               static_cast<int>(version), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &output_interface, this, nullptr);

    // Send output geometry
    wl_output_send_geometry(resource,
        0, 0,                                // x, y (position in global compositor space)
        mDisplayWidthMm, mDisplayHeightMm,   // physical width/height mm
        WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "ANativeDrawer",                     // make
        "Android Display",                   // model
        WL_OUTPUT_TRANSFORM_NORMAL);

    // Send mode
    int32_t refreshMhz = static_cast<int32_t>(std::round(mRefreshRate * 1000.0f));
    if (refreshMhz <= 0) refreshMhz = 60000;
    wl_output_send_mode(resource,
        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        mDisplayWidth, mDisplayHeight,
        refreshMhz);

    if (version >= 2) wl_output_send_scale(resource, 1);
    if (version >= 4) {
        wl_output_send_name(resource, "Android-0");
        wl_output_send_description(resource, "Android Primary Display");
    }
    if (version >= 2) wl_output_send_done(resource);
}

// ─────────────────────────────────────────────────────────────────────────────
// commitSurface — the hot path
// ─────────────────────────────────────────────────────────────────────────────
void SurfaceBridge::commitSurface(WaylandSurface* surface) {
    // ── 1. Lazy-create the SurfaceFlinger layer ───────────────────────────────
    if (!surface->sfLayer) {
        // Only create a SurfaceFlinger layer if this surface has a valid window role:
        // xdgToplevel, xdgPopup, subsurface, or isXwayland!
        // Do NOT create window layers for cursor surfaces or unassigned surfaces!
        if (surface->isCursor || (!surface->xdgToplevel && !surface->xdgPopup && !surface->isSubsurface && !surface->isXwayland)) {
            return;
        }

        // Defer layer creation until the client actually attaches a buffer so the
        // layer is created with its true geometry and BLASTBufferQueue doesn't reject frames.
        if (surface->pending.hasBuffer && surface->pending.buffer) {
            int32_t layerW = mDisplayWidth;
            int32_t layerH = mDisplayHeight;

            struct wl_shm_buffer* shm_buffer = wl_shm_buffer_get(surface->pending.buffer);
            if (shm_buffer) {
                layerW = wl_shm_buffer_get_width(shm_buffer);
                layerH = wl_shm_buffer_get_height(shm_buffer);
            } else {
                void* userData = wl_resource_get_user_data(surface->pending.buffer);
                auto* shmBuf   = reinterpret_cast<ShmBuffer*>(userData);
                if (shmBuf) {
                    layerW = shmBuf->width;
                    layerH = shmBuf->height;
                }
            }

            std::string name = (surface->isXwayland ? "xwayland#" : "wl_surface#") + std::to_string(surface->id);
            int32_t z = surface->zOrder;
            if (surface->isSubsurface && surface->parentSurface) {
                z = surface->parentSurface->zOrder + 20;
            } else if (surface->isPopup && surface->parentSurface) {
                z = surface->parentSurface->zOrder + 50;
            }
            surface->sfLayer = mSfBridge.createLayer(
                name,
                layerW, layerH,
                z);

            if (!surface->sfLayer) {
                ALOGE("Failed to create SurfaceFlinger layer for surface %u", surface->id);
                return;
            }

            int32_t posX = 0;
            int32_t posY = 0;
            if (surface->isSubsurface && surface->parentSurface) {
                posX = surface->parentSurface->committed.x + surface->subX;
                posY = surface->parentSurface->committed.y + surface->subY;
            } else if (surface->isPopup && surface->parentSurface) {
                posX = surface->parentSurface->committed.x + surface->popupX;
                posY = surface->parentSurface->committed.y + surface->popupY;
                // Clamp to screen bounds so popup menu is not cut off by display edge
                if (posX + layerW > mDisplayWidth) posX = std::max(0, mDisplayWidth - layerW);
                if (posY + layerH > mDisplayHeight) posY = std::max(0, mDisplayHeight - layerH);
                if (posX < 0) posX = 0;
                if (posY < 0) posY = 0;
                ALOGI("Popup surface %u anchored at (%d, %d), size %dx%d (parent at %d, %d)",
                      surface->id, posX, posY, layerW, layerH,
                      surface->parentSurface->committed.x, surface->parentSurface->committed.y);
            } else if (layerW < mDisplayWidth || layerH < mDisplayHeight) {
                posX = std::max(0, (mDisplayWidth - layerW) / 2);
                posY = std::max(surface->hasDecor ? surface->decorHeight : 0, (mDisplayHeight - layerH) / 2);
                ALOGI("Windowed surface %u centered at (%d, %d), size %dx%d",
                      surface->id, posX, posY, layerW, layerH);
            }

            SurfaceFlingerBridge::Transaction tx;
            tx.setPosition(surface->sfLayer, posX, posY);
            tx.apply();
            surface->committed.x = posX;
            surface->committed.y = posY;
            surface->committed.width = layerW;
            surface->committed.height = layerH;
            surface->committed.mapped = true;

            if (surface->xdgToplevel || surface->isXwayland) {
                activateSurface(surface);
            }

            // Propagate position to any existing child subsurfaces
            for (WaylandSurface* sub : surface->subsurfaces) {
                if (sub && sub->sfLayer) {
                    int32_t sx = posX + sub->subX;
                    int32_t sy = posY + sub->subY;
                    SurfaceFlingerBridge::Transaction subTx;
                    subTx.setPosition(sub->sfLayer, sx, sy);
                    subTx.apply();
                    sub->committed.x = sx;
                    sub->committed.y = sy;
                }
            }
            // Propagate position to any existing child popups
            for (WaylandSurface* pop : surface->popups) {
                if (pop && pop->sfLayer) {
                    int32_t px = posX + pop->popupX;
                    int32_t py = posY + pop->popupY;
                    if (px + pop->committed.width > mDisplayWidth) px = std::max(0, mDisplayWidth - pop->committed.width);
                    if (py + pop->committed.height > mDisplayHeight) py = std::max(0, mDisplayHeight - pop->committed.height);
                    if (px < 0) px = 0;
                    if (py < 0) py = 0;
                    SurfaceFlingerBridge::Transaction popTx;
                    popTx.setPosition(pop->sfLayer, px, py);
                    popTx.apply();
                    pop->committed.x = px;
                    pop->committed.y = py;
                }
            }
        }
    }

    // Keep subsurface position in sync with parent on commit
    if (surface->isSubsurface && surface->parentSurface && surface->sfLayer) {
        int32_t targetX = surface->parentSurface->committed.x + surface->subX;
        int32_t targetY = surface->parentSurface->committed.y + surface->subY;
        if (targetX != surface->committed.x || targetY != surface->committed.y) {
            SurfaceFlingerBridge::Transaction tx;
            tx.setPosition(surface->sfLayer, targetX, targetY);
            tx.apply();
            surface->committed.x = targetX;
            surface->committed.y = targetY;
        }
    }

    // Keep popup position in sync with parent on commit
    if (surface->isPopup && surface->parentSurface && surface->sfLayer) {
        int32_t targetX = surface->parentSurface->committed.x + surface->popupX;
        int32_t targetY = surface->parentSurface->committed.y + surface->popupY;
        if (targetX + surface->committed.width > mDisplayWidth) targetX = std::max(0, mDisplayWidth - surface->committed.width);
        if (targetY + surface->committed.height > mDisplayHeight) targetY = std::max(0, mDisplayHeight - surface->committed.height);
        if (targetX < 0) targetX = 0;
        if (targetY < 0) targetY = 0;
        if (targetX != surface->committed.x || targetY != surface->committed.y) {
            SurfaceFlingerBridge::Transaction tx;
            tx.setPosition(surface->sfLayer, targetX, targetY);
            tx.apply();
            surface->committed.x = targetX;
            surface->committed.y = targetY;
        }
    }

    // ── 2. Copy/import the buffer ─────────────────────────────────────────────
    if (surface->pending.hasBuffer && surface->pending.buffer) {
        if (surface->sfLayer) {
            if (wl_shm_buffer_get(surface->pending.buffer)) {
                blitShmBuffer(surface, surface->pending.buffer);
            } else {
                void* userData = wl_resource_get_user_data(surface->pending.buffer);
                auto* shmBuf   = reinterpret_cast<ShmBuffer*>(userData);
                if (shmBuf && shmBuf->pool && shmBuf->pool->data) {
                    blitShmBuffer(surface, surface->pending.buffer);
                }
            }
        }
        // else: DMA-BUF path (handled by LinuxDmaBuf protocol impl)

        // Notify client the buffer is released and can be reused
        wl_buffer_send_release(surface->pending.buffer);
        surface->pending.hasBuffer = false;
        // Damage is consumed by this commit.
        surface->pending.damage.clear();
    }

    // ── 3. Fire frame callbacks ───────────────────────────────────────────────
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t nowMs = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    for (wl_resource* cb : surface->frameCallbacks) {
        wl_callback_send_done(cb, nowMs);
        wl_resource_destroy(cb);
    }
    surface->frameCallbacks.clear();

    // ── 4. Update decor if toplevel windowed ─────────────────────────────────
    if (surface->hasDecor && (surface->xdgToplevel || surface->isXwayland) && !surface->isFullscreen && surface->committed.mapped) {
        if (!surface->decorLayer || surface->committed.width != static_cast<int32_t>(surface->decorBuffer.size() / (surface->decorHeight ? surface->decorHeight : 1))) {
            updateDecor(surface);
        }
    }
}

// Merge the damaged sub-rectangle of a client shm buffer into the surface's
// persistent back buffer. The back buffer is always complete, so the copy into
// SurfaceFlinger's freshly-dequeued buffer can be a straight full-frame blit
// without ever exposing stale regions.
static void mergeDamage(std::vector<uint32_t>& back, int32_t backStride,
                        const uint8_t* src, int32_t srcStride,
                        int32_t srcWidth, int32_t srcHeight,
                        uint32_t format,
                        bool fullRepaint,
                        const SurfaceFlingerBridge::DirtyRect& r) {
    int32_t x0 = 0, y0 = 0, x1, y1;
    const int32_t limitW = std::min(srcWidth,  backStride);
    const int32_t limitH = std::min(srcHeight, (int32_t)(back.size() / (backStride ? backStride : 1)));

    if (fullRepaint) {
        x1 = limitW; y1 = limitH;
    } else {
        x0 = std::max<int32_t>(0, std::min(r.left, limitW));
        y0 = std::max<int32_t>(0, std::min(r.top, limitH));
        x1 = std::max<int32_t>(0, std::min(r.right, limitW));
        y1 = std::max<int32_t>(0, std::min(r.bottom, limitH));
    }
    if (x1 <= x0 || y1 <= y0) return;

    const bool isXrgb = (format == WL_SHM_FORMAT_XRGB8888);

    for (int32_t y = y0; y < y1; ++y) {
        const uint8_t* srcRow = src + static_cast<size_t>(y) * srcStride
                                      + static_cast<size_t>(x0) * 4;
        uint32_t* dstRow = back.data() + static_cast<size_t>(y) * backStride + x0;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(x1 - x0) * 4);
        if (isXrgb) {
            for (int32_t x = 0; x < (x1 - x0); ++x) {
                dstRow[x] |= 0xFF000000;
            }
        }
    }
}

// Present the (always complete) back buffer into the SurfaceFlinger layer.
static bool presentBackBuffer(SurfaceFlingerBridge& sf, SFLayerHandle layer,
                              const std::vector<uint32_t>& back, int32_t backStride) {
    SFLockedBuffer dst{};
    if (!sf.lockBuffer(layer, dst, nullptr)) return false;

    const int32_t copyWidth  = std::min<int32_t>(backStride, dst.width);
    const int32_t copyHeight = std::min<int32_t>((int32_t)(back.size() / (backStride ? backStride : 1)),
                                                 dst.height);
    ALOGI("presentBackBuffer: backStride=%d, dst.w=%d, dst.h=%d, copyW=%d, copyH=%d",
          backStride, dst.width, dst.height, copyWidth, copyHeight);
    const size_t rowBytes = static_cast<size_t>(copyWidth) * 4;
    for (int32_t y = 0; y < copyHeight; ++y) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(back.data())
                              + static_cast<size_t>(y) * backStride * 4;
        uint8_t* dstRow = static_cast<uint8_t*>(dst.bits)
                        + static_cast<size_t>(y) * dst.stride * 4;
        std::memcpy(dstRow, srcRow, rowBytes);
    }
    return sf.unlockAndPost(layer);
}

bool SurfaceBridge::blitShmBuffer(WaylandSurface* surface, wl_resource* bufferResource) {
    // An empty damage rect means the client asked for a full repaint, or this
    // is the first frame.
    const bool fullRepaint = !surface->pending.damage.valid;

    SurfaceFlingerBridge::DirtyRect r;
    r.left   = surface->pending.damage.x1;
    r.top    = surface->pending.damage.y1;
    r.right  = surface->pending.damage.x2;
    r.bottom = surface->pending.damage.y2;

    struct wl_shm_buffer* shm_buffer = wl_shm_buffer_get(bufferResource);
    const uint8_t* src = nullptr;
    int32_t srcW = 0, srcH = 0, srcStride = 0;
    uint32_t format = WL_SHM_FORMAT_ARGB8888;
    bool haveSrc = false;

    if (shm_buffer) {
        wl_shm_buffer_begin_access(shm_buffer);
        src       = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm_buffer));
        srcW      = wl_shm_buffer_get_width(shm_buffer);
        srcH      = wl_shm_buffer_get_height(shm_buffer);
        srcStride = wl_shm_buffer_get_stride(shm_buffer);
        format    = wl_shm_buffer_get_format(shm_buffer);
        haveSrc   = (src != nullptr && srcW > 0 && srcH > 0);
    } else {
        auto* shmBuf = static_cast<ShmBuffer*>(wl_resource_get_user_data(bufferResource));
        if (shmBuf && shmBuf->pool && shmBuf->pool->data) {
            src       = static_cast<const uint8_t*>(shmBuf->pool->data) + shmBuf->offset;
            srcW      = shmBuf->width;
            srcH      = shmBuf->height;
            srcStride = shmBuf->stride;
            format    = shmBuf->format;
            haveSrc   = true;
        }
    }

    if (!haveSrc) {
        if (shm_buffer) wl_shm_buffer_end_access(shm_buffer);
        return false;
    }

    // Dynamic layer resize when buffer geometry changes
    if (surface->sfLayer && (surface->committed.width != srcW || surface->committed.height != srcH)) {
        mSfBridge.resizeLayer(surface->sfLayer, srcW, srcH);
        int32_t posX = surface->committed.x;
        int32_t posY = surface->committed.y;
        if (surface->isSubsurface && surface->parentSurface) {
            posX = surface->parentSurface->committed.x + surface->subX;
            posY = surface->parentSurface->committed.y + surface->subY;
        }
        SurfaceFlingerBridge::Transaction tx;
        tx.setPosition(surface->sfLayer, posX, posY);
        if (surface->decorLayer) {
            tx.setPosition(surface->decorLayer, posX, posY - surface->decorHeight);
        }
        tx.apply();
        surface->committed.x = posX;
        surface->committed.y = posY;

        if (surface->hasDecor && surface->decorLayer) {
            updateDecor(surface);
        }

        // Propagate position update to any child subsurfaces
        for (WaylandSurface* sub : surface->subsurfaces) {
            if (sub && sub->sfLayer) {
                int32_t sx = posX + sub->subX;
                int32_t sy = posY + sub->subY;
                SurfaceFlingerBridge::Transaction subTx;
                subTx.setPosition(sub->sfLayer, sx, sy);
                subTx.apply();
                sub->committed.x = sx;
                sub->committed.y = sy;
            }
        }
    }

    // Resize (or invalidate) the back buffer when the client changes size, and
    // force a full repaint in that case — a resized image can't be patched.
    bool resized = false;
    if (surface->backStride != srcW || surface->backHeight != srcH) {
        surface->backStride = srcW;
        surface->backWidth  = srcW;
        surface->backHeight = srcH;
        surface->backBuffer.assign(static_cast<size_t>(srcW) * srcH, 0);
        resized = true;
    }
    const bool full = fullRepaint || resized || surface->backBuffer.empty();

    mergeDamage(surface->backBuffer, surface->backStride,
                src, srcStride, srcW, srcH, format, full, r);

    if (shm_buffer) wl_shm_buffer_end_access(shm_buffer);

    if (!presentBackBuffer(mSfBridge, surface->sfLayer,
                           surface->backBuffer, surface->backStride)) {
        ALOGE("present failed for surface %u", surface->id);
        return false;
    }

    ALOGI("blitShmBuffer: %dx%d src fmt=0x%x, %s damage", srcW, srcH, format, full ? "full" : "partial");

    surface->committed.width  = srcW;
    surface->committed.height = srcH;
    surface->committed.mapped = true;
    return true;
}

WaylandSurface* SurfaceBridge::surfaceFromResource(wl_resource* resource) {
    auto it = mSurfaces.find(resource);
    return (it != mSurfaces.end()) ? it->second.get() : nullptr;
}

void SurfaceBridge::destroySurface(WaylandSurface* surface) {
    if (!surface) return;
    if (mGrabSurface == surface) {
        mGrabSurface = nullptr;
    }
    bool wasActive = (mActiveSurface == surface);
    if (wasActive) {
        mActiveSurface = nullptr;
    }
    if (mSeat) {
        mSeat->notifySurfaceDestroyed(surface->resource);
    }
    if (surface->parentSurface) {
        auto& subs = surface->parentSurface->subsurfaces;
        subs.erase(std::remove(subs.begin(), subs.end(), surface), subs.end());
        auto& pops = surface->parentSurface->popups;
        pops.erase(std::remove(pops.begin(), pops.end(), surface), pops.end());
        surface->parentSurface = nullptr;
    }
    for (WaylandSurface* sub : surface->subsurfaces) {
        if (sub) sub->parentSurface = nullptr;
    }
    surface->subsurfaces.clear();
    for (WaylandSurface* pop : surface->popups) {
        if (pop) pop->parentSurface = nullptr;
    }
    surface->popups.clear();

    if (surface->decorLayer) {
        mSfBridge.destroyLayer(surface->decorLayer);
        surface->decorLayer = nullptr;
    }
    if (surface->sfLayer) {
        mSfBridge.destroyLayer(surface->sfLayer);
        surface->sfLayer = nullptr;
    }
    // Frame callbacks are orphaned; destroy them
    for (wl_resource* cb : surface->frameCallbacks) {
        wl_resource_destroy(cb);
    }
    surface->frameCallbacks.clear();

    wl_resource* res = surface->resource;
    mSurfaces.erase(res);
    ALOGI("wl_surface destroyed");

    if (wasActive) {
        WaylandSurface* nextTop = nullptr;
        int32_t topZ = -1;
        for (const auto& [r, s] : mSurfaces) {
            if (s && s->committed.mapped && (s->xdgToplevel || s->isXwayland) && !s->isCursor) {
                if (s->zOrder > topZ) {
                    topZ = s->zOrder;
                    nextTop = s.get();
                }
            }
        }
        if (nextTop) {
            activateSurface(nextTop);
        } else if (mSeat) {
            mSeat->setKeyboardFocus(nullptr);
        }
    }
}

void SurfaceBridge::destroyLayerForSurface(WaylandSurface* surface) {
    if (surface) {
        if (surface->decorLayer) {
            mSfBridge.destroyLayer(surface->decorLayer);
            surface->decorLayer = nullptr;
        }
        if (surface->sfLayer) {
            mSfBridge.destroyLayer(surface->sfLayer);
            surface->sfLayer = nullptr;
        }
    }
}

WaylandSurface* SurfaceBridge::surfaceAt(int32_t screenX, int32_t screenY,
                                        int32_t* outLocalX, int32_t* outLocalY) {
    WaylandSurface* best = nullptr;
    int32_t bestZ = -1;

    for (const auto& [res, surf] : mSurfaces) {
        if (!surf || !surf->committed.mapped || !surf->sfLayer || surf->isCursor) continue;
        if (!surf->xdgToplevel && !surf->xdgPopup && !surf->isSubsurface && !surf->isXwayland) continue;
        int32_t sx = surf->committed.x;
        int32_t sy = surf->committed.y;
        int32_t sw = surf->committed.width;
        int32_t sh = surf->committed.height;

        int32_t topY = sy;
        if (surf->hasDecor && surf->decorLayer) {
            topY = sy - surf->decorHeight;
        }

        if (screenX >= sx && screenX < sx + sw &&
            screenY >= topY && screenY < sy + sh) {
            int32_t effectiveZ = surf->zOrder;
            if (surf->isPopup && surf->parentSurface) {
                effectiveZ = surf->parentSurface->zOrder + 50;
            } else if (surf->isSubsurface && surf->parentSurface) {
                effectiveZ = surf->parentSurface->zOrder + 20;
            }
            if (effectiveZ > bestZ) {
                best = surf.get();
                bestZ = effectiveZ;
            }
        }
    }

    if (best) {
        if (outLocalX) *outLocalX = screenX - best->committed.x;
        if (outLocalY) *outLocalY = screenY - best->committed.y;
    }
    return best;
}

void SurfaceBridge::updateDecor(WaylandSurface* surface) {
    if (!surface || !surface->hasDecor || surface->isFullscreen) {
        if (surface && surface->decorLayer) {
            mSfBridge.destroyLayer(surface->decorLayer);
            surface->decorLayer = nullptr;
        }
        return;
    }

    int32_t w = surface->committed.width;
    int32_t h = surface->decorHeight;
    if (w <= 0 || h <= 0) return;

    if (!surface->decorLayer) {
        std::string decorName = "wl_surface#" + std::to_string(surface->id) + "-decor";
        surface->decorLayer = mSfBridge.createLayer(decorName, w, h, surface->zOrder + 10);
        if (!surface->decorLayer) {
            ALOGE("Failed to create decor layer for surface %u", surface->id);
            return;
        }
    } else {
        mSfBridge.resizeLayer(surface->decorLayer, w, h);
    }

    SurfaceFlingerBridge::Transaction tx;
    tx.setPosition(surface->decorLayer, surface->committed.x, surface->committed.y - h);
    tx.apply();

    // Active vs Inactive titlebar theme
    bool isActive = (surface == mActiveSurface);
    uint32_t bgColor     = isActive ? 0xFF222428 : 0xFF141517;
    uint32_t borderColor = isActive ? 0xFF383A40 : 0xFF222326;
    uint32_t textColor   = isActive ? 0xFFFFFFFF : 0xFF7E8187;
    uint32_t btnColor    = isActive ? 0xFFE0E0E0 : 0xFF65676C;

    // Render titlebar pixels
    surface->decorBuffer.assign(static_cast<size_t>(w) * h, bgColor);

    // Bottom border line at y = h - 1
    for (int32_t x = 0; x < w; ++x) {
        surface->decorBuffer[(h - 1) * w + x] = borderColor;
    }

    // Window title text
    std::string title = surface->title.empty()
        ? (surface->appId.empty() ? "Window" : surface->appId)
        : surface->title;
    drawString(surface->decorBuffer.data(), w, h, 14, 11, title.c_str(), 2, textColor, w - 140);

    // Sharp Material/Windows style window buttons:
    // 1. Minimize '—': [w - 132 .. w - 89]
    // 2. Maximize '□': [w - 88 .. w - 45]
    // 3. Close    '✕': [w - 44 .. w]

    // Minimize: 10px horizontal line, 2px high at y = 18
    int32_t minCx = w - 110;
    for (int32_t y = 17; y <= 18; ++y) {
        for (int32_t x = minCx - 5; x <= minCx + 5; ++x) {
            if (x >= 0 && x < w) surface->decorBuffer[y * w + x] = btnColor;
        }
    }

    // Maximize: 10x10 hollow square outline, 2px border
    int32_t maxCx = w - 66;
    int32_t sqX0 = maxCx - 5, sqX1 = maxCx + 5;
    int32_t sqY0 = 13, sqY1 = 23;
    for (int32_t y = sqY0; y <= sqY1; ++y) {
        for (int32_t x = sqX0; x <= sqX1; ++x) {
            if (x >= 0 && x < w) {
                if (y <= sqY0 + 1 || y >= sqY1 - 1 || x <= sqX0 + 1 || x >= sqX1 - 1) {
                    surface->decorBuffer[y * w + x] = btnColor;
                }
            }
        }
    }

    // Close: 10x10 sharp cross '✕', 2px thick
    int32_t clsCx = w - 22;
    int32_t clsCy = 18;
    for (int32_t d = -4; d <= 4; ++d) {
        int32_t y = clsCy + d;
        if (y >= 0 && y < h) {
            int32_t x1 = clsCx + d;
            int32_t x2 = clsCx - d;
            if (x1 >= 0 && x1 < w) surface->decorBuffer[y * w + x1] = btnColor;
            if (x1 + 1 >= 0 && x1 + 1 < w) surface->decorBuffer[y * w + x1 + 1] = btnColor;
            if (x2 >= 0 && x2 < w) surface->decorBuffer[y * w + x2] = btnColor;
            if (x2 + 1 >= 0 && x2 + 1 < w) surface->decorBuffer[y * w + x2 + 1] = btnColor;
        }
    }

    presentBackBuffer(mSfBridge, surface->decorLayer, surface->decorBuffer, w);
}

void SurfaceBridge::moveSurface(WaylandSurface* surface, int32_t newX, int32_t newY) {
    if (!surface || !surface->sfLayer) return;

    // Constrain so window cannot be lost completely off-screen
    int32_t minY = surface->hasDecor ? surface->decorHeight : 0;
    int32_t clampedY = std::max(minY, newY);
    int32_t clampedX = std::clamp(newX, -(surface->committed.width - 100), mDisplayWidth - 100);

    surface->committed.x = clampedX;
    surface->committed.y = clampedY;

    SurfaceFlingerBridge::Transaction tx;
    tx.setPosition(surface->sfLayer, clampedX, clampedY);
    if (surface->decorLayer) {
        tx.setPosition(surface->decorLayer, clampedX, clampedY - surface->decorHeight);
    }
    for (WaylandSurface* sub : surface->subsurfaces) {
        if (sub && sub->sfLayer) {
            int32_t sx = clampedX + sub->subX;
            int32_t sy = clampedY + sub->subY;
            tx.setPosition(sub->sfLayer, sx, sy);
            sub->committed.x = sx;
            sub->committed.y = sy;
        }
    }
    tx.apply();
}

void SurfaceBridge::startMoveGrab(WaylandSurface* surface, int32_t screenX, int32_t screenY) {
    if (!surface) return;
    activateSurface(surface);
    mGrabSurface = surface;
    mGrabStartX = screenX;
    mGrabStartY = screenY;
    mWindowStartX = surface->committed.x;
    mWindowStartY = surface->committed.y;
    ALOGI("startMoveGrab: surface %u at screen (%d, %d), window at (%d, %d)",
          surface->id, screenX, screenY, mWindowStartX, mWindowStartY);
}

void SurfaceBridge::updateMoveGrab(int32_t screenX, int32_t screenY) {
    if (!mGrabSurface) return;
    int32_t dx = screenX - mGrabStartX;
    int32_t dy = screenY - mGrabStartY;
    moveSurface(mGrabSurface, mWindowStartX + dx, mWindowStartY + dy);
}

void SurfaceBridge::endMoveGrab() {
    if (mGrabSurface) {
        ALOGI("endMoveGrab: ended grab for surface %u at (%d, %d)",
              mGrabSurface->id, mGrabSurface->committed.x, mGrabSurface->committed.y);
        mGrabSurface = nullptr;
    }
}

void SurfaceBridge::closeSurface(WaylandSurface* surface) {
    if (!surface) return;
    if (surface->xdgToplevel) {
        xdg_toplevel_send_close(surface->xdgToplevel);
        ALOGI("closeSurface: sent close to surface %u", surface->id);
    } else if (surface->isXwayland) {
        destroyLayerForSurface(surface);
        ALOGI("closeSurface: closed Xwayland surface %u", surface->id);
    }
}

void SurfaceBridge::minimizeSurface(WaylandSurface* surface) {
    if (!surface) return;
    SurfaceFlingerBridge::Transaction tx;
    if (surface->sfLayer) tx.hide(surface->sfLayer);
    if (surface->decorLayer) tx.hide(surface->decorLayer);
    for (WaylandSurface* sub : surface->subsurfaces) {
        if (sub && sub->sfLayer) tx.hide(sub->sfLayer);
    }
    tx.apply();
    ALOGI("minimizeSurface: hid layers for surface %u", surface->id);
}

void SurfaceBridge::toggleMaximize(WaylandSurface* surface) {
    if (!surface) return;
    if (surface->xdgToplevel) {
        activateSurface(surface);
        surface->isFullscreen = !surface->isFullscreen;
        struct wl_array states;
        wl_array_init(&states);
        uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *a = XDG_TOPLEVEL_STATE_ACTIVATED;
        if (surface->isFullscreen) {
            uint32_t* m = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
            *m = XDG_TOPLEVEL_STATE_MAXIMIZED;
            int32_t decorH = surface->hasDecor ? surface->decorHeight : 0;
            xdg_toplevel_send_configure(surface->xdgToplevel, mDisplayWidth, mDisplayHeight - decorH, &states);
            moveSurface(surface, 0, decorH);
        } else {
            xdg_toplevel_send_configure(surface->xdgToplevel, 0, 0, &states);
        }
        wl_array_release(&states);
        if (surface->xdgSurface) {
            xdg_surface_send_configure(surface->xdgSurface, wl_display_next_serial(
                wl_client_get_display(wl_resource_get_client(surface->resource))));
        }
        ALOGI("toggleMaximize: surface %u fullscreen=%d", surface->id, surface->isFullscreen);
    } else if (surface->isXwayland) {
        activateSurface(surface);
        surface->isFullscreen = !surface->isFullscreen;
        int32_t decorH = surface->hasDecor ? surface->decorHeight : 0;
        if (surface->isFullscreen) {
            moveSurface(surface, 0, decorH);
        }
        ALOGI("toggleMaximize: Xwayland surface %u fullscreen=%d", surface->id, surface->isFullscreen);
    }
}

int32_t SurfaceBridge::allocateZOrder() {
    mNextZOrder += 100;
    return mNextZOrder;
}

void SurfaceBridge::activateSurface(WaylandSurface* surface) {
    if (!surface) return;
    WaylandSurface* root = surface;
    while (root && root->parentSurface) {
        root = root->parentSurface;
    }
    if (!root) return;

    WaylandSurface* old = mActiveSurface;
    bool changed = (old != root);

    if (changed && old) {
        // Deactivate old window according to standard Wayland xdg_shell protocol
        // Only if old's resource is still valid in mSurfaces!
        bool oldValid = (mSurfaces.find(old->resource) != mSurfaces.end());
        if (oldValid && old->xdgToplevel) {
            struct wl_array states;
            wl_array_init(&states);
            // Include states EXCEPT XDG_TOPLEVEL_STATE_ACTIVATED
            if (old->isFullscreen) {
                uint32_t* f = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
                if (f) *f = XDG_TOPLEVEL_STATE_FULLSCREEN;
            }
            int32_t w = old->isFullscreen ? mDisplayWidth : 0;
            int32_t h = old->isFullscreen ? mDisplayHeight : 0;
            xdg_toplevel_send_configure(old->xdgToplevel, w, h, &states);
            wl_array_release(&states);

            if (old->xdgSurface && old->resource) {
                struct wl_client* client = wl_resource_get_client(old->resource);
                if (client) {
                    xdg_surface_send_configure(old->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
                }
            }
        }
        if (oldValid && old->hasDecor && old->decorLayer) {
            updateDecor(old);
        }
    }

    mActiveSurface = root;

    // Allocate new topmost Z-order band for the activated window
    int32_t newZ = allocateZOrder();
    root->zOrder = newZ;

    // Atomically raise root surface, decor layer, and all children
    SurfaceFlingerBridge::Transaction tx;
    if (root->sfLayer) {
        tx.setZOrder(root->sfLayer, newZ);
    }
    if (root->decorLayer) {
        tx.setZOrder(root->decorLayer, newZ + 10);
    }
    int32_t subOffset = 20;
    for (WaylandSurface* sub : root->subsurfaces) {
        if (sub) {
            sub->zOrder = newZ + subOffset;
            if (sub->sfLayer) {
                tx.setZOrder(sub->sfLayer, newZ + subOffset);
            }
            subOffset = std::min(subOffset + 2, 45);
        }
    }
    int32_t popOffset = 50;
    for (WaylandSurface* pop : root->popups) {
        if (pop) {
            pop->zOrder = newZ + popOffset;
            if (pop->sfLayer) {
                tx.setZOrder(pop->sfLayer, newZ + popOffset);
            }
            popOffset = std::min(popOffset + 2, 95);
        }
    }
    tx.apply();

    // Standard Wayland xdg_shell activation configure event
    if (root->xdgToplevel) {
        struct wl_array states;
        wl_array_init(&states);
        uint32_t* a = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (a) *a = XDG_TOPLEVEL_STATE_ACTIVATED;
        if (root->isFullscreen) {
            uint32_t* f = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
            if (f) *f = XDG_TOPLEVEL_STATE_FULLSCREEN;
        }
        int32_t w = root->isFullscreen ? mDisplayWidth : 0;
        int32_t h = root->isFullscreen ? mDisplayHeight : 0;
        xdg_toplevel_send_configure(root->xdgToplevel, w, h, &states);
        wl_array_release(&states);

        if (root->xdgSurface && root->resource) {
            struct wl_client* client = wl_resource_get_client(root->resource);
            if (client) {
                xdg_surface_send_configure(root->xdgSurface, wl_display_next_serial(wl_client_get_display(client)));
            }
        }
    }

    // Update server-side decor titlebar
    if (root->hasDecor && root->decorLayer) {
        updateDecor(root);
    }

    // Dispatch Wayland wl_keyboard enter/leave via SeatManager
    if (mSeat && root->resource) {
        mSeat->setKeyboardFocus(root->resource);
    }

    ALOGI("activateSurface: Activated window %u ('%s') at z=%d",
          root->id, root->title.c_str(), newZ);
}

} // namespace andwayland
