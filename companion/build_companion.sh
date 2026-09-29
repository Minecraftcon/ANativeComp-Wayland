#!/bin/bash
set -e

SDK_ROOT="/home/shado/envs/android-sdk"
BUILD_TOOLS="$SDK_ROOT/build-tools/34.0.0"
ANDROID_JAR="$SDK_ROOT/platforms/android-34/android.jar"
D8="$SDK_ROOT/cmdline-tools/latest/bin/d8"

COMPANION_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$COMPANION_DIR/build"
SRC_DIR="$COMPANION_DIR/src"
RES_DIR="$COMPANION_DIR/res"
MANIFEST="$COMPANION_DIR/AndroidManifest.xml"
GEN_DIR="$BUILD_DIR/gen"
OBJ_DIR="$BUILD_DIR/obj"
OUT_APK="$BUILD_DIR/ANativeDrawer.apk"

echo "=== Building ANativeDrawer Companion App ==="

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$GEN_DIR" "$OBJ_DIR"

# 1. Compile Resources with AAPT2
echo "[1/5] Compiling resources with aapt2..."
"$BUILD_TOOLS/aapt2" compile --dir "$RES_DIR" -o "$BUILD_DIR/resources.zip"

# 2. Link APK with AAPT2 and generate R.java
echo "[2/5] Linking resources and generating R.java..."
"$BUILD_TOOLS/aapt2" link "$BUILD_DIR/resources.zip" \
    -I "$ANDROID_JAR" \
    --manifest "$MANIFEST" \
    --java "$GEN_DIR" \
    -o "$BUILD_DIR/unaligned.apk" \
    --auto-add-overlay

# 3. Compile Java Source Code
echo "[3/5] Compiling Java source files with javac..."
find "$SRC_DIR" "$GEN_DIR" -name "*.java" > "$BUILD_DIR/sources.txt"
javac -encoding UTF-8 \
    -cp "$ANDROID_JAR" \
    -d "$OBJ_DIR" \
    @"$BUILD_DIR/sources.txt"

# 4. Convert Class Files to DEX with D8
echo "[4/5] Converting classes to DEX with d8..."
find "$OBJ_DIR" -name "*.class" > "$BUILD_DIR/classes.txt"
"$D8" --min-api 26 \
    --lib "$ANDROID_JAR" \
    --output "$BUILD_DIR" \
    @"$BUILD_DIR/classes.txt"

# Add classes.dex to unaligned APK
(cd "$BUILD_DIR" && "$BUILD_TOOLS/aapt" add unaligned.apk classes.dex)

# 5. ZipAlign and Sign APK
echo "[5/5] Aligning and signing APK..."
"$BUILD_TOOLS/zipalign" -f -p 4 "$BUILD_DIR/unaligned.apk" "$BUILD_DIR/aligned.apk"

# Generate debug keystore if not exists
KEYSTORE="$COMPANION_DIR/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkey -v -keystore "$KEYSTORE" -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi

"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-pass pass:android \
    --ks-key-alias androiddebugkey \
    --key-pass pass:android \
    --out "$OUT_APK" \
    "$BUILD_DIR/aligned.apk"

echo "=== SUCCESS! Built: $OUT_APK ==="
ls -lh "$OUT_APK"
