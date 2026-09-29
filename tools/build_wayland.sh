#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# tools/build_wayland.sh
#
# Cross-compile libwayland-server for Android aarch64.
# Outputs to: third_party/wayland/{include,lib}/
#
# Requirements:
#   - meson, ninja
#   - Android NDK (set NDK or ANDROID_NDK_HOME)
#   - wayland source (auto-downloaded if not present)
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
WAYLAND_VERSION="1.23.1"
WAYLAND_SRC="$PROJECT_DIR/third_party/wayland-src"
WAYLAND_OUT="$PROJECT_DIR/third_party/wayland"
NDK="${NDK:-${ANDROID_NDK_HOME:-""}}"

if [[ -z "$NDK" ]]; then
    echo "ERROR: Set NDK=/path/to/ndk"
    exit 1
fi

# ── Download wayland source ───────────────────────────────────────────────────
if [[ ! -d "$WAYLAND_SRC" ]]; then
    echo "Downloading wayland $WAYLAND_VERSION..."
    mkdir -p "$PROJECT_DIR/third_party"
    curl -L "https://gitlab.freedesktop.org/wayland/wayland/-/releases/$WAYLAND_VERSION/downloads/wayland-$WAYLAND_VERSION.tar.xz" \
        -o /tmp/wayland.tar.xz
    tar -xf /tmp/wayland.tar.xz -C "$PROJECT_DIR/third_party"
    mv "$PROJECT_DIR/third_party/wayland-$WAYLAND_VERSION" "$WAYLAND_SRC"
fi

# ── Set up Android cross-compilation toolchain for meson ─────────────────────
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang"
CLANGXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang++"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

if [[ ! -f "$CLANG" ]]; then
    echo "ERROR: clang not found at $CLANG"
    echo "Check your NDK path and Android API version (android33)"
    exit 1
fi

# ── Write meson cross file ────────────────────────────────────────────────────
CROSS_FILE="/tmp/android-aarch64.ini"
cat > "$CROSS_FILE" <<EOF
[binaries]
c       = '$CLANG'
cpp     = '$CLANGXX'
ar      = '$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
strip   = '$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
pkgconfig = 'pkg-config'

[host_machine]
system     = 'android'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'

[properties]
sys_root = '$SYSROOT'
EOF

# ── Build wayland (server only, no scanner, no client, no docs) ───────────────
BUILD_DIR="$WAYLAND_SRC/build-android"
rm -rf "$BUILD_DIR"

meson setup "$BUILD_DIR" "$WAYLAND_SRC" \
    --cross-file "$CROSS_FILE" \
    --prefix "$WAYLAND_OUT" \
    --buildtype release \
    -Dscanner=false \
    -Dtests=false \
    -Ddocumentation=false \
    -Ddtd_validation=false \
    -Dlibraries=true

ninja -C "$BUILD_DIR"
ninja -C "$BUILD_DIR" install

echo ""
echo "✓ libwayland-server built and installed to: $WAYLAND_OUT"
ls -lh "$WAYLAND_OUT/lib/"libwayland*.* 2>/dev/null || true
