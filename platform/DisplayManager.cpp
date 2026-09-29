/**
 * DisplayManager.cpp — M1 stub
 * Full implementation hooks into SurfaceFlinger display change callbacks.
 */

#include "DisplayManager.h"
#include "SurfaceFlingerBridge.h"

namespace andwayland {

DisplayManager& DisplayManager::get() {
    static DisplayManager instance;
    return instance;
}

bool DisplayManager::init() {
    auto& sf = SurfaceFlingerBridge::get();
    auto info = sf.getDisplayInfo();
    mWidth       = info.width;
    mHeight      = info.height;
    mOrientation = info.orientation;
    mRefreshRate = info.refreshRate;
    return true;
}

void DisplayManager::shutdown() {}

} // namespace andwayland
