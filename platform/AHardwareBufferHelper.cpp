/**
 * AHardwareBufferHelper.cpp
 * DMA-BUF import utilities — M5 full implementation.
 */

#include "AHardwareBufferHelper.h"
#include <drm/drm_fourcc.h>
#include <android/log.h>

#define LOG_TAG "andwayland:AHWB"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace andwayland {
namespace ahwb {

uint32_t drmFormatToAhwb(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ABGR8888: return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_XBGR8888: return AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
        case DRM_FORMAT_RGB888:   return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
        case DRM_FORMAT_RGB565:   return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
        default:
            ALOGE("Unknown DRM format 0x%x, defaulting to RGBA8888", drmFormat);
            return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    }
}

AHardwareBuffer* importDmaBuf(int /*dmaBufFd*/,
                               uint32_t /*width*/,
                               uint32_t /*height*/,
                               uint32_t /*drmFormat*/,
                               uint64_t /*modifier*/) {
    // M5: Implement via AHardwareBuffer_createFromHandle() or
    // gralloc4 mapper HAL importBuffer()
    ALOGE("importDmaBuf: not yet implemented (M5)");
    return nullptr;
}

} // namespace ahwb
} // namespace andwayland
