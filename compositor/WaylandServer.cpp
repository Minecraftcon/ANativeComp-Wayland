/**
 * WaylandServer.cpp
 *
 * Initializes the Wayland display, registers all protocol globals, and
 * drives the main event loop.
 */

#include "WaylandServer.h"
#include "SurfaceBridge.h"
#include "SeatManager.h"
#include "ExtensionRegistry.h"

// libwayland-server
#include <wayland-server.h>

// Protocol generated headers (from wayland-scanner)
#include "xdg-shell-protocol.h"
#include "viewporter-protocol.h"
#include "xdg-decoration-protocol.h"
#ifdef ENABLE_DMABUF
#  include "linux-dmabuf-protocol.h"
#endif
#ifdef ENABLE_LAYER_SHELL
#  include "layer-shell-protocol.h"
#endif

#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>

#define LOG_TAG "andwayland:Server"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace andwayland {

// ─────────────────────────────────────────────────────────────────────────────
// wl_region implementation
// ─────────────────────────────────────────────────────────────────────────────
struct RegionData {
    struct Rect { int32_t x, y, w, h; };
    std::vector<Rect> rects;
};

static void region_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void region_add(wl_client*, wl_resource* resource,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* reg = static_cast<RegionData*>(wl_resource_get_user_data(resource));
    if (reg && w > 0 && h > 0) {
        reg->rects.push_back({x, y, w, h});
    }
}

static void region_subtract(wl_client*, wl_resource*,
                            int32_t, int32_t, int32_t, int32_t) {
    // Advisory region subtraction
}

static const struct wl_region_interface region_interface = {
    .destroy  = region_destroy,
    .add      = region_add,
    .subtract = region_subtract,
};

// ─────────────────────────────────────────────────────────────────────────────
// wl_compositor implementation
// ─────────────────────────────────────────────────────────────────────────────
static void compositor_create_surface(wl_client* client, wl_resource* resource, uint32_t id);
static void compositor_create_region(wl_client* client, wl_resource* resource, uint32_t id);

static const struct wl_compositor_interface compositor_interface = {
    .create_surface = compositor_create_surface,
    .create_region  = compositor_create_region,
};

static void compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_interface, data, nullptr);
}

static void compositor_create_surface(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* bridge = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    int version = wl_resource_get_version(resource);
    bridge->createSurface(client, id, version);
}

static void compositor_create_region(wl_client* client, wl_resource* /*resource*/, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_region_interface, 1, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* reg = new RegionData();
    wl_resource_set_implementation(res, &region_interface, reg, [](wl_resource* r) {
        delete static_cast<RegionData*>(wl_resource_get_user_data(r));
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// xdg_wm_base (xdg-shell)
// ─────────────────────────────────────────────────────────────────────────────
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

static void positioner_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void positioner_set_size(wl_client*, wl_resource* resource, int32_t w, int32_t h) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) { p->width = w; p->height = h; }
}
static void positioner_set_anchor_rect(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) { p->anchorX = x; p->anchorY = y; p->anchorW = w; p->anchorH = h; }
}
static void positioner_set_anchor(wl_client*, wl_resource* resource, uint32_t anchor) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) p->anchor = anchor;
}
static void positioner_set_gravity(wl_client*, wl_resource* resource, uint32_t gravity) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) p->gravity = gravity;
}
static void positioner_set_constraint_adjustment(wl_client*, wl_resource* resource, uint32_t adj) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) p->constraintAdjustment = adj;
}
static void positioner_set_offset(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) { p->offsetX = x; p->offsetY = y; }
}
static void positioner_set_reactive(wl_client*, wl_resource* resource) {
    auto* p = static_cast<PositionerData*>(wl_resource_get_user_data(resource));
    if (p) p->reactive = true;
}
static void positioner_set_parent_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void positioner_set_parent_configure(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_positioner_interface positioner_interface_impl = {
    .destroy                    = positioner_destroy,
    .set_size                   = positioner_set_size,
    .set_anchor_rect            = positioner_set_anchor_rect,
    .set_anchor                 = positioner_set_anchor,
    .set_gravity                = positioner_set_gravity,
    .set_constraint_adjustment  = positioner_set_constraint_adjustment,
    .set_offset                 = positioner_set_offset,
    .set_reactive               = positioner_set_reactive,
    .set_parent_size            = positioner_set_parent_size,
    .set_parent_configure       = positioner_set_parent_configure,
};

static void xdg_wm_base_pong(wl_client*, wl_resource*, uint32_t) {}
static void xdg_wm_base_get_xdg_surface(wl_client* client, wl_resource* resource,
                                         uint32_t id, wl_resource* surface_resource);
static void xdg_wm_base_create_positioner(wl_client* client, wl_resource* resource, uint32_t id) {
    int version = wl_resource_get_version(resource);
    wl_resource* posRes = wl_resource_create(client, &xdg_positioner_interface, version, id);
    if (!posRes) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* pos = new PositionerData();
    wl_resource_set_implementation(posRes, &positioner_interface_impl, pos, [](wl_resource* r) {
        delete static_cast<PositionerData*>(wl_resource_get_user_data(r));
    });
}

static const struct xdg_wm_base_interface xdg_wm_base_interface_impl = {
    .destroy           = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface   = xdg_wm_base_get_xdg_surface,
    .pong              = xdg_wm_base_pong,
};

static void xdg_wm_base_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_wm_base_interface_impl, data, nullptr);
}

