#pragma once
/**
 * SeatManager.h
 *
 * Reads Android input events from /dev/input/eventX (evdev) and
 * forwards them as Wayland wl_seat / wl_keyboard / wl_pointer / wl_touch events
 * to the focused Wayland client.
 */

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <atomic>

struct wl_client;
struct wl_resource;
struct wl_display;
struct wl_event_loop;

namespace andwayland {

class SurfaceBridge;
struct WaylandSurface;

class SeatManager {
public:
    SeatManager();
    ~SeatManager();

    /** Legacy / stub init */
    bool init();

    /** Full initialization: attach to Wayland event loop, bridge, and enumerate /dev/input */
    bool start(wl_display* display, wl_event_loop* loop, std::shared_ptr<SurfaceBridge> bridge);

    /** Run the input reading loop (blocks — call from a thread if desired). */
    void run();

    /** Stop input handling and close file descriptors. Thread-safe. */
    void stop();

    /** Called by WaylandServer when a client binds wl_seat. */
    void bindSeat(wl_client* client, uint32_t version, uint32_t id);

    /** Set the currently focused surface for keyboard events. */
    void setKeyboardFocus(wl_resource* surfaceResource);

    /** Set the currently hovered surface for pointer events. */
    void setPointerFocus(wl_resource* surfaceResource);

    /** Notify SeatManager that a surface is being destroyed. */
    void notifySurfaceDestroyed(wl_resource* surfaceResource);

    /** Handle readable event on an evdev fd */
    int handleEvdevEvent(int fd, uint32_t mask);

    /** Handle inotify events for device hotplugging */
    int handleInotifyEvent(int fd, uint32_t mask);

    /** Enlist/remove an evdev device by filesystem path */
    void enlistDevice(const std::string& devPath);
    void removeDevice(const std::string& devPath);

    /** Update EVIOCGRAB on all hardware typing keyboards based on focus */
    void updateKeyboardGrabs(bool grab);

    /** Resource tracking helpers */
    void addPointerResource(wl_resource* resource);
    void addKeyboardResource(wl_resource* resource);
    void addTouchResource(wl_resource* resource);
    void removeResource(wl_resource* resource);

    /** Accessors for keymap */
    int getKeymapFd() const;
    uint32_t getKeymapSize() const;

    /** Accessors for last touch coordinates */
    int32_t getLastTouchScreenX() const;
    int32_t getLastTouchScreenY() const;

    std::shared_ptr<SurfaceBridge> getBridge() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    std::atomic<bool>     mRunning{false};
};

} // namespace andwayland
