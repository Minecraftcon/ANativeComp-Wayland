#pragma once
/**
 * DisplayManager.h
 *
 * Provides display geometry and handles rotation/orientation changes
 * from SurfaceFlinger, notifying the compositor and connected clients.
 */

#include <cstdint>
#include <functional>

namespace andwayland {

class DisplayManager {
public:
    using RotationCallback = std::function<void(int32_t newOrientation)>;

    static DisplayManager& get();

    bool init();
    void shutdown();

    int32_t width()       const { return mWidth;  }
    int32_t height()      const { return mHeight; }
    int32_t orientation() const { return mOrientation; }
    float   refreshRate() const { return mRefreshRate; }

    void setRotationCallback(RotationCallback cb) { mRotationCb = cb; }

private:
    DisplayManager() = default;

    int32_t         mWidth       = 0;
    int32_t         mHeight      = 0;
    int32_t         mOrientation = 0;
    float           mRefreshRate = 60.0f;
    RotationCallback mRotationCb;
};

} // namespace andwayland
