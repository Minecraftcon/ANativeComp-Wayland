#!/bin/bash
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGING_DIR="$REPO_ROOT/packaging"
MAGISK_DIR="$PACKAGING_DIR/magisk"
OUT_ZIP="$PACKAGING_DIR/andwayland-magisk.zip"

echo "=== Packaging ANativeDrawer Magisk Module ==="

# 1. Ensure companion APK is built
if [ ! -f "$REPO_ROOT/companion/build/ANativeDrawer.apk" ]; then
    echo "Building companion APK first..."
    "$REPO_ROOT/companion/build_companion.sh"
fi

# 2. Setup module overlay tree
mkdir -p "$MAGISK_DIR/system/bin"
mkdir -p "$MAGISK_DIR/system/lib64"
mkdir -p "$MAGISK_DIR/system/priv-app/ANativeDrawer"

echo "Copying binaries and libraries..."
cp -f "$REPO_ROOT/build/andwayland" "$MAGISK_DIR/system/bin/andwayland"
chmod 755 "$MAGISK_DIR/system/bin/andwayland"

if [ -f "$REPO_ROOT/third_party/wayland/lib/libwayland-server.so" ]; then
    cp -f "$REPO_ROOT/third_party/wayland/lib/libwayland-server.so" "$MAGISK_DIR/system/lib64/libwayland-server.so"
    chmod 644 "$MAGISK_DIR/system/lib64/libwayland-server.so"
fi

echo "Copying companion APK to system/priv-app/..."
cp -f "$REPO_ROOT/companion/build/ANativeDrawer.apk" "$MAGISK_DIR/system/priv-app/ANativeDrawer/ANativeDrawer.apk"
chmod 644 "$MAGISK_DIR/system/priv-app/ANativeDrawer/ANativeDrawer.apk"

chmod 755 "$MAGISK_DIR/service.sh"

# 3. Create zip package
rm -f "$OUT_ZIP"
echo "Creating flashable Magisk zip..."
(cd "$MAGISK_DIR" && zip -r -9 "$OUT_ZIP" module.prop service.sh system)

echo "=== Magisk Module Packaged: $OUT_ZIP ==="
unzip -l "$OUT_ZIP"
