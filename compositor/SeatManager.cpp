/**
 * SeatManager.cpp
 *
 * Full evdev input subsystem for ANativeDrawer:
 * - Enumerates /dev/input/event* (touchscreen, keypad, mouse, keyboard)
 * - Directly hooks into the Wayland main event loop (wl_event_loop_add_fd)
 * - Translates touch events (Linux Multi-Touch Type B) to wl_touch
 * - Translates primary finger touch to wl_pointer emulation (for desktop Linux apps)
 * - Translates hardware keys to wl_keyboard with complete standard XKB keymap
 * - Automatically performs surface hit-testing and local coordinate translation
 */

#include "SeatManager.h"
#include "SurfaceBridge.h"
#include "DefaultKeymap.h"
#include "virtual-keyboard-protocol.h"
#include "text-input-protocol.h"
#include "input-method-protocol.h"
#include <signal.h>

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <android/log.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cerrno>

#define LOG_TAG "andwayland:Seat"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

namespace andwayland {

struct TouchSlot {
    int32_t trackingId = -1;
    int32_t screenX = 0;
    int32_t screenY = 0;
    int32_t localX = 0;
    int32_t localY = 0;
    bool down = false;
    bool modified = false;
    WaylandSurface* targetSurface = nullptr;
    wl_resource* targetSurfaceResource = nullptr;
};

struct EvdevDevice {
    int fd = -1;
    std::string path;
    std::string name;
    bool isTouch = false;
    bool isKeyboard = false;
    bool isTypingKeyboard = false;
    bool isPointer = false;
    bool isGrabbed = false;
    struct input_absinfo absX{};
    struct input_absinfo absY{};
    struct wl_event_source* source = nullptr;
};

enum class PointerGestureState {
    IDLE,
    PENDING_DECISION,
    SCROLLING,
    DRAGGING_SELECTION,
    HOLD_TRIGGERED
};

struct PointerGestureContext {
    PointerGestureState state = PointerGestureState::IDLE;
    int32_t startScreenX = 0;
    int32_t startScreenY = 0;
    int32_t lastScreenX = 0;
    int32_t lastScreenY = 0;
    int32_t localX = 0;
    int32_t localY = 0;
    uint32_t downTimeMs = 0;
    uint32_t lastTapTimeMs = 0;
    int32_t lastTapScreenX = 0;
    int32_t lastTapScreenY = 0;
    struct wl_event_source* holdTimerSource = nullptr;
    WaylandSurface* targetSurface = nullptr;
    wl_resource* targetSurfaceResource = nullptr;
};

struct TextInputV3State {
    bool enabled = false;
    std::string surroundingText;
    int32_t cursor = 0;
    int32_t anchor = 0;
    bool surroundingChanged = false;
    uint32_t textChangeCause = 0;
    uint32_t contentHint = 0;
    uint32_t contentPurpose = 0;
    bool contentTypeChanged = false;
    int32_t cursorX = 0, cursorY = 0, cursorW = 0, cursorH = 0;
    bool cursorRectChanged = false;
};

struct TextInputV3Resource {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    wl_resource* focusedSurface = nullptr;
    SeatManager::Impl* impl = nullptr;
    TextInputV3State pending;
    TextInputV3State current;
};

struct InputMethodV2Resource {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    SeatManager::Impl* impl = nullptr;
    uint32_t serial = 0;
    bool active = false;
};

struct SeatManager::Impl {
    wl_display* display = nullptr;
    wl_event_loop* eventLoop = nullptr;
    std::shared_ptr<SurfaceBridge> bridge;

    std::vector<EvdevDevice> devices;
    std::vector<wl_resource*> seatResources;
    std::vector<wl_resource*> pointerResources;
    std::vector<wl_resource*> keyboardResources;
    std::vector<wl_resource*> touchResources;

    wl_resource* currentKeyboardSurface = nullptr;
    wl_resource* currentPointerSurface = nullptr;

    static constexpr int MAX_TOUCH_SLOTS = 16;
    TouchSlot touchSlots[MAX_TOUCH_SLOTS];
    int currentSlot = 0;

    int32_t pointerScreenX = 0;
    int32_t pointerScreenY = 0;
    bool pointerModified = false;

    PointerGestureContext gesture;

    int keymapFd = -1;
    size_t keymapSize = 0;

    int inotifyFd = -1;
    struct wl_event_source* inotifySource = nullptr;

    std::vector<TextInputV3Resource*> textInputs;
    TextInputV3Resource* activeTextInput = nullptr;

    std::vector<InputMethodV2Resource*> inputMethods;

