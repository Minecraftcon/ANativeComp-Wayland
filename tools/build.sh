#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# tools/build.sh — Cross-compile ANativeDrawer for Android aarch64
#
# Usage:
#   ./tools/build.sh [clean] [push] [run]
#
#   clean  — wipe the build directory first
#   push   — adb push binary to /data/local/tmp/andwayland after build
#   run    — push + run the binary as root via adb shell
#
# Requirements:
#   - Android NDK r25+ in $NDK or $ANDROID_NDK_HOME
#   - CMake 3.22+
#   - wayland-scanner (host) in PATH
#   - libwayland-server cross-compiled into third_party/wayland/
#     (run ./tools/build_wayland.sh first)
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
BINARY="$BUILD_DIR/andwayland"

# ── Find NDK ─────────────────────────────────────────────────────────────────
NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-""}}}"
if [[ -z "$NDK" ]]; then
    # Try common locations
    for dir in \
        "$HOME/Android/Sdk/ndk/26.3.11579264" \
        "$HOME/Android/Sdk/ndk/25.2.9519653" \
        "/opt/android-ndk" \
        "/usr/local/android-ndk"; do
        if [[ -d "$dir" ]]; then
            NDK="$dir"
            break
        fi
    done
fi

if [[ -z "$NDK" || ! -d "$NDK" ]]; then
    echo "ERROR: Android NDK not found."
    echo "  Set NDK=/path/to/ndk or ANDROID_NDK_HOME=/path/to/ndk"
    exit 1
fi
echo "Using NDK: $NDK"

# ── Parse arguments ───────────────────────────────────────────────────────────
DO_CLEAN=0
DO_PUSH=0
DO_RUN=0
BUILD_TYPE="Release"

for arg in "$@"; do
    case "$arg" in
        clean)  DO_CLEAN=1 ;;
        push)   DO_PUSH=1  ;;
        run)    DO_RUN=1; DO_PUSH=1 ;;
        debug)  BUILD_TYPE="Debug" ;;
    esac
done

# ── Sanity checks ─────────────────────────────────────────────────────────────
if ! command -v wayland-scanner &>/dev/null; then
    echo "ERROR: wayland-scanner not found. Install with:"
    echo "  sudo apt install wayland-scanner  # Debian/Ubuntu"
    echo "  sudo pacman -S wayland           # Arch"
    exit 1
fi

if [[ ! -f "$PROJECT_DIR/third_party/wayland/lib/libwayland-server.a" ]] && \
   [[ ! -f "$PROJECT_DIR/third_party/wayland/lib/libwayland-server.so" ]]; then
    echo "WARNING: libwayland-server not found in third_party/wayland/lib/"
    echo "  Run: ./tools/build_wayland.sh"
    echo "  Continuing anyway (CMake will error if it can't find it)"
fi

# ── Clean ─────────────────────────────────────────────────────────────────────
if [[ $DO_CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# ── Configure ─────────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"
cmake \
    -S "$PROJECT_DIR" \
    -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-33 \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DENABLE_XWAYLAND=OFF \
    -DENABLE_DMABUF=ON \
    -DENABLE_LAYER_SHELL=OFF

# ── Build ─────────────────────────────────────────────────────────────────────
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: Build succeeded but binary not found at $BINARY"
    exit 1
fi

echo ""
echo "✓ Build complete: $BINARY"
ls -lh "$BINARY"

# ── Push ─────────────────────────────────────────────────────────────────────
if [[ $DO_PUSH -eq 1 ]]; then
    echo ""
    echo "Pushing to device..."
    adb push "$BINARY" /data/local/tmp/andwayland
    adb shell chmod +x /data/local/tmp/andwayland

    if [[ -f "$PROJECT_DIR/third_party/wayland/lib/libwayland-server.so" ]]; then
        adb push "$PROJECT_DIR/third_party/wayland/lib/libwayland-server.so" /data/local/tmp/libwayland-server.so
    fi

    LIBCXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
    if [[ -f "$LIBCXX" ]]; then
        adb push "$LIBCXX" /data/local/tmp/libc++_shared.so
    fi

    echo "✓ Pushed binary and shared libs to /data/local/tmp/"
fi

# ── Run ──────────────────────────────────────────────────────────────────────
if [[ $DO_RUN -eq 1 ]]; then
    echo ""
    echo "Running on device as root..."
    echo "  WAYLAND_DISPLAY=wayland-0"
    echo "  XDG_RUNTIME_DIR=/data/wayland"
    echo "  Press Ctrl+C to stop."
    echo ""
    adb shell "su -c '
        mkdir -p /data/wayland
        chmod 777 /data/wayland
        XDG_RUNTIME_DIR=/data/wayland \
        WAYLAND_DEBUG=1 \
        LD_LIBRARY_PATH=/data/local/tmp:/system/lib64 \
        /data/local/tmp/andwayland --socket wayland-0
    '"
fi
