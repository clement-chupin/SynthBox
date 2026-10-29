#!/bin/bash
# build_native_apk.sh — Compile l'APK natif C++/SDL2 (simulateur Android)
# Usage: ./build_native_apk.sh [--release] [--clean] [--install]
# Output: android-native/app/build/outputs/apk/debug/app-debug.apk

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NATIVE_DIR="$SCRIPT_DIR/android-native"

SDK_DIR="$HOME/android-sdk"
JDK_DIR="$HOME/jdk21"
JDK_VERSION="jdk-21.0.5+11"
NDK_VERSION="26.1.10909125"

RELEASE=0
CLEAN=0
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        --release) RELEASE=1 ;;
        --clean)   CLEAN=1 ;;
        --install) INSTALL=1 ;;
    esac
done

echo "=== GrvEP Native APK Build ==="

# ── Prérequis: JDK ────────────────────────────────────────────────────────────
if [ ! -f "$JDK_DIR/$JDK_VERSION/bin/java" ]; then
    echo "[setup] JDK 21 introuvable dans $JDK_DIR/$JDK_VERSION"
    echo "        Lance android-native/setup_android.sh pour l'installer."
    exit 1
fi
export JAVA_HOME="$JDK_DIR/$JDK_VERSION"
export PATH="$JAVA_HOME/bin:$PATH"

# ── Prérequis: Android SDK + NDK ─────────────────────────────────────────────
if [ ! -d "$SDK_DIR/ndk/$NDK_VERSION" ]; then
    echo "[setup] NDK $NDK_VERSION introuvable dans $SDK_DIR/ndk/"
    echo "        Lance android-native/setup_android.sh pour l'installer."
    exit 1
fi
export ANDROID_HOME="$SDK_DIR"
export PATH="$SDK_DIR/cmdline-tools/latest/bin:$SDK_DIR/platform-tools:$PATH"

# ── local.properties (régénéré à chaque fois pour rester à jour) ─────────────
cat > "$NATIVE_DIR/local.properties" << EOF
sdk.dir=$SDK_DIR
ndk.dir=$SDK_DIR/ndk/$NDK_VERSION
EOF

cd "$NATIVE_DIR"

# ── Clean optionnel ───────────────────────────────────────────────────────────
if [ "$CLEAN" -eq 1 ]; then
    echo "[clean] Nettoyage du build précédent..."
    ./gradlew clean --no-daemon --quiet
fi

# ── Build ─────────────────────────────────────────────────────────────────────
if [ "$RELEASE" -eq 1 ]; then
    TASK="assembleRelease"
    VARIANT="release"
    APK="$NATIVE_DIR/app/build/outputs/apk/release/app-release-unsigned.apk"
else
    TASK="assembleDebug"
    VARIANT="debug"
    APK="$NATIVE_DIR/app/build/outputs/apk/debug/app-debug.apk"
fi

echo "[build] gradlew $TASK..."
./gradlew "$TASK" --no-daemon 2>&1 | tee /tmp/grvep_gradle.log
STATUS=${PIPESTATUS[0]}

if [ $STATUS -ne 0 ]; then
    echo ""
    echo "=== ERREUR DE BUILD (voir /tmp/grvep_gradle.log) ==="
    exit 1
fi

# ── Résultat ──────────────────────────────────────────────────────────────────
if [ -f "$APK" ]; then
    SIZE=$(du -h "$APK" | cut -f1)
    echo ""
    echo "=== APK prêt : $APK ($SIZE) ==="
    echo ""
    if [ "$INSTALL" -eq 1 ]; then
        if command -v adb &>/dev/null; then
            echo "[install] adb install -r ..."
            adb install -r "$APK"
            echo "[install] OK"
        else
            echo "[install] adb introuvable dans le PATH"
        fi
    else
        echo "Pour installer via USB :"
        echo "  adb install -r $APK"
        echo ""
        echo "Options disponibles :"
        echo "  --clean    nettoie avant de builder"
        echo "  --release  build release (non signé)"
        echo "  --install  installe directement via adb"
    fi
else
    echo "ERREUR: APK non trouvé à $APK"
    exit 1
fi
