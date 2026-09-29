#pragma once
/**
 * ExtensionRegistry.h
 *
 * Manages the external draw API — allows other root processes to
 * request SurfaceFlinger layers through a secondary Unix socket IPC.
 *
 * Also manages compositor-level features:
 *   - Wallpaper layer
 *   - HUD/overlay injection
 *   - Screen capture API
 */

#include <memory>

namespace andwayland {

class ExtensionRegistry {
public:
    ExtensionRegistry();
    ~ExtensionRegistry();

    /** Start the extension API socket. */
    bool init(const char* socketPath = "/data/wayland/.andwayland-ext");

    /** Stop the extension server. */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace andwayland
