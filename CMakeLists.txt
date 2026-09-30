cmake_minimum_required(VERSION 3.22)
project(andwayland VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ────────────────────────────────────────────────────────────────────────────
# Options
# ────────────────────────────────────────────────────────────────────────────
option(ENABLE_XWAYLAND   "Enable XWayland support"           ON)
option(ENABLE_DMABUF     "Enable zwp_linux_dmabuf_v1"        ON)
set(ENABLE_LAYER_SHELL ON CACHE BOOL "Enable zwlr_layer_shell_v1" FORCE)
option(ENABLE_ASAN       "Enable AddressSanitizer (debug)"   OFF)

# ────────────────────────────────────────────────────────────────────────────
# Android platform checks
# ────────────────────────────────────────────────────────────────────────────
if(NOT ANDROID)
    message(FATAL_ERROR "This project must be cross-compiled for Android. "
        "Use -DCMAKE_TOOLCHAIN_FILE=\$NDK/build/cmake/android.toolchain.cmake")
endif()

message(STATUS "Android ABI:      ${ANDROID_ABI}")
message(STATUS "Android Platform: ${ANDROID_PLATFORM}")

# ────────────────────────────────────────────────────────────────────────────
# Find wayland-server (must be pre-built for Android)
# We look in <project>/third_party/wayland/lib first, then system paths.
# ────────────────────────────────────────────────────────────────────────────
set(WAYLAND_PREBUILT_DIR "${CMAKE_SOURCE_DIR}/third_party/wayland")

find_library(WAYLAND_SERVER_LIB wayland-server
    PATHS "${WAYLAND_PREBUILT_DIR}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)

if(NOT WAYLAND_SERVER_LIB)
    message(FATAL_ERROR
        "libwayland-server not found. Run:\n"
        "  ./tools/build_wayland.sh\n"
        "to cross-compile libwayland-server for Android aarch64.")
endif()

find_path(WAYLAND_SERVER_INCLUDE wayland-server.h
    PATHS "${WAYLAND_PREBUILT_DIR}/include"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)

# ────────────────────────────────────────────────────────────────────────────
# Android platform libraries (loaded from /system/lib64 at runtime)
# We stub-link against them so the linker is happy; they're always present
# on a rooted device in /system/lib64.
# ────────────────────────────────────────────────────────────────────────────
set(ANDROID_SYSROOT_LIBS
    # Standard NDK-public libraries (always present)
    android     # ANativeWindow, AHardwareBuffer
    EGL
    GLESv2
    log
)

# ────────────────────────────────────────────────────────────────────────────
# Wayland-scanner: generate C bindings from .xml protocol files
# ────────────────────────────────────────────────────────────────────────────
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

function(wayland_add_protocol TARGET XML_FILE GENERATED_PREFIX)
    set(OUT_H   "${CMAKE_SOURCE_DIR}/protocols/generated/${GENERATED_PREFIX}.h")
    set(OUT_SRC "${CMAKE_SOURCE_DIR}/protocols/generated/${GENERATED_PREFIX}.c")
    add_custom_command(
        OUTPUT "${OUT_H}" "${OUT_SRC}"
        COMMAND ${WAYLAND_SCANNER} server-header "${XML_FILE}" "${OUT_H}"
        COMMAND ${WAYLAND_SCANNER} private-code  "${XML_FILE}" "${OUT_SRC}"
        DEPENDS "${XML_FILE}"
        COMMENT "Generating Wayland protocol bindings: ${GENERATED_PREFIX}"
    )
    target_sources(${TARGET} PRIVATE "${OUT_SRC}")
endfunction()

# ────────────────────────────────────────────────────────────────────────────
# Source files
# ────────────────────────────────────────────────────────────────────────────
set(COMPOSITOR_SOURCES
    compositor/main.cpp
    compositor/WaylandServer.cpp
    compositor/SurfaceBridge.cpp
    compositor/SeatManager.cpp
    compositor/ExtensionRegistry.cpp

    platform/SurfaceFlingerBridge.cpp
    platform/DisplayManager.cpp
    platform/AHardwareBufferHelper.cpp
)

# ────────────────────────────────────────────────────────────────────────────
# Main binary
# ────────────────────────────────────────────────────────────────────────────
add_executable(andwayland ${COMPOSITOR_SOURCES})

target_include_directories(andwayland PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/compositor"
    "${CMAKE_SOURCE_DIR}/platform"
    "${CMAKE_SOURCE_DIR}/protocols/generated"
    "${WAYLAND_SERVER_INCLUDE}"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/gui/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/ui/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/binder/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/nativewindow/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/nativebase/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/arect/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_frameworks_native/libs/math/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_core/libutils/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_core/libcutils/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_core/libsystem/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_libbase/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_logging/liblog/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_hardware_libhardware/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_libhidl/transport/token/1.0/utils/include"
    "${CMAKE_SOURCE_DIR}/third_party/aosp_system_libhidl/base/include"
)

target_compile_definitions(andwayland PRIVATE
    ANDWAYLAND_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${ENABLE_XWAYLAND}>:ENABLE_XWAYLAND>
    $<$<BOOL:${ENABLE_DMABUF}>:ENABLE_DMABUF>
    $<$<BOOL:${ENABLE_LAYER_SHELL}>:ENABLE_LAYER_SHELL>
)

target_compile_options(andwayland PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Wno-invalid-specialization
    -Wno-deprecated-literal-operator
    -fno-rtti
    -fvisibility=hidden
    $<$<BOOL:${ENABLE_ASAN}>:-fsanitize=address>
)

target_link_directories(andwayland PRIVATE
    "${CMAKE_SOURCE_DIR}/device-sysroot/lib64"
)

# Link against wayland-server, libffi, and Android system libs
target_link_libraries(andwayland PRIVATE
    "${WAYLAND_SERVER_LIB}"
    "${CMAKE_SOURCE_DIR}/third_party/libffi/lib/libffi.a"
    ${ANDROID_SYSROOT_LIBS}
    gui
    ui
    binder
    utils
    cutils
    -Wl,--allow-shlib-undefined
)

target_link_options(andwayland PRIVATE
    "-Wl,-rpath,/data/adb/modules/andwayland/system/lib64"
    "-Wl,-rpath,/data/data/com.termux/files/usr/lib"
    "-Wl,-rpath,/system/lib64"
)

if(ENABLE_ASAN)
    target_link_options(andwayland PRIVATE -fsanitize=address)
endif()

# Strip the binary in Release mode
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET andwayland POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:andwayland>
        COMMENT "Stripping binary")
endif()

# ────────────────────────────────────────────────────────────────────────────
# Installation
# ────────────────────────────────────────────────────────────────────────────
install(TARGETS andwayland RUNTIME DESTINATION bin)

# ────────────────────────────────────────────────────────────────────────────
# Wayland protocol generation targets
# (run after add_executable so wayland_add_protocol can call target_sources)
# ────────────────────────────────────────────────────────────────────────────
set(XDG_SHELL_XML "${CMAKE_SOURCE_DIR}/protocols/xdg_shell/xdg-shell.xml")
set(VIEWPORTER_XML "${CMAKE_SOURCE_DIR}/protocols/viewporter/viewporter.xml")
set(LINUX_DMABUF_XML "${CMAKE_SOURCE_DIR}/protocols/linux_dmabuf/linux-dmabuf-v1.xml")
set(LAYER_SHELL_XML "${CMAKE_SOURCE_DIR}/protocols/layer_shell/wlr-layer-shell-unstable-v1.xml")
set(VIRTUAL_KEYBOARD_XML "${CMAKE_SOURCE_DIR}/protocols/virtual_keyboard/virtual-keyboard-unstable-v1.xml")
set(XDG_DECORATION_XML "${CMAKE_SOURCE_DIR}/protocols/xdg_decoration/xdg-decoration-unstable-v1.xml")
set(XWAYLAND_SHELL_XML "${CMAKE_SOURCE_DIR}/protocols/xwayland_shell/xwayland-shell-v1.xml")

if(EXISTS "${XDG_SHELL_XML}")
    wayland_add_protocol(andwayland "${XDG_SHELL_XML}" xdg-shell-protocol)
endif()
if(EXISTS "${VIEWPORTER_XML}")
    wayland_add_protocol(andwayland "${VIEWPORTER_XML}" viewporter-protocol)
endif()
if(EXISTS "${XDG_DECORATION_XML}")
    wayland_add_protocol(andwayland "${XDG_DECORATION_XML}" xdg-decoration-protocol)
endif()
if(EXISTS "${XWAYLAND_SHELL_XML}")
    wayland_add_protocol(andwayland "${XWAYLAND_SHELL_XML}" xwayland-shell-protocol)
endif()
if(EXISTS "${VIRTUAL_KEYBOARD_XML}")
    wayland_add_protocol(andwayland "${VIRTUAL_KEYBOARD_XML}" virtual-keyboard-protocol)
endif()
if(ENABLE_DMABUF AND EXISTS "${LINUX_DMABUF_XML}")
    wayland_add_protocol(andwayland "${LINUX_DMABUF_XML}" linux-dmabuf-protocol)
endif()
if(ENABLE_LAYER_SHELL AND EXISTS "${LAYER_SHELL_XML}")
    wayland_add_protocol(andwayland "${LAYER_SHELL_XML}" layer-shell-protocol)
endif()
set(TEXT_INPUT_XML "${CMAKE_SOURCE_DIR}/protocols/text_input/text-input-unstable-v3.xml")
set(INPUT_METHOD_XML "${CMAKE_SOURCE_DIR}/protocols/input_method/input-method-unstable-v2.xml")
if(EXISTS "${TEXT_INPUT_XML}")
    wayland_add_protocol(andwayland "${TEXT_INPUT_XML}" text-input-protocol)
endif()
if(EXISTS "${INPUT_METHOD_XML}")
    wayland_add_protocol(andwayland "${INPUT_METHOD_XML}" input-method-protocol)
endif()

