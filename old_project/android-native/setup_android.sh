#!/bin/bash
# setup_android.sh — Build GrvEP native Android APK (C++ SDL2 simulator)
# Usage: ./setup_android.sh
# Output: app/build/outputs/apk/debug/app-debug.apk

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$HOME/android-sdk"
JDK_DIR="$HOME/jdk21"
JDK_VERSION="jdk-21.0.5+11"
NDK_VERSION="26.1.10909125"

echo "=== GrvEP Native Android APK Build ==="
cd "$SCRIPT_DIR"

# ── 0. JDK 21 ────────────────────────────────────────────────────────────────
if [ ! -f "$JDK_DIR/$JDK_VERSION/bin/java" ]; then
    echo "[0] Downloading Eclipse Temurin JDK 21..."
    mkdir -p "$JDK_DIR"
    curl -L -o "$JDK_DIR/jdk21.tar.gz" \
        "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.5%2B11/OpenJDK21U-jdk_x64_linux_hotspot_21.0.5_11.tar.gz" \
        --progress-bar
    tar -xzf "$JDK_DIR/jdk21.tar.gz" -C "$JDK_DIR"
    rm "$JDK_DIR/jdk21.tar.gz"
else
    echo "[0] JDK 21 already present"
fi
export JAVA_HOME="$JDK_DIR/$JDK_VERSION"
export PATH="$JAVA_HOME/bin:$PATH"

# ── 1. Android SDK ─────────────────────────────────────────────────────────────
export ANDROID_HOME="$SDK_DIR"
export PATH="$SDK_DIR/cmdline-tools/latest/bin:$SDK_DIR/platform-tools:$PATH"

if [ ! -d "$SDK_DIR/ndk/$NDK_VERSION" ]; then
    echo "[1] Installing Android NDK $NDK_VERSION..."
    yes | sdkmanager --licenses > /dev/null 2>&1 || true
    sdkmanager "ndk;$NDK_VERSION" "platforms;android-33" "build-tools;33.0.2"
else
    echo "[1] NDK already installed"
fi

# ── 2. SDL2 source ─────────────────────────────────────────────────────────────
if [ ! -d "$SCRIPT_DIR/SDL2/src" ]; then
    echo "[2] Cloning SDL2 (release-2.30.x)..."
    git clone --depth=1 --branch release-2.30.x \
        https://github.com/libsdl-org/SDL.git \
        "$SCRIPT_DIR/SDL2"
else
    echo "[2] SDL2 already present"
fi

# ── 3. Write local.properties for Gradle ──────────────────────────────────────
cat > "$SCRIPT_DIR/local.properties" << EOF
sdk.dir=$SDK_DIR
ndk.dir=$SDK_DIR/ndk/$NDK_VERSION
EOF
echo "[3] local.properties written"

# ── 4. Build APK ───────────────────────────────────────────────────────────────
echo "[4] Building APK (this may take 5-10 minutes on first build)..."
cd "$SCRIPT_DIR"
./gradlew assembleDebug --no-daemon

APK="$SCRIPT_DIR/app/build/outputs/apk/debug/app-debug.apk"
if [ -f "$APK" ]; then
    SIZE=$(du -h "$APK" | cut -f1)
    echo ""
    echo "=== APK ready: $APK ($SIZE) ==="
    echo ""
    echo "Install on Android phone via USB:"
    echo "  adb install -r $APK"
    echo ""
    echo "Or copy the APK to the phone and open it (enable 'Unknown sources')."
else
    echo "ERROR: APK not found."
    exit 1
fi
