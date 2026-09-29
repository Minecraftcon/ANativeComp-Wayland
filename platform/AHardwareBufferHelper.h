#pragma once
/**
 * AHardwareBufferHelper.h
 *
 * Utilities for wrapping DMA-BUF file descriptors into AHardwareBuffer
 * for zero-copy import into SurfaceFlinger.
 *
 * Android 12+ (API 31) has improved AHardwareBuffer_createFromHandle()
 * support. Older APIs require going through gralloc mapper HAL directly.
 */

#include <cstdint>
#include <android/hardware_buffer.h>

namespace andwayland {
namespace ahwb {

/**
 * Wrap a DMA-BUF file descriptor as an AHardwareBuffer.
 * Returns nullptr on failure.
 *
 * Caller must call AHardwareBuffer_release() on the result.
 */
AHardwareBuffer* importDmaBuf(int dmaBufFd,
                               uint32_t width,
                               uint32_t height,
                               uint32_t drmFormat,
                               uint64_t modifier);

/**
 * Map a DRM format code to an AHardwareBuffer format constant.
 * Returns AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM as a safe default.
 */
uint32_t drmFormatToAhwb(uint32_t drmFormat);

} // namespace ahwb
} // namespace andwayland