    void triggerVirtualKeyboardVisibility(bool show) {
        DIR* proc = opendir("/proc");
        if (!proc) return;
        struct dirent* entry;
        while ((entry = readdir(proc)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char* endptr = nullptr;
            pid_t pid = strtol(entry->d_name, &endptr, 10);
            if (!endptr || *endptr != '\0' || pid <= 0) continue;
            char cmdlinePath[64];
            snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%d/cmdline", pid);
            FILE* f = fopen(cmdlinePath, "r");
            if (f) {
                char cmd[128];
                size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
                fclose(f);
                if (n > 0) {
                    cmd[n] = '\0';
                    if (strstr(cmd, "wvkbd") != nullptr) {
                        kill(pid, show ? SIGUSR2 : SIGUSR1);
                    }
                }
            }
        }
        closedir(proc);
    }

    void sendInputMethodActivate(TextInputV3Resource* ti) {
        ALOGI("TextInput: Activating virtual keyboard for text field");
        if (bridge) {
            bridge->setLayerSurfacesVisible(true);
        }
        for (auto* im : inputMethods) {
            zwp_input_method_v2_send_activate(im->resource);
            if (ti && ti->current.surroundingChanged) {
                zwp_input_method_v2_send_surrounding_text(
                    im->resource,
                    ti->current.surroundingText.c_str(),
                    ti->current.cursor,
                    ti->current.anchor);
            }
            if (ti && ti->current.contentTypeChanged) {
                zwp_input_method_v2_send_content_type(
                    im->resource,
                    ti->current.contentHint,
                    ti->current.contentPurpose);
            }
            im->serial++;
            zwp_input_method_v2_send_done(im->resource);
            im->active = true;
        }
        triggerVirtualKeyboardVisibility(true);
    }

    void sendInputMethodDeactivate() {
        ALOGI("TextInput: Deactivating virtual keyboard");
        if (bridge) {
            bridge->setLayerSurfacesVisible(false);
        }
        for (auto* im : inputMethods) {
            if (im->active) {
                zwp_input_method_v2_send_deactivate(im->resource);
                im->serial++;
                zwp_input_method_v2_send_done(im->resource);
                im->active = false;
            }
        }
        triggerVirtualKeyboardVisibility(false);
    }

    void sendInputMethodState(TextInputV3Resource* ti) {
        if (!ti) return;
        for (auto* im : inputMethods) {
            if (im->active) {
                if (ti->current.surroundingChanged) {
                    zwp_input_method_v2_send_surrounding_text(
                        im->resource,
                        ti->current.surroundingText.c_str(),
                        ti->current.cursor,
                        ti->current.anchor);
                }
                if (ti->current.contentTypeChanged) {
                    zwp_input_method_v2_send_content_type(
                        im->resource,
                        ti->current.contentHint,
                        ti->current.contentPurpose);
                }
                im->serial++;
                zwp_input_method_v2_send_done(im->resource);
            }
        }
    }

    void handleTextInputCommit(TextInputV3Resource* ti) {
        bool wasActive = (activeTextInput == ti);
        bool nowActive = ti->pending.enabled;

        ti->current = ti->pending;
        ti->pending.surroundingChanged = false;
        ti->pending.contentTypeChanged = false;
        ti->pending.cursorRectChanged = false;

        if (nowActive && !wasActive) {
            activeTextInput = ti;
            sendInputMethodActivate(ti);
        } else if (!nowActive && wasActive) {
            activeTextInput = nullptr;
            sendInputMethodDeactivate();
        } else if (nowActive && wasActive) {
            sendInputMethodState(ti);
        }
    }

    void handleHoldTimeout() {
        if (gesture.state != PointerGestureState::PENDING_DECISION) {
            return;
        }
        gesture.state = PointerGestureState::HOLD_TRIGGERED;

        if (!gesture.targetSurfaceResource || !display) {
            return;
        }

        struct wl_client* targetClient = wl_resource_get_client(gesture.targetSurfaceResource);
        uint32_t serial = wl_display_next_serial(display);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t timeMs = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

        gesture.state = PointerGestureState::HOLD_TRIGGERED;
        for (wl_resource* ptr : pointerResources) {
            if (wl_resource_get_client(ptr) == targetClient) {
                wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                wl_pointer_send_button(ptr, serial, timeMs, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
                wl_pointer_send_frame(ptr);
            }
        }
        ALOGI("Pointer Emulation: HOLD triggered -> switched BTN_LEFT to BTN_RIGHT at (%d, %d)",
              gesture.localX, gesture.localY);
    }

    void prepareKeymap() {
        if (keymapFd >= 0) return;
        keymapSize = strlen(gDefaultKeymap) + 1;
        keymapFd = memfd_create("wl_keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (keymapFd >= 0) {
            ftruncate(keymapFd, static_cast<off_t>(keymapSize));
            write(keymapFd, gDefaultKeymap, keymapSize);
            fcntl(keymapFd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
            ALOGI("Prepared standard XKB keymap (size: %zu bytes)", keymapSize);
        } else {
            ALOGW("Failed to create memfd for keymap: %s", strerror(errno));
        }
    }
};

static inline int32_t scaleCoord(int32_t val, int32_t minVal, int32_t maxVal, int32_t targetMax) {
    if (maxVal > minVal && targetMax > 0) {
        int64_t range = maxVal - minVal + 1;
        int64_t scaled = (static_cast<int64_t>(val - minVal) * targetMax) / range;
        return static_cast<int32_t>(std::clamp<int64_t>(scaled, 0, targetMax - 1));
    }
    return std::clamp(val, 0, targetMax - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Wayland Protocol Interfaces
// ─────────────────────────────────────────────────────────────────────────────
static void keyboard_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_iface = {
    .release = keyboard_release,
};

static void pointer_set_cursor(wl_client*, wl_resource* resource, uint32_t, wl_resource* surface_resource, int32_t, int32_t) {
    if (surface_resource) {
        auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
        if (seat && seat->getBridge()) {
            WaylandSurface* surf = seat->getBridge()->surfaceFromResource(surface_resource);
            if (surf) {
                surf->isCursor = true;
                seat->getBridge()->destroyLayerForSurface(surf);
            }
        }
    }
}
static void pointer_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_iface = {
    .set_cursor = pointer_set_cursor,
    .release    = pointer_release,
};

static void touch_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_touch_interface touch_iface = {
    .release = touch_release,
};

static void seat_get_pointer(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    wl_resource* ptr = wl_resource_create(client, &wl_pointer_interface,
                                          wl_resource_get_version(resource), id);
    if (!ptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(ptr, &pointer_iface, seat, [](wl_resource* r) {
        auto* s = static_cast<SeatManager*>(wl_resource_get_user_data(r));
        if (s) s->removeResource(r);
    });
    if (seat) seat->addPointerResource(ptr);
    ALOGI("Seat: Client %p requested wl_pointer (id: %u)", client, id);
}

static void seat_get_keyboard(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    wl_resource* kbd = wl_resource_create(client, &wl_keyboard_interface,
                                          wl_resource_get_version(resource), id);
    if (!kbd) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(kbd, &keyboard_iface, seat, [](wl_resource* r) {
        auto* s = static_cast<SeatManager*>(wl_resource_get_user_data(r));
        if (s) s->removeResource(r);
    });

    if (seat) {
        seat->addKeyboardResource(kbd);
        int fd = seat->getKeymapFd();
        uint32_t sz = seat->getKeymapSize();
        if (fd >= 0 && sz > 0) {
            wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, sz);
        }
    }

    if (wl_resource_get_version(kbd) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
        wl_keyboard_send_repeat_info(kbd, 33, 500); // 33Hz repeat, 500ms delay
    }
    ALOGI("Seat: Client %p requested wl_keyboard (id: %u)", client, id);
}

static void seat_get_touch(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    wl_resource* touch = wl_resource_create(client, &wl_touch_interface,
                                            wl_resource_get_version(resource), id);
    if (!touch) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(touch, &touch_iface, seat, [](wl_resource* r) {
        auto* s = static_cast<SeatManager*>(wl_resource_get_user_data(r));
        if (s) s->removeResource(r);
    });
    if (seat) seat->addTouchResource(touch);
    ALOGI("Seat: Client %p requested wl_touch (id: %u)", client, id);
}

static void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct wl_seat_interface seat_iface = {
    .get_pointer  = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch    = seat_get_touch,
    .release      = seat_release,
};

// ─────────────────────────────────────────────────────────────────────────────
// zwp_virtual_keyboard_v1 implementation
// ─────────────────────────────────────────────────────────────────────────────
static void vkbd_keymap(wl_client* /*client*/, wl_resource* /*resource*/,
                        uint32_t format, int32_t fd, uint32_t size) {
    ALOGI("VirtualKeyboard: keymap supplied by client (format %u, size %u)", format, size);
    if (fd >= 0) {
        close(fd);
    }
}

static void vkbd_key(wl_client* /*client*/, wl_resource* resource,
                     uint32_t time, uint32_t key, uint32_t state) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    if (seat) {
        seat->injectVirtualKey(time, key, state);
    }
}

static void vkbd_modifiers(wl_client* /*client*/, wl_resource* resource,
                           uint32_t mods_depressed, uint32_t mods_latched,
                           uint32_t mods_locked, uint32_t group) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    if (seat) {
        seat->injectVirtualModifiers(mods_depressed, mods_latched, mods_locked, group);
    }
}

static void vkbd_destroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_virtual_keyboard_v1_interface vkbd_iface = {
    .keymap    = vkbd_keymap,
    .key       = vkbd_key,
    .modifiers = vkbd_modifiers,
    .destroy   = vkbd_destroy,
};

static void vkbd_mgr_create_virtual_keyboard(wl_client* client, wl_resource* resource,
                                             wl_resource* /*seat*/, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    wl_resource* vkbd_res = wl_resource_create(client, &zwp_virtual_keyboard_v1_interface, 1, id);
    if (!vkbd_res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(vkbd_res, &vkbd_iface, seat, nullptr);
    ALOGI("VirtualKeyboard: Client %p created virtual keyboard (id: %u)", client, id);
}

static const struct zwp_virtual_keyboard_manager_v1_interface vkbd_mgr_iface = {
    .create_virtual_keyboard = vkbd_mgr_create_virtual_keyboard,
};

// ─────────────────────────────────────────────────────────────────────────────
// zwp_text_input_v3 implementation
// ─────────────────────────────────────────────────────────────────────────────
static void text_input_destroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void text_input_enable(wl_client* /*client*/, wl_resource* resource) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.enabled = true;
    }
}

static void text_input_disable(wl_client* /*client*/, wl_resource* resource) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.enabled = false;
    }
}

static void text_input_set_surrounding_text(wl_client* /*client*/, wl_resource* resource,
                                            const char* text, int32_t cursor, int32_t anchor) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.surroundingText = text ? text : "";
        ti->pending.cursor = cursor;
        ti->pending.anchor = anchor;
        ti->pending.surroundingChanged = true;
    }
}

static void text_input_set_text_change_cause(wl_client* /*client*/, wl_resource* resource, uint32_t cause) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.textChangeCause = cause;
    }
}

static void text_input_set_content_type(wl_client* /*client*/, wl_resource* resource,
                                       uint32_t hint, uint32_t purpose) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.contentHint = hint;
        ti->pending.contentPurpose = purpose;
        ti->pending.contentTypeChanged = true;
    }
}

