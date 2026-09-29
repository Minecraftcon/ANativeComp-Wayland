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

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <android/log.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
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
    bool isPointer = false;
    struct input_absinfo absX{};
    struct input_absinfo absY{};
    struct wl_event_source* source = nullptr;
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

    int keymapFd = -1;
    size_t keymapSize = 0;

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

static void pointer_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {}
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
}

static void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct wl_seat_interface seat_iface = {
    .get_pointer  = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch    = seat_get_touch,
    .release      = seat_release,
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

    DIR* dir = opendir("/dev/input");
    if (!dir) {
        ALOGE("Failed to open /dev/input: %s", strerror(errno));
        return false;
    }

    struct dirent* ent;
    int count = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        std::string devPath = std::string("/dev/input/") + ent->d_name;
        int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            ALOGW("Cannot open %s: %s", devPath.c_str(), strerror(errno));
            continue;
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
            if ((keyBits[KEY_A / 8] & (1 << (KEY_A % 8))) ||
                (keyBits[KEY_ENTER / 8] & (1 << (KEY_ENTER % 8))) ||
                (keyBits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8))) ||
                (keyBits[KEY_POWER / 8] & (1 << (KEY_POWER % 8)))) {
                dev.isKeyboard = true;
            }
        }

        dev.source = wl_event_loop_add_fd(
            mImpl->eventLoop, fd, WL_EVENT_READABLE,
            [](int fd, uint32_t mask, void* data) -> int {
                return static_cast<SeatManager*>(data)->handleEvdevEvent(fd, mask);
            },
            this);

        ALOGI("Enlisted input device %s (%s): touch=%d, kbd=%d, ptr=%d, X=[%d..%d], Y=[%d..%d]",
              devPath.c_str(), name, dev.isTouch, dev.isKeyboard, dev.isPointer,
              dev.absX.minimum, dev.absX.maximum, dev.absY.minimum, dev.absY.maximum);

        mImpl->devices.push_back(std::move(dev));
        count++;
    }
    closedir(dir);

    ALOGI("SeatManager started with %d active input devices", count);
    return count > 0;
}

void SeatManager::run() {
    mRunning = true;
    while (mRunning) {
        usleep(100000);
    }
}

void SeatManager::stop() {
    mRunning = false;
    for (auto& dev : mImpl->devices) {
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
}

int SeatManager::getKeymapFd() const {
    return mImpl->keymapFd;
}

uint32_t SeatManager::getKeymapSize() const {
    return static_cast<uint32_t>(mImpl->keymapSize);
}

void SeatManager::notifySurfaceDestroyed(wl_resource* surfaceResource) {
    if (mImpl->currentKeyboardSurface == surfaceResource) mImpl->currentKeyboardSurface = nullptr;
    if (mImpl->currentPointerSurface == surfaceResource) mImpl->currentPointerSurface = nullptr;

    for (int s = 0; s < Impl::MAX_TOUCH_SLOTS; ++s) {
        if (mImpl->touchSlots[s].targetSurfaceResource == surfaceResource) {
            mImpl->touchSlots[s].targetSurface = nullptr;
            mImpl->touchSlots[s].targetSurfaceResource = nullptr;
            mImpl->touchSlots[s].down = false;
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
                } else if (ev.code == ABS_X) {
                    mImpl->pointerScreenX = scaleCoord(ev.value, dev->absX.minimum, dev->absX.maximum, dispW);
                    mImpl->pointerModified = true;
                } else if (ev.code == ABS_Y) {
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

                        if (surf && surf->resource) {
                            setKeyboardFocus(surf->resource);

                            struct wl_client* targetClient = wl_resource_get_client(surf->resource);

                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    wl_touch_send_down(touch, serial, timeMs, surf->resource, s,
                                                       wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                    wl_touch_send_frame(touch);
                                }
                            }

                            // Pointer emulation for slot 0
                            if (s == 0) {
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
                                        wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                                        wl_pointer_send_frame(ptr);
                                    }
                                }
                            }
                            ALOGI("Touch DOWN on surface %u at screen (%d, %d) -> local (%d, %d)",
                                  surf->id, slot.screenX, slot.screenY, localX, localY);
                        } else {
                            ALOGI("Touch DOWN at screen (%d, %d) outside any window", slot.screenX, slot.screenY);
                        }
                    }
                    // Case 2: Touch MOTION
                    else if (slot.trackingId >= 0 && slot.down) {
                        if (slot.targetSurface && slot.targetSurfaceResource) {
                            WaylandSurface* surf = slot.targetSurface;
                            int32_t localX = surf ? (slot.screenX - surf->committed.x) : slot.localX;
                            int32_t localY = surf ? (slot.screenY - surf->committed.y) : slot.localY;
                            slot.localX = localX;
                            slot.localY = localY;

                            struct wl_client* targetClient = wl_resource_get_client(slot.targetSurfaceResource);

                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    wl_touch_send_motion(touch, timeMs, s,
                                                         wl_fixed_from_int(localX), wl_fixed_from_int(localY));
                                    wl_touch_send_frame(touch);
                                }
                            }

                            if (s == 0) {
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
                    // Case 3: Touch UP
                    else if (slot.trackingId == -1 && slot.down) {
                        slot.down = false;
                        if (slot.targetSurfaceResource) {
                            struct wl_client* targetClient = wl_resource_get_client(slot.targetSurfaceResource);

                            for (wl_resource* touch : mImpl->touchResources) {
                                if (wl_resource_get_client(touch) == targetClient) {
                                    wl_touch_send_up(touch, serial, timeMs, s);
                                    wl_touch_send_frame(touch);
                                }
                            }

                            if (s == 0) {
                                for (wl_resource* ptr : mImpl->pointerResources) {
                                    if (wl_resource_get_client(ptr) == targetClient) {
                                        wl_pointer_send_button(ptr, serial, timeMs, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                                        wl_pointer_send_frame(ptr);
                                    }
                                }
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
    return 0;
}

} // namespace andwayland
