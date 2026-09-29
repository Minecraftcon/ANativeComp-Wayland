/**
 * ExtensionRegistry.cpp — M1 stub
 */

#include "ExtensionRegistry.h"
#include <android/log.h>

#define LOG_TAG "andwayland:Ext"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace andwayland {

struct ExtensionRegistry::Impl {};

ExtensionRegistry::ExtensionRegistry() : mImpl(std::make_unique<Impl>()) {}
ExtensionRegistry::~ExtensionRegistry() { stop(); }

bool ExtensionRegistry::init(const char* socketPath) {
    ALOGI("ExtensionRegistry stub (socket: %s) — implemented in later milestone", socketPath);
    return true;
}

void ExtensionRegistry::stop() {}

} // namespace andwayland