static void text_input_set_cursor_rectangle(wl_client* /*client*/, wl_resource* resource,
                                            int32_t x, int32_t y, int32_t width, int32_t height) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti) {
        ti->pending.cursorX = x;
        ti->pending.cursorY = y;
        ti->pending.cursorW = width;
        ti->pending.cursorH = height;
        ti->pending.cursorRectChanged = true;
    }
}

static void text_input_commit(wl_client* /*client*/, wl_resource* resource) {
    auto* ti = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(resource));
    if (ti && ti->impl) {
        ti->impl->handleTextInputCommit(ti);
    }
}

static const struct zwp_text_input_v3_interface text_input_v3_iface = {
    .destroy               = text_input_destroy,
    .enable                = text_input_enable,
    .disable               = text_input_disable,
    .set_surrounding_text  = text_input_set_surrounding_text,
    .set_text_change_cause = text_input_set_text_change_cause,
    .set_content_type      = text_input_set_content_type,
    .set_cursor_rectangle  = text_input_set_cursor_rectangle,
    .commit                = text_input_commit,
};

static void text_input_mgr_destroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void text_input_mgr_get_text_input(wl_client* client, wl_resource* resource,
                                          uint32_t id, struct wl_resource* /*seat*/) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    if (seat) {
        seat->createTextInput(client, id);
    }
}

static const struct zwp_text_input_manager_v3_interface text_input_mgr_iface = {
    .destroy        = text_input_mgr_destroy,
    .get_text_input = text_input_mgr_get_text_input,
};

// ─────────────────────────────────────────────────────────────────────────────
// zwp_input_method_v2 implementation
// ─────────────────────────────────────────────────────────────────────────────
static void im_commit_string(wl_client* /*client*/, wl_resource* resource, const char* text) {
    auto* im = static_cast<InputMethodV2Resource*>(wl_resource_get_user_data(resource));
    if (im && im->impl && im->impl->activeTextInput && im->impl->activeTextInput->resource) {
        zwp_text_input_v3_send_commit_string(im->impl->activeTextInput->resource, text ? text : "");
    }
}

static void im_set_preedit_string(wl_client* /*client*/, wl_resource* resource,
                                  const char* text, int32_t cursor_begin, int32_t cursor_end) {
    auto* im = static_cast<InputMethodV2Resource*>(wl_resource_get_user_data(resource));
    if (im && im->impl && im->impl->activeTextInput && im->impl->activeTextInput->resource) {
        zwp_text_input_v3_send_preedit_string(im->impl->activeTextInput->resource, text ? text : "",
                                              cursor_begin, cursor_end);
    }
}

static void im_delete_surrounding_text(wl_client* /*client*/, wl_resource* resource,
                                      uint32_t before_length, uint32_t after_length) {
    auto* im = static_cast<InputMethodV2Resource*>(wl_resource_get_user_data(resource));
    if (im && im->impl && im->impl->activeTextInput && im->impl->activeTextInput->resource) {
        zwp_text_input_v3_send_delete_surrounding_text(im->impl->activeTextInput->resource,
                                                      before_length, after_length);
    }
}

static void im_commit(wl_client* /*client*/, wl_resource* resource, uint32_t /*serial*/) {
    auto* im = static_cast<InputMethodV2Resource*>(wl_resource_get_user_data(resource));
    if (im && im->impl && im->impl->activeTextInput && im->impl->activeTextInput->resource) {
        uint32_t next_serial = wl_display_next_serial(im->impl->display);
        zwp_text_input_v3_send_done(im->impl->activeTextInput->resource, next_serial);
    }
}

static void im_popup_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

static const struct zwp_input_popup_surface_v2_interface im_popup_iface = {
    .destroy = im_popup_destroy,
};

static void im_get_input_popup_surface(wl_client* client, wl_resource* /*resource*/,
                                      uint32_t id, wl_resource* /*surface*/) {
    wl_resource* popup_res = wl_resource_create(client, &zwp_input_popup_surface_v2_interface, 1, id);
    if (!popup_res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(popup_res, &im_popup_iface, nullptr, nullptr);
}

static void im_grab_release(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

static const struct zwp_input_method_keyboard_grab_v2_interface im_grab_iface = {
    .release = im_grab_release,
};

static void im_grab_keyboard(wl_client* client, wl_resource* /*resource*/, uint32_t keyboard) {
    wl_resource* grab_res = wl_resource_create(client, &zwp_input_method_keyboard_grab_v2_interface, 1, keyboard);
    if (!grab_res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(grab_res, &im_grab_iface, nullptr, nullptr);
}

static void im_destroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_input_method_v2_interface im_iface = {
    .commit_string           = im_commit_string,
    .set_preedit_string      = im_set_preedit_string,
    .delete_surrounding_text = im_delete_surrounding_text,
    .commit                  = im_commit,
    .get_input_popup_surface = im_get_input_popup_surface,
    .grab_keyboard           = im_grab_keyboard,
    .destroy                 = im_destroy,
};

static void im_mgr_get_input_method(wl_client* client, wl_resource* resource,
                                    wl_resource* /*seat*/, uint32_t id) {
    auto* seat = static_cast<SeatManager*>(wl_resource_get_user_data(resource));
    if (seat) {
        seat->createInputMethod(client, id);
    }
}

static void im_mgr_destroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_input_method_manager_v2_interface im_mgr_iface = {
    .get_input_method = im_mgr_get_input_method,
    .destroy          = im_mgr_destroy,
};


// ─────────────────────────────────────────────────────────────────────────────
// SeatManager Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
SeatManager::SeatManager() : mImpl(std::make_unique<Impl>()) {}

SeatManager::~SeatManager() {
    stop();
}

bool SeatManager::init() {
    return true;
}

bool SeatManager::start(wl_display* display, wl_event_loop* loop, std::shared_ptr<SurfaceBridge> bridge) {
    mImpl->display = display;
    mImpl->eventLoop = loop;
    mImpl->bridge = bridge;

    mImpl->prepareKeymap();

    mImpl->gesture.holdTimerSource = wl_event_loop_add_timer(
        mImpl->eventLoop,
        [](void* data) -> int {
            auto* impl = static_cast<SeatManager::Impl*>(data);
            impl->handleHoldTimeout();
            return 0;
        },
        mImpl.get());

    DIR* dir = opendir("/dev/input");
    if (!dir) {
        ALOGE("Failed to open /dev/input: %s", strerror(errno));
        return false;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        enlistDevice(std::string("/dev/input/") + ent->d_name);
    }
    closedir(dir);

    // Setup inotify for dynamic USB/Bluetooth keyboard hotplugging
    int inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd >= 0) {
        if (inotify_add_watch(inotifyFd, "/dev/input", IN_CREATE | IN_DELETE) >= 0) {
            mImpl->inotifySource = wl_event_loop_add_fd(
                mImpl->eventLoop, inotifyFd, WL_EVENT_READABLE,
                [](int fd, uint32_t mask, void* data) -> int {
                    return static_cast<SeatManager*>(data)->handleInotifyEvent(fd, mask);
                },
                this);
            mImpl->inotifyFd = inotifyFd;
            ALOGI("SeatManager: Hardware input hotplug monitoring active on /dev/input");
        } else {
            close(inotifyFd);
        }
    }

    ALOGI("SeatManager started with %zu active input devices", mImpl->devices.size());
    return !mImpl->devices.empty();
}

void SeatManager::enlistDevice(const std::string& devPath) {
    for (const auto& d : mImpl->devices) {
        if (d.path == devPath) return; // already registered
    }

    int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("SeatManager: Cannot open %s: %s", devPath.c_str(), strerror(errno));
        return;
    }

    char name[256] = "Unknown";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);

    uint8_t evBits[(EV_MAX + 7) / 8] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);

    bool hasKey = evBits[EV_KEY / 8] & (1 << (EV_KEY % 8));
    bool hasRel = evBits[EV_REL / 8] & (1 << (EV_REL % 8));
    bool hasAbs = evBits[EV_ABS / 8] & (1 << (EV_ABS % 8));

    EvdevDevice dev;
    dev.fd = fd;
    dev.path = devPath;
    dev.name = name;

    if (hasAbs) {
        uint8_t absBits[(ABS_MAX + 7) / 8] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);

        if (absBits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8))) {
            dev.isTouch = true;
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &dev.absX);
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &dev.absY);
        } else if (absBits[ABS_X / 8] & (1 << (ABS_X % 8))) {
            dev.isPointer = true;
            ioctl(fd, EVIOCGABS(ABS_X), &dev.absX);
            ioctl(fd, EVIOCGABS(ABS_Y), &dev.absY);
        }
    }
    if (hasRel) {
        dev.isPointer = true;
    }
    if (hasKey) {
        uint8_t keyBits[(KEY_MAX + 7) / 8] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);

        bool hasAlpha = (keyBits[KEY_A / 8] & (1 << (KEY_A % 8))) &&
                        (keyBits[KEY_Z / 8] & (1 << (KEY_Z % 8))) &&
                        (keyBits[KEY_SPACE / 8] & (1 << (KEY_SPACE % 8)));
        bool hasEnter = (keyBits[KEY_ENTER / 8] & (1 << (KEY_ENTER % 8)));
        bool hasOtherKeys = (keyBits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8))) ||
                            (keyBits[KEY_POWER / 8] & (1 << (KEY_POWER % 8)));

        if (hasAlpha || hasEnter || hasOtherKeys) {
            dev.isKeyboard = true;
        }

        // Only genuine typing keyboards with full alphabet are grabbed exclusively.
        // Phone buttons (Volume, Power, Fingerprint) are NEVER grabbed!
        if (hasAlpha) {
            dev.isTypingKeyboard = true;
        }
    }

    dev.source = wl_event_loop_add_fd(
        mImpl->eventLoop, fd, WL_EVENT_READABLE,
        [](int fd, uint32_t mask, void* data) -> int {
            return static_cast<SeatManager*>(data)->handleEvdevEvent(fd, mask);
        },
        this);

    ALOGI("Enlisted input device %s (%s): touch=%d, kbd=%d (typing=%d), ptr=%d, X=[%d..%d], Y=[%d..%d]",
          devPath.c_str(), name, dev.isTouch, dev.isKeyboard, dev.isTypingKeyboard, dev.isPointer,
          dev.absX.minimum, dev.absX.maximum, dev.absY.minimum, dev.absY.maximum);

    // If a Wayland window is currently focused, grab this newly plugged typing keyboard immediately
    if (dev.isTypingKeyboard && mImpl->currentKeyboardSurface != nullptr) {
        if (ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
            dev.isGrabbed = true;
            ALOGI("SeatManager: Grabbed hotplugged typing keyboard %s (%s)", dev.path.c_str(), dev.name.c_str());
        }
    }

    mImpl->devices.push_back(std::move(dev));
}