static void xdg_wm_base_get_xdg_surface(wl_client* client, wl_resource* resource,
                                          uint32_t id, wl_resource* surface_resource) {
    auto* bridge = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    int version = wl_resource_get_version(resource);
    bridge->getXdgSurface(client, id, surface_resource, version);
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_subcompositor
// ─────────────────────────────────────────────────────────────────────────────
static void subsurface_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void subsurface_set_position(wl_client*, wl_resource* resource, int32_t x, int32_t y) {
    auto* childSurface = static_cast<WaylandSurface*>(wl_resource_get_user_data(resource));
    if (childSurface) {
        childSurface->subX = x;
        childSurface->subY = y;
    }
}
static void subsurface_place_above(wl_client*, wl_resource*, wl_resource*) {}
static void subsurface_place_below(wl_client*, wl_resource*, wl_resource*) {}
static void subsurface_set_sync(wl_client*, wl_resource*) {}
static void subsurface_set_desync(wl_client*, wl_resource*) {}

static const struct wl_subsurface_interface subsurface_interface_impl = {
    .destroy      = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above  = subsurface_place_above,
    .place_below  = subsurface_place_below,
    .set_sync     = subsurface_set_sync,
    .set_desync   = subsurface_set_desync,
};

static void subcompositor_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void subcompositor_get_subsurface(wl_client* client, wl_resource* resource,
                                        uint32_t id, wl_resource* surface_res, wl_resource* parent_res) {
    auto* bridge = static_cast<SurfaceBridge*>(wl_resource_get_user_data(resource));
    WaylandSurface* child = bridge ? bridge->surfaceFromResource(surface_res) : nullptr;
    WaylandSurface* parent = bridge ? bridge->surfaceFromResource(parent_res) : nullptr;

    wl_resource* subRes = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    if (!subRes) {
        wl_client_post_no_memory(client);
        return;
    }

    if (child && parent) {
        child->parentSurface = parent;
        child->isSubsurface  = true;
        parent->subsurfaces.push_back(child);
    }

    wl_resource_set_implementation(subRes, &subsurface_interface_impl, child, nullptr);
}

static const struct wl_subcompositor_interface subcompositor_interface_impl = {
    .destroy        = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_subcompositor_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_interface_impl, data, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_data_device_manager
// ─────────────────────────────────────────────────────────────────────────────
static void data_source_offer(wl_client*, wl_resource*, const char*) {}
static void data_source_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
static void data_source_set_actions(wl_client*, wl_resource*, uint32_t) {}

static const struct wl_data_source_interface data_source_interface_impl = {
    .offer       = data_source_offer,
    .destroy     = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void data_device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*, uint32_t) {}
static void data_device_set_selection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void data_device_release(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

static const struct wl_data_device_interface data_device_interface_impl = {
    .start_drag    = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release       = data_device_release,
};

static void data_device_manager_create_data_source(wl_client* client, wl_resource* resource, uint32_t id) {
    int version = wl_resource_get_version(resource);
    wl_resource* ds = wl_resource_create(client, &wl_data_source_interface, version, id);
    if (!ds) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(ds, &data_source_interface_impl, nullptr, nullptr);
}

static void data_device_manager_get_data_device(wl_client* client, wl_resource* resource, uint32_t id, wl_resource*) {
    int version = wl_resource_get_version(resource);
    wl_resource* dd = wl_resource_create(client, &wl_data_device_interface, version, id);
    if (!dd) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(dd, &data_device_interface_impl, nullptr, nullptr);
}

static const struct wl_data_device_manager_interface data_device_manager_interface_impl = {
    .create_data_source = data_device_manager_create_data_source,
    .get_data_device    = data_device_manager_get_data_device,
};

static void data_device_manager_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_data_device_manager_interface,
                                               static_cast<int>(version), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &data_device_manager_interface_impl, nullptr, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_output (single output = primary display)
// ─────────────────────────────────────────────────────────────────────────────
static void output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* bridge = static_cast<SurfaceBridge*>(data);
    bridge->bindOutput(client, version, id);
}

// ─────────────────────────────────────────────────────────────────────────────
// wl_seat
// ─────────────────────────────────────────────────────────────────────────────
static void seat_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(data);
    seat->bindSeat(client, version, id);
}

// ─────────────────────────────────────────────────────────────────────────────
// WaylandServer
// ─────────────────────────────────────────────────────────────────────────────
WaylandServer::WaylandServer()  = default;
WaylandServer::~WaylandServer() {
    stop();
    if (mDisplay) {
        wl_display_destroy(mDisplay);
        mDisplay = nullptr;
    }
    if (mWakeSource) {
        wl_event_source_remove(mWakeSource);
        mWakeSource = nullptr;
    }
    if (mWakeReadFd  >= 0) close(mWakeReadFd);
    if (mWakeWriteFd >= 0) close(mWakeWriteFd);
}

bool WaylandServer::init(const std::string& socketName,
                         std::shared_ptr<SurfaceBridge>     bridge,
                         std::shared_ptr<SeatManager>       seat,
                         std::shared_ptr<ExtensionRegistry> ext) {
    mBridge = bridge;
    mSeat   = seat;
    mExt    = ext;

    // ── Create Wayland display ───────────────────────────────────────────────
    mDisplay = wl_display_create();
    if (!mDisplay) {
        ALOGE("wl_display_create() failed");
        return false;
    }

    // ── Set up socket directory ──────────────────────────────────────────────
    // On Android we use /data/local/tmp/wayland or /run/user/0 on newer roots
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (!runtimeDir) {
        // Create a fallback directory writable by root
        mkdir("/data/wayland", 0777);
        setenv("XDG_RUNTIME_DIR", "/data/wayland", 1);
        runtimeDir = "/data/wayland";
    }
    ALOGI("Wayland runtime dir: %s", runtimeDir);

    if (wl_display_add_socket(mDisplay, socketName.c_str()) != 0) {
        ALOGE("Failed to create Wayland socket '%s': %s",
              socketName.c_str(), strerror(errno));
        return false;
    }
    mSocketName = socketName;
    std::string socketPath = std::string(runtimeDir) + "/" + socketName;
    chmod(socketPath.c_str(), 0777);
    ALOGI("Listening on %s", socketPath.c_str());

    // ── Register protocol globals ────────────────────────────────────────────
    registerCompositorGlobal();
    registerShmGlobal();
    registerXdgWmBaseGlobal();
    registerSeatGlobal();
    registerOutputGlobal();
    registerSubcompositorGlobal();
    registerDataDeviceManagerGlobal();
    registerViewporterGlobal();
    registerXdgDecorationGlobal();
#ifdef ENABLE_DMABUF
    registerLinuxDmaBufGlobal();
#endif
#ifdef ENABLE_LAYER_SHELL
    registerLayerShellGlobal();
#endif

    mEventLoop = wl_display_get_event_loop(mDisplay);

    // ── Wake pipe (for stop()) ───────────────────────────────────────────────
    // Must come after mEventLoop is set — the loop blocks indefinitely, so
    // this is the only way stop() can break it out.
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0) {
        mWakeReadFd  = pfd[0];
        mWakeWriteFd = pfd[1];
        mWakeSource = wl_event_loop_add_fd(mEventLoop, mWakeReadFd, WL_EVENT_READABLE,
                                           [](int fd, uint32_t mask, void* data) -> int {
                                               (void)fd; (void)mask;
                                               auto* self = static_cast<WaylandServer*>(data);
                                               self->mRunning = false;
                                               return 0;
                                           }, this);
    }

    if (mSeat && mBridge) {
        mBridge->setSeatManager(mSeat.get());
        if (!mSeat->start(mDisplay, mEventLoop, mBridge)) {
            ALOGW("SeatManager::start() could not open some input devices or had warnings");
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Global registrations
// ─────────────────────────────────────────────────────────────────────────────
void WaylandServer::registerCompositorGlobal() {
    wl_global_create(mDisplay, &wl_compositor_interface,
                     5, mBridge.get(), compositor_bind);
}

void WaylandServer::registerShmGlobal() {
    wl_display_init_shm(mDisplay); // adds wl_shm with ARGB8888/XRGB8888
}

void WaylandServer::registerXdgWmBaseGlobal() {
    wl_global_create(mDisplay, &xdg_wm_base_interface,
                     5, mBridge.get(), xdg_wm_base_bind);
}

void WaylandServer::registerSeatGlobal() {
    wl_global_create(mDisplay, &wl_seat_interface,
                     7, mSeat.get(), seat_bind);
}

void WaylandServer::registerOutputGlobal() {
    wl_global_create(mDisplay, &wl_output_interface,
                     4, mBridge.get(), output_bind);
}

void WaylandServer::registerSubcompositorGlobal() {
    wl_global_create(mDisplay, &wl_subcompositor_interface,
                     1, mBridge.get(), subcompositor_bind);
}

void WaylandServer::registerDataDeviceManagerGlobal() {
    wl_global_create(mDisplay, &wl_data_device_manager_interface,
                     3, nullptr, data_device_manager_bind);
}

// ─────────────────────────────────────────────────────────────────────────────
// wp_viewporter implementation
// ─────────────────────────────────────────────────────────────────────────────
static void viewport_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void viewport_set_source(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
static void viewport_set_destination(wl_client*, wl_resource*, int32_t, int32_t) {}

static const struct wp_viewport_interface viewport_interface_impl = {
    .destroy         = viewport_destroy,
    .set_source      = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}
static void viewporter_get_viewport(wl_client* client, wl_resource*, uint32_t id, wl_resource* surface) {
    wl_resource* vp = wl_resource_create(client, &wp_viewport_interface, 1, id);
    if (!vp) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(vp, &viewport_interface_impl, surface, nullptr);
}

static const struct wp_viewporter_interface viewporter_interface_impl = {
    .destroy      = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_viewporter_interface,
                                               static_cast<int>(version), id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &viewporter_interface_impl, nullptr, nullptr);
}

void WaylandServer::registerViewporterGlobal() {
    wl_global_create(mDisplay, &wp_viewporter_interface,
                     1, nullptr, viewporter_bind);
}

static void xdg_decoration_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* bridge = static_cast<SurfaceBridge*>(data);
    if (bridge) {
        bridge->bindXdgDecoration(client, version, id);
    }
}

void WaylandServer::registerXdgDecorationGlobal() {
    wl_global_create(mDisplay, &zxdg_decoration_manager_v1_interface,
                     1, mBridge.get(), xdg_decoration_bind);
}

#ifdef ENABLE_DMABUF
void WaylandServer::registerLinuxDmaBufGlobal() {
    // Registered by LinuxDmaBuf::registerGlobal(mDisplay, ...)
}
#endif

#ifdef ENABLE_LAYER_SHELL
void WaylandServer::registerLayerShellGlobal() {
    // Registered by LayerShell::registerGlobal(mDisplay, ...)
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Event loop
// ─────────────────────────────────────────────────────────────────────────────
void WaylandServer::run() {
    mRunning = true;
    ALOGI("Event loop starting");

    // Block indefinitely; the wake pipe (registered below) and the input fds are
    // what interrupt the wait. A finite timeout here would add that much
    // latency to every input event and keep the loop spinning when idle.
    while (mRunning) {
        int ret = wl_event_loop_dispatch(mEventLoop, -1);
        if (ret < 0) {
            ALOGE("wl_event_loop_dispatch error: %d", ret);
            break;
        }
        // Push anything queued during the callbacks out to clients.
        // libwayland >= 1.20 returns void here and reaps dead clients itself.
        wl_display_flush_clients(mDisplay);
    }

    ALOGI("Event loop stopped");
}

void WaylandServer::stop() {
    mRunning = false;
    // Wake the event loop
    if (mWakeWriteFd >= 0) {
        char b = 1;
        (void)write(mWakeWriteFd, &b, 1);
    }
}

} // namespace andwayland
