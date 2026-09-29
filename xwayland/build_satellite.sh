#!/bin/bash
set -e

# Script to cross-compile xwayland-satellite for Android AArch64
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK_ROOT="/home/shado/envs/android-sdk/ndk/29.0.14206865"
LLVM_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
SYSROOT="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

cd "$REPO_ROOT/third_party/xwayland-satellite"

PKG_CONFIG_ALLOW_CROSS=1 \
PKG_CONFIG_SYSROOT_DIR="$REPO_ROOT/sysroot-termux" \
PKG_CONFIG_LIBDIR="$REPO_ROOT/sysroot-termux/data/data/com.termux/files/usr/lib/pkgconfig:$REPO_ROOT/sysroot-termux/data/data/com.termux/files/usr/share/pkgconfig" \
BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$SYSROOT -I$SYSROOT/usr/include/aarch64-linux-android" \
CC_aarch64_linux_android="$LLVM_BIN/aarch64-linux-android31-clang" \
CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$LLVM_BIN/aarch64-linux-android31-clang" \
RUSTFLAGS="-L$REPO_ROOT/sysroot-termux/data/data/com.termux/files/usr/lib" \
cargo build --release --target aarch64-linux-android

"$LLVM_BIN/llvm-strip" target/aarch64-linux-android/release/xwayland-satellite
cp target/aarch64-linux-android/release/xwayland-satellite "$REPO_ROOT/xwayland/prebuilt/xwayland-satellite"
echo "Build complete: $REPO_ROOT/xwayland/prebuilt/xwayland-satellite"