void SeatManager::removeDevice(const std::string& devPath) {
    auto it = std::find_if(mImpl->devices.begin(), mImpl->devices.end(),
                           [&devPath](const EvdevDevice& d) { return d.path == devPath; });
    if (it != mImpl->devices.end()) {
        ALOGI("SeatManager: Disconnecting input device %s (%s)", it->path.c_str(), it->name.c_str());
        if (it->isGrabbed && it->fd >= 0) {
            ioctl(it->fd, EVIOCGRAB, 0);
            it->isGrabbed = false;
        }
        if (it->source) {
            wl_event_source_remove(it->source);
            it->source = nullptr;
        }
        if (it->fd >= 0) {
            close(it->fd);
            it->fd = -1;
        }
        mImpl->devices.erase(it);
    }
}

int SeatManager::handleInotifyEvent(int fd, uint32_t mask) {
    alignas(struct inotify_event) char buf[1024];
    ssize_t len = read(fd, buf, sizeof(buf));
    if (len <= 0) return 1;

    for (char* ptr = buf; ptr < buf + len; ) {
        auto* event = reinterpret_cast<struct inotify_event*>(ptr);
        if (event->len > 0 && strncmp(event->name, "event", 5) == 0) {
            std::string devPath = "/dev/input/" + std::string(event->name);
            if (event->mask & IN_CREATE) {
                ALOGI("SeatManager: Hotplug event: device added %s", devPath.c_str());
                enlistDevice(devPath);
            } else if (event->mask & IN_DELETE) {
                ALOGI("SeatManager: Hotplug event: device removed %s", devPath.c_str());
                removeDevice(devPath);
            }
        }
        ptr += sizeof(struct inotify_event) + event->len;
    }
    return 1;
}

void SeatManager::updateKeyboardGrabs(bool grab) {
    for (auto& dev : mImpl->devices) {
        if (!dev.isTypingKeyboard || dev.fd < 0) continue;
        if (grab && !dev.isGrabbed) {
            if (ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
                dev.isGrabbed = true;
                ALOGI("SeatManager: Grabbed hardware keyboard %s (%s) for focused window",
                      dev.path.c_str(), dev.name.c_str());
            } else {
                ALOGW("SeatManager: Failed to grab keyboard %s: %s", dev.path.c_str(), strerror(errno));
            }
        } else if (!grab && dev.isGrabbed) {
            ioctl(dev.fd, EVIOCGRAB, 0);
            dev.isGrabbed = false;
            ALOGI("SeatManager: Released hardware keyboard %s (%s) back to Android",
                  dev.path.c_str(), dev.name.c_str());
        }
    }
}

void SeatManager::run() {
    mRunning = true;
    while (mRunning) {
        usleep(100000);
    }
}

void SeatManager::stop() {
    mRunning = false;
    updateKeyboardGrabs(false);

    if (mImpl->inotifySource) {
        wl_event_source_remove(mImpl->inotifySource);
        mImpl->inotifySource = nullptr;
    }
    if (mImpl->inotifyFd >= 0) {
        close(mImpl->inotifyFd);
        mImpl->inotifyFd = -1;
    }

    if (mImpl->gesture.holdTimerSource) {
        wl_event_source_remove(mImpl->gesture.holdTimerSource);
        mImpl->gesture.holdTimerSource = nullptr;
    }
    for (auto& dev : mImpl->devices) {
        if (dev.isGrabbed && dev.fd >= 0) {
            ioctl(dev.fd, EVIOCGRAB, 0);
            dev.isGrabbed = false;
        }
        if (dev.source) {
            wl_event_source_remove(dev.source);
            dev.source = nullptr;
        }
        if (dev.fd >= 0) {
            close(dev.fd);
            dev.fd = -1;
        }
    }
    mImpl->devices.clear();

    if (mImpl->keymapFd >= 0) {
        close(mImpl->keymapFd);
        mImpl->keymapFd = -1;
    }
}

void SeatManager::bindSeat(wl_client* client, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_seat_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_iface, this, [](wl_resource* r) {
        auto* s = static_cast<SeatManager*>(wl_resource_get_user_data(r));
        if (s) s->removeResource(r);
    });

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER
                  | WL_SEAT_CAPABILITY_KEYBOARD
                  | WL_SEAT_CAPABILITY_TOUCH;
    wl_seat_send_capabilities(resource, caps);

    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "default");
    }

    mImpl->seatResources.push_back(resource);
}

void SeatManager::bindVirtualKeyboardManager(wl_client* client, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_virtual_keyboard_manager_v1_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &vkbd_mgr_iface, this, nullptr);
    ALOGI("VirtualKeyboardManager bound by client %p (id: %u)", client, id);
}

void SeatManager::bindTextInputManager(wl_client* client, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &text_input_mgr_iface, this, nullptr);
    ALOGI("TextInputManager bound by client %p (id: %u)", client, id);
}

void SeatManager::createTextInput(wl_client* client, uint32_t id) {
    wl_resource* ti_res = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
    if (!ti_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* ti = new TextInputV3Resource();
    ti->resource = ti_res;
    ti->client = client;
    ti->impl = mImpl.get();
    mImpl->textInputs.push_back(ti);

    wl_resource_set_implementation(ti_res, &text_input_v3_iface, ti, [](wl_resource* r) {
        auto* res = static_cast<TextInputV3Resource*>(wl_resource_get_user_data(r));
        if (res) {
            if (res->impl) {
                auto it = std::find(res->impl->textInputs.begin(), res->impl->textInputs.end(), res);
                if (it != res->impl->textInputs.end()) {
                    res->impl->textInputs.erase(it);
                }
                if (res->impl->activeTextInput == res) {
                    res->impl->activeTextInput = nullptr;
                    res->impl->sendInputMethodDeactivate();
                }
            }
            delete res;
        }
    });

    if (mImpl->currentKeyboardSurface && wl_resource_get_client(mImpl->currentKeyboardSurface) == client) {
        ti->focusedSurface = mImpl->currentKeyboardSurface;
        zwp_text_input_v3_send_enter(ti->resource, mImpl->currentKeyboardSurface);
    }
    ALOGI("TextInput: Client %p created text input (id: %u)", client, id);
}

void SeatManager::bindInputMethodManager(wl_client* client, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &zwp_input_method_manager_v2_interface,
                                               static_cast<int>(version), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &im_mgr_iface, this, nullptr);
    ALOGI("InputMethodManager bound by client %p (id: %u)", client, id);
}

void SeatManager::createInputMethod(wl_client* client, uint32_t id) {
    wl_resource* im_res = wl_resource_create(client, &zwp_input_method_v2_interface, 1, id);
    if (!im_res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* im = new InputMethodV2Resource();
    im->resource = im_res;
    im->client = client;
    im->impl = mImpl.get();
    mImpl->inputMethods.push_back(im);

    wl_resource_set_implementation(im_res, &im_iface, im, [](wl_resource* r) {
        auto* res = static_cast<InputMethodV2Resource*>(wl_resource_get_user_data(r));
        if (res) {
            if (res->impl) {
                auto it = std::find(res->impl->inputMethods.begin(), res->impl->inputMethods.end(), res);
                if (it != res->impl->inputMethods.end()) {
                    res->impl->inputMethods.erase(it);
                }
            }
            delete res;
        }
    });

    ALOGI("InputMethod: Client %p created input method (id: %u)", client, id);

    if (mImpl->activeTextInput && mImpl->activeTextInput->current.enabled) {
        zwp_input_method_v2_send_activate(im->resource);
        if (mImpl->activeTextInput->current.surroundingChanged) {
            zwp_input_method_v2_send_surrounding_text(
                im->resource,
                mImpl->activeTextInput->current.surroundingText.c_str(),
                mImpl->activeTextInput->current.cursor,
                mImpl->activeTextInput->current.anchor);
        }
        if (mImpl->activeTextInput->current.contentTypeChanged) {
            zwp_input_method_v2_send_content_type(
                im->resource,
                mImpl->activeTextInput->current.contentHint,
                mImpl->activeTextInput->current.contentPurpose);
        }
        im->serial++;
        zwp_input_method_v2_send_done(im->resource);
        im->active = true;
    }
}

void SeatManager::injectVirtualKey(uint32_t timeMs, uint32_t key, uint32_t state) {
    if (!mImpl->display) return;
    if (timeMs == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        timeMs = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }

    // Dismiss virtual keyboard if Escape is tapped on the virtual keyboard
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (mImpl->activeTextInput) {
            ALOGI("VirtualKeyboard: ESC key pressed -> dismissing virtual keyboard");
            mImpl->activeTextInput = nullptr;
            mImpl->sendInputMethodDeactivate();
        }
    }

    uint32_t serial = wl_display_next_serial(mImpl->display);
    if (mImpl->currentKeyboardSurface) {
        struct wl_client* targetClient = wl_resource_get_client(mImpl->currentKeyboardSurface);
        for (wl_resource* kbd : mImpl->keyboardResources) {
            if (wl_resource_get_client(kbd) == targetClient) {
                wl_keyboard_send_key(kbd, serial, timeMs, key, state);
            }
        }
    } else {
        ALOGI("VirtualKeyboard: Key event (key=%u, state=%u) dropped: no focused surface", key, state);
        mImpl->sendInputMethodDeactivate();
    }
}

void SeatManager::injectVirtualModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (!mImpl->display) return;
    uint32_t serial = wl_display_next_serial(mImpl->display);
    if (mImpl->currentKeyboardSurface) {
        struct wl_client* targetClient = wl_resource_get_client(mImpl->currentKeyboardSurface);
        for (wl_resource* kbd : mImpl->keyboardResources) {
            if (wl_resource_get_client(kbd) == targetClient) {
                wl_keyboard_send_modifiers(kbd, serial, depressed, latched, locked, group);
            }
        }
    }
}


void SeatManager::addPointerResource(wl_resource* resource) {
    mImpl->pointerResources.push_back(resource);
}

void SeatManager::addKeyboardResource(wl_resource* resource) {
    mImpl->keyboardResources.push_back(resource);
}

void SeatManager::addTouchResource(wl_resource* resource) {
    mImpl->touchResources.push_back(resource);
}

void SeatManager::removeResource(wl_resource* resource) {
    auto eraseFrom = [resource](std::vector<wl_resource*>& vec) {
        vec.erase(std::remove(vec.begin(), vec.end(), resource), vec.end());
    };
    eraseFrom(mImpl->seatResources);
    eraseFrom(mImpl->pointerResources);
    eraseFrom(mImpl->keyboardResources);
    eraseFrom(mImpl->touchResources);

    if (mImpl->currentKeyboardSurface == resource) mImpl->currentKeyboardSurface = nullptr;
    if (mImpl->currentPointerSurface == resource) mImpl->currentPointerSurface = nullptr;
    if (mImpl->gesture.targetSurfaceResource == resource) {
        if (mImpl->gesture.holdTimerSource) {
            wl_event_source_timer_update(mImpl->gesture.holdTimerSource, 0);
        }
        mImpl->gesture.state = PointerGestureState::IDLE;
        mImpl->gesture.targetSurface = nullptr;
        mImpl->gesture.targetSurfaceResource = nullptr;
    }
}

int SeatManager::getKeymapFd() const {
    return mImpl->keymapFd;
}

uint32_t SeatManager::getKeymapSize() const {
    return static_cast<uint32_t>(mImpl->keymapSize);
}

int32_t SeatManager::getLastTouchScreenX() const {
    return mImpl ? mImpl->touchSlots[0].screenX : 0;
}

int32_t SeatManager::getLastTouchScreenY() const {
    return mImpl ? mImpl->touchSlots[0].screenY : 0;
}

std::shared_ptr<SurfaceBridge> SeatManager::getBridge() const {
    return mImpl ? mImpl->bridge : nullptr;
}

void SeatManager::notifySurfaceDestroyed(wl_resource* surfaceResource) {
    if (mImpl->currentKeyboardSurface == surfaceResource) mImpl->currentKeyboardSurface = nullptr;
    if (mImpl->currentPointerSurface == surfaceResource) mImpl->currentPointerSurface = nullptr;
    if (mImpl->gesture.targetSurfaceResource == surfaceResource) {
        if (mImpl->gesture.holdTimerSource) {
            wl_event_source_timer_update(mImpl->gesture.holdTimerSource, 0);
        }
        mImpl->gesture.state = PointerGestureState::IDLE;
        mImpl->gesture.targetSurface = nullptr;
        mImpl->gesture.targetSurfaceResource = nullptr;
    }

    for (int s = 0; s < Impl::MAX_TOUCH_SLOTS; ++s) {
        if (mImpl->touchSlots[s].targetSurfaceResource == surfaceResource) {
            mImpl->touchSlots[s].targetSurface = nullptr;
            mImpl->touchSlots[s].targetSurfaceResource = nullptr;
            mImpl->touchSlots[s].down = false;
        }
    }

    for (auto* ti : mImpl->textInputs) {
        if (ti->focusedSurface == surfaceResource) {
            ti->focusedSurface = nullptr;
            ti->pending.enabled = false;
            ti->current.enabled = false;
            if (mImpl->activeTextInput == ti) {
                mImpl->activeTextInput = nullptr;
                mImpl->sendInputMethodDeactivate();
            }
        }
    }
}

void SeatManager::setKeyboardFocus(wl_resource* surfaceResource) {
    if (mImpl->currentKeyboardSurface == surfaceResource) return;

    uint32_t serial = wl_display_next_serial(mImpl->display);

    if (mImpl->currentKeyboardSurface) {
        struct wl_client* oldClient = wl_resource_get_client(mImpl->currentKeyboardSurface);
        for (wl_resource* kbd : mImpl->keyboardResources) {
            if (wl_resource_get_client(kbd) == oldClient) {
                wl_keyboard_send_leave(kbd, serial, mImpl->currentKeyboardSurface);
            }
        }
        for (auto* ti : mImpl->textInputs) {
            if (ti->client == oldClient) {
                zwp_text_input_v3_send_leave(ti->resource, mImpl->currentKeyboardSurface);
                ti->focusedSurface = nullptr;
                ti->pending.enabled = false;
                ti->current.enabled = false;
                if (mImpl->activeTextInput == ti) {
                    mImpl->activeTextInput = nullptr;
                    mImpl->sendInputMethodDeactivate();
                }
            }
        }
    }

    mImpl->currentKeyboardSurface = surfaceResource;

    if (mImpl->currentKeyboardSurface) {
        struct wl_client* newClient = wl_resource_get_client(mImpl->currentKeyboardSurface);
        struct wl_array keys;
        wl_array_init(&keys);
        for (wl_resource* kbd : mImpl->keyboardResources) {
            if (wl_resource_get_client(kbd) == newClient) {
                wl_keyboard_send_enter(kbd, serial, mImpl->currentKeyboardSurface, &keys);
            }
        }
        wl_array_release(&keys);

        for (auto* ti : mImpl->textInputs) {
            if (ti->client == newClient) {
                ti->focusedSurface = mImpl->currentKeyboardSurface;
                zwp_text_input_v3_send_enter(ti->resource, mImpl->currentKeyboardSurface);
            }
        }

        // A Wayland window now has keyboard focus:
        // Grab typing keyboards exclusively to prevent double-input to Android
        updateKeyboardGrabs(true);
    } else {
        // No Wayland window has focus:
        mImpl->activeTextInput = nullptr;
        mImpl->sendInputMethodDeactivate();
        // Release typing keyboards so Android can receive all inputs
        updateKeyboardGrabs(false);
    }
}

void SeatManager::setPointerFocus(wl_resource* surfaceResource) {
    if (mImpl->currentPointerSurface == surfaceResource) return;
    uint32_t serial = wl_display_next_serial(mImpl->display);

    if (mImpl->currentPointerSurface) {
        struct wl_client* oldClient = wl_resource_get_client(mImpl->currentPointerSurface);
        for (wl_resource* ptr : mImpl->pointerResources) {
            if (wl_resource_get_client(ptr) == oldClient) {
                wl_pointer_send_leave(ptr, serial, mImpl->currentPointerSurface);
                wl_pointer_send_frame(ptr);
            }
        }
    }

    mImpl->currentPointerSurface = surfaceResource;

    if (mImpl->currentPointerSurface) {
        struct wl_client* newClient = wl_resource_get_client(mImpl->currentPointerSurface);
        for (wl_resource* ptr : mImpl->pointerResources) {
            if (wl_resource_get_client(ptr) == newClient) {
                wl_pointer_send_enter(ptr, serial, mImpl->currentPointerSurface,
                                      wl_fixed_from_int(0), wl_fixed_from_int(0));
                wl_pointer_send_frame(ptr);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Event Parsing & Dispatching
// ─────────────────────────────────────────────────────────────────────────────
static bool isNonTouchApp(const WaylandSurface* surf) {
    if (!surf) return false;
    return surf->isXwayland;
}

int SeatManager::handleEvdevEvent(int fd, uint32_t mask) {
    if (!(mask & WL_EVENT_READABLE)) return 0;

    EvdevDevice* dev = nullptr;
    for (auto& d : mImpl->devices) {
        if (d.fd == fd) {
            dev = &d;
            break;
        }
    }
    if (!dev) return 0;

    int32_t dispW = mImpl->bridge ? mImpl->bridge->displayWidth() : 720;
    int32_t dispH = mImpl->bridge ? mImpl->bridge->displayHeight() : 1600;

    struct input_event evs[64];
    while (true) {
        ssize_t bytes = read(fd, evs, sizeof(evs));
        if (bytes < static_cast<ssize_t>(sizeof(struct input_event))) {
            break;
        }
        int count = static_cast<int>(bytes / sizeof(struct input_event));
        for (int i = 0; i < count; ++i) {
            const auto& ev = evs[i];

            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_SLOT) {
                    if (ev.value >= 0 && ev.value < Impl::MAX_TOUCH_SLOTS) {
                        mImpl->currentSlot = ev.value;
                    }
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    TouchSlot& slot = mImpl->touchSlots[mImpl->currentSlot];
                    slot.trackingId = ev.value;
                    slot.modified = true;
                } else if (ev.code == ABS_MT_POSITION_X) {
                    TouchSlot& slot = mImpl->touchSlots[mImpl->currentSlot];
                    slot.screenX = scaleCoord(ev.value, dev->absX.minimum, dev->absX.maximum, dispW);
                    slot.modified = true;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    TouchSlot& slot = mImpl->touchSlots[mImpl->currentSlot];
                    slot.screenY = scaleCoord(ev.value, dev->absY.minimum, dev->absY.maximum, dispH);
                    slot.modified = true;
                } else if (ev.code == ABS_X && !dev->isTouch) {
                    mImpl->pointerScreenX = scaleCoord(ev.value, dev->absX.minimum, dev->absX.maximum, dispW);
                    mImpl->pointerModified = true;
                } else if (ev.code == ABS_Y && !dev->isTouch) {
                    mImpl->pointerScreenY = scaleCoord(ev.value, dev->absY.minimum, dev->absY.maximum, dispH);
                    mImpl->pointerModified = true;
                }
            } else if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    mImpl->pointerScreenX = std::clamp(mImpl->pointerScreenX + ev.value, 0, dispW - 1);
                    mImpl->pointerModified = true;
                } else if (ev.code == REL_Y) {
                    mImpl->pointerScreenY = std::clamp(mImpl->pointerScreenY + ev.value, 0, dispH - 1);
                    mImpl->pointerModified = true;
                } else if (ev.code == REL_WHEEL) {
                    uint32_t timeMs = static_cast<uint32_t>(ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000);
                    wl_fixed_t value = wl_fixed_from_double(-ev.value * 10.0);
                    for (wl_resource* ptr : mImpl->pointerResources) {
                        wl_pointer_send_axis(ptr, timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, value);
                        wl_pointer_send_frame(ptr);
                    }
                }
            } else if (ev.type == EV_KEY) {
                uint32_t timeMs = static_cast<uint32_t>(ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000);
                if (ev.code == BTN_TOUCH) {
                    if (ev.value == 0 && mImpl->touchSlots[0].down) {
                        mImpl->touchSlots[0].trackingId = -1;
                        mImpl->touchSlots[0].modified = true;
                    }
                } else if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    uint32_t btn = ev.code;
                    uint32_t state = ev.value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
                    if (btn == BTN_LEFT) {
                        if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
                            if (mImpl->bridge && mImpl->bridge->isMoveGrabActive()) {
                                mImpl->bridge->endMoveGrab();
                            }
                        } else if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
                            if (mImpl->bridge && mImpl->currentPointerSurface) {
                                WaylandSurface* surf = mImpl->bridge->surfaceFromResource(mImpl->currentPointerSurface);
                                if (surf && (!surf->isLayerSurface || surf->layerKeyboardInteractivity)) {
                                    mImpl->bridge->activateSurface(surf);
                                }
                            }
                        }
                    }
                    uint32_t serial = wl_display_next_serial(mImpl->display);
                    if (mImpl->currentPointerSurface) {
                        struct wl_client* targetClient = wl_resource_get_client(mImpl->currentPointerSurface);
                        for (wl_resource* ptr : mImpl->pointerResources) {
                            if (wl_resource_get_client(ptr) == targetClient) {
                                wl_pointer_send_button(ptr, serial, timeMs, btn, state);
                                wl_pointer_send_frame(ptr);
                            }
                        }
                    }
                } else if (ev.code < BTN_MISC) { // Standard keyboard keys
                    // Super/Windows key releases keyboard grab back to Android
                    if (ev.value == 1 && (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA)) {
                        ALOGI("SeatManager: Super/Meta key pressed -> yielding keyboard focus to Android");
                        setKeyboardFocus(nullptr);
                        return 1;
                    }

                    // Android Back key or Home key dismisses active virtual keyboard
                    if (ev.value == 1 && (ev.code == KEY_BACK || ev.code == KEY_HOMEPAGE)) {
                        if (mImpl->activeTextInput) {
                            ALOGI("SeatManager: KEY_BACK pressed -> dismissing active virtual keyboard");
                            mImpl->activeTextInput = nullptr;
                            mImpl->sendInputMethodDeactivate();
                            return 1;
                        }
                    }

                    uint32_t serial = wl_display_next_serial(mImpl->display);
                    uint32_t state = ev.value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
                    if (mImpl->currentKeyboardSurface) {
                        struct wl_client* targetClient = wl_resource_get_client(mImpl->currentKeyboardSurface);
                        for (wl_resource* kbd : mImpl->keyboardResources) {
                            if (wl_resource_get_client(kbd) == targetClient) {
                                wl_keyboard_send_key(kbd, serial, timeMs, ev.code, state);
                            }
                        }
                    }
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                uint32_t timeMs = static_cast<uint32_t>(ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000);

                // Multi-touch dispatch
                for (int s = 0; s < Impl::MAX_TOUCH_SLOTS; ++s) {
                    TouchSlot& slot = mImpl->touchSlots[s];
                    if (!slot.modified) continue;
                    slot.modified = false;

                    uint32_t serial = wl_display_next_serial(mImpl->display);

                    // Case 1: Touch DOWN
                    if (slot.trackingId >= 0 && !slot.down) {
                        slot.down = true;
                        int32_t localX = 0, localY = 0;
                        WaylandSurface* surf = mImpl->bridge ? mImpl->bridge->surfaceAt(slot.screenX, slot.screenY, &localX, &localY) : nullptr;
                        slot.targetSurface = surf;
                        slot.targetSurfaceResource = surf ? surf->resource : nullptr;
                        slot.localX = localX;
                        slot.localY = localY;

                        // Check if touched on the server-side titlebar
                        if (surf && surf->hasDecor && localY < 0) {
                            mImpl->bridge->activateSurface(surf);
                            int32_t w = surf->committed.width;
                            if (localX >= w - 44) {
                                ALOGI("Titlebar: Close button hit for surface %u", surf->id);
                                if (mImpl->currentKeyboardSurface == surf->resource) {
                                    setKeyboardFocus(nullptr);
                                }
                                mImpl->bridge->closeSurface(surf);
                            } else if (localX >= w - 88) {
                                ALOGI("Titlebar: Maximize button hit for surface %u", surf->id);
                                mImpl->bridge->toggleMaximize(surf);
                            } else if (localX >= w - 132) {
                                ALOGI("Titlebar: Minimize button hit for surface %u", surf->id);
                                if (mImpl->currentKeyboardSurface == surf->resource) {
                                    setKeyboardFocus(nullptr);
                                }
                                mImpl->bridge->minimizeSurface(surf);
                            } else {
                                ALOGI("Titlebar: Drag grab started for surface %u", surf->id);
                                mImpl->bridge->startMoveGrab(surf, slot.screenX, slot.screenY);
                            }
                            continue;
                        }

                        if (surf && surf->resource) {
                            if (surf->isLayerSurface && !mImpl->currentKeyboardSurface) {
                                ALOGI("SeatManager: Layer surface touched without active Wayland focus -> dismissing virtual keyboard");
                                mImpl->sendInputMethodDeactivate();
                                continue;
                            }
                            if (!surf->isLayerSurface || surf->layerKeyboardInteractivity) {
                                mImpl->bridge->activateSurface(surf);
                                setKeyboardFocus(surf->resource);
                            }

                            struct wl_client* targetClient = wl_resource_get_client(surf->resource);

                            bool hasTouchResource = false;
                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    hasTouchResource = true;
                                    break;
                                }
                            }

                            bool isNativeTouch = hasTouchResource && !isNonTouchApp(surf);
                            if (isNativeTouch) {
                                for (wl_resource* touch : mImpl->touchResources) {
                                    if (wl_resource_get_client(touch) == targetClient) {
                                        wl_touch_send_down(touch, serial, timeMs, surf->resource, s,
                                                           wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                        wl_touch_send_frame(touch);
                                    }
                                }
                            }

                            // Pointer emulation & gesture handling for slot 0 for non-touch apps
                            if (s == 0 && !isNativeTouch) {
                                if (mImpl->currentPointerSurface != surf->resource) {
                                    if (mImpl->currentPointerSurface) {
                                        for (wl_resource* oldPtr : mImpl->pointerResources) {
                                            if (wl_resource_get_client(oldPtr) == wl_resource_get_client(mImpl->currentPointerSurface)) {
                                                wl_pointer_send_leave(oldPtr, serial, mImpl->currentPointerSurface);
                                                wl_pointer_send_frame(oldPtr);
                                            }
                                        }
                                    }
                                    mImpl->currentPointerSurface = surf->resource;
                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            wl_pointer_send_enter(ptr, serial, surf->resource,
                                                                  wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                } else {
                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            wl_pointer_send_motion(ptr, timeMs,
                                                                   wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                }

                                mImpl->pointerScreenX = slot.screenX;
                                mImpl->pointerScreenY = slot.screenY;

                                mImpl->gesture.startScreenX = slot.screenX;
                                mImpl->gesture.startScreenY = slot.screenY;
                                mImpl->gesture.lastScreenX  = slot.screenX;
                                mImpl->gesture.lastScreenY  = slot.screenY;
                                mImpl->gesture.localX       = localX;
                                mImpl->gesture.localY       = localY;
                                mImpl->gesture.downTimeMs   = timeMs;
                                mImpl->gesture.targetSurface = surf;
                                mImpl->gesture.targetSurfaceResource = surf->resource;
                                mImpl->gesture.state        = PointerGestureState::DRAGGING_SELECTION;

                                for (wl_resource* ptr : mImpl->pointerResources) {
                                    if (wl_resource_get_client(ptr) == targetClient) {
                                        wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                                        wl_pointer_send_frame(ptr);
                                    }
                                }

                                if (mImpl->gesture.holdTimerSource) {
                                    wl_event_source_timer_update(mImpl->gesture.holdTimerSource, 500);
                                }
                                ALOGI("Pointer Emulation: Touch DOWN -> BTN_LEFT PRESSED at (%d, %d)", localX, localY);
                            }
                            ALOGI("Touch DOWN on surface %u (client %p, hasTouch=%d) at screen (%d, %d) -> local (%d, %d)",
                                  surf->id, targetClient, hasTouchResource, slot.screenX, slot.screenY, localX, localY);
                        } else {
                            ALOGI("Touch DOWN at screen (%d, %d) outside any window", slot.screenX, slot.screenY);
                            mImpl->activeTextInput = nullptr;
                            mImpl->sendInputMethodDeactivate();
                            if (mImpl->currentKeyboardSurface) {
                                ALOGI("SeatManager: Touch outside Wayland windows -> releasing keyboard focus to Android");
                                setKeyboardFocus(nullptr);
                            }
                        }
                    }
                    // Case 2: Touch MOTION
                    else if (slot.trackingId >= 0 && slot.down) {
                        if (mImpl->bridge && mImpl->bridge->isMoveGrabActive()) {
                            mImpl->bridge->updateMoveGrab(slot.screenX, slot.screenY);
                            continue;
                        }

                        if (slot.targetSurface && slot.targetSurfaceResource) {
                            WaylandSurface* surf = slot.targetSurface;
                            int32_t localX = surf ? (slot.screenX - surf->committed.x) : slot.localX;
                            int32_t localY = surf ? (slot.screenY - surf->committed.y) : slot.localY;
                            slot.localX = localX;
                            slot.localY = localY;

                            struct wl_client* targetClient = wl_resource_get_client(slot.targetSurfaceResource);

                            bool hasTouchResource = false;
                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    hasTouchResource = true;
                                    break;
                                }
                            }

                            bool isNativeTouch = hasTouchResource && !isNonTouchApp(surf);
                            if (isNativeTouch) {
                                for (wl_resource* touch : mImpl->touchResources) {
                                    if (wl_resource_get_client(touch) == targetClient) {
                                        wl_touch_send_motion(touch, timeMs, s,
                                                             wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                        wl_touch_send_frame(touch);
                                    }
                                }
                            }

                            if (s == 0 && !isNativeTouch) {
                                mImpl->gesture.localX = localX;
                                mImpl->gesture.localY = localY;
                                mImpl->pointerScreenX = slot.screenX;
                                mImpl->pointerScreenY = slot.screenY;

                                int32_t dx = slot.screenX - mImpl->gesture.startScreenX;
                                int32_t dy = slot.screenY - mImpl->gesture.startScreenY;
                                if ((dx * dx + dy * dy) >= 10 * 10) {
                                    if (mImpl->gesture.holdTimerSource) {
                                        wl_event_source_timer_update(mImpl->gesture.holdTimerSource, 0);
                                    }
                                }

                                // If a second finger is touching, treat motion as scrolling!
                                bool twoFingersDown = (mImpl->touchSlots[1].trackingId >= 0 && mImpl->touchSlots[1].down);
                                if (twoFingersDown) {
                                    if (mImpl->gesture.state != PointerGestureState::SCROLLING) {
                                        mImpl->gesture.state = PointerGestureState::SCROLLING;
                                        for (wl_resource* ptr : mImpl->pointerResources) {
                                            if (wl_resource_get_client(ptr) == targetClient) {
                                                wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                                                wl_pointer_send_frame(ptr);
                                            }
                                        }
                                    }
                                    int32_t deltaX = slot.screenX - mImpl->gesture.lastScreenX;
                                    int32_t deltaY = slot.screenY - mImpl->gesture.lastScreenY;
                                    mImpl->gesture.lastScreenX = slot.screenX;
                                    mImpl->gesture.lastScreenY = slot.screenY;

                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            if (deltaY != 0) {
                                                wl_pointer_send_axis(ptr, timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                                     wl_fixed_from_double(-deltaY * 2.0));
                                            }
                                            if (deltaX != 0) {
                                                wl_pointer_send_axis(ptr, timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                                                     wl_fixed_from_double(-deltaX * 2.0));
                                            }
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                } else if (mImpl->gesture.state == PointerGestureState::DRAGGING_SELECTION) {
                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            wl_pointer_send_motion(ptr, timeMs,
                                                                   wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // Case 3: Touch UP
                    else if (slot.trackingId == -1 && slot.down) {
                        slot.down = false;
                        if (mImpl->bridge && mImpl->bridge->isMoveGrabActive()) {
                            mImpl->bridge->endMoveGrab();
                        }

                        if (slot.targetSurfaceResource) {
                            struct wl_client* targetClient = wl_resource_get_client(slot.targetSurfaceResource);

                            bool hasTouchResource = false;
                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    hasTouchResource = true;
                                    break;
                                }
                            }

                            bool isNativeTouch = hasTouchResource && !isNonTouchApp(slot.targetSurface);
                            if (isNativeTouch) {
                                for (wl_resource* touch : mImpl->touchResources) {
                                    if (wl_resource_get_client(touch) == targetClient) {
                                        wl_touch_send_up(touch, serial, timeMs, s);
                                        wl_touch_send_frame(touch);
                                    }
                                }
                            }

                            if (s == 0 && !isNativeTouch) {
                                if (mImpl->gesture.holdTimerSource) {
                                    wl_event_source_timer_update(mImpl->gesture.holdTimerSource, 0);
                                }

                                if (mImpl->gesture.state == PointerGestureState::HOLD_TRIGGERED) {
                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            wl_pointer_send_button(ptr, serial, timeMs, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                    ALOGI("Pointer Emulation: HOLD ended -> released BTN_RIGHT");
                                } else if (mImpl->gesture.state == PointerGestureState::DRAGGING_SELECTION) {
                                    for (wl_resource* ptr : mImpl->pointerResources) {
                                        if (wl_resource_get_client(ptr) == targetClient) {
                                            wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                                            wl_pointer_send_frame(ptr);
                                        }
                                    }
                                    ALOGI("Pointer Emulation: released BTN_LEFT at local (%d, %d)", mImpl->gesture.localX, mImpl->gesture.localY);
                                }

                                mImpl->gesture.state = PointerGestureState::IDLE;
                                mImpl->gesture.targetSurface = nullptr;
                                mImpl->gesture.targetSurfaceResource = nullptr;
                            }
                            ALOGI("Touch UP for slot %d", s);
                        }
                        slot.targetSurface = nullptr;
                        slot.targetSurfaceResource = nullptr;
                    }
                }

                // Mouse/pointer motion
                if (mImpl->pointerModified) {
                    mImpl->pointerModified = false;
                    if (mImpl->bridge && mImpl->bridge->isMoveGrabActive()) {
                        mImpl->bridge->updateMoveGrab(mImpl->pointerScreenX, mImpl->pointerScreenY);
                    } else {
                        uint32_t serial = wl_display_next_serial(mImpl->display);
                        int32_t localX = 0, localY = 0;
                        WaylandSurface* surf = mImpl->bridge ? mImpl->bridge->surfaceAt(mImpl->pointerScreenX, mImpl->pointerScreenY, &localX, &localY) : nullptr;
                        if (surf && surf->resource) {
                        struct wl_client* targetClient = wl_resource_get_client(surf->resource);
                        for (wl_resource* ptr : mImpl->pointerResources) {
                            if (wl_resource_get_client(ptr) == targetClient) {
                                if (mImpl->currentPointerSurface != surf->resource) {
                                    if (mImpl->currentPointerSurface) {
                                        for (wl_resource* oldPtr : mImpl->pointerResources) {
                                            if (wl_resource_get_client(oldPtr) == wl_resource_get_client(mImpl->currentPointerSurface)) {
                                                wl_pointer_send_leave(oldPtr, serial, mImpl->currentPointerSurface);
                                                wl_pointer_send_frame(oldPtr);
                                            }
                                        }
                                    }
                                    mImpl->currentPointerSurface = surf->resource;
                                    wl_pointer_send_enter(ptr, serial, surf->resource,
                                                          wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                }
                                wl_pointer_send_motion(ptr, timeMs, wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                wl_pointer_send_frame(ptr);
                            }
                        }
                    } else {
                        if (mImpl->currentPointerSurface) {
                            for (wl_resource* oldPtr : mImpl->pointerResources) {
                                if (wl_resource_get_client(oldPtr) == wl_resource_get_client(mImpl->currentPointerSurface)) {
                                    wl_pointer_send_leave(oldPtr, serial, mImpl->currentPointerSurface);
                                    wl_pointer_send_frame(oldPtr);
                                }
                            }
                            mImpl->currentPointerSurface = nullptr;
                        }
                    }
                    }
                }
            }
        }
    }
    return 0;
}

} // namespace andwayland
