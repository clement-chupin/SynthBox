# GrvEP Android Native

Android APK built from the **C++ SDL2 simulator** (not the web/WASM version).

## Architecture

- `libSDL2.so` — SDL2 cross-compiled for Android arm64 (from `SDL2/` source)
- `libmain.so` — full GrvEP simulator: SDL2 window + AMY audio + U8g2 rendering
- `SDLActivity.java` — SDL2 Java activity wrapper (bundled from SDL2 template)

The SDL2 logical resolution is fixed at 820×560 and automatically scaled to the device screen. Touch events are mapped to mouse events by SDL2, so the keyboard tap and joystick drag work on touchscreen.

## First-time build

```bash
cd android-native
./setup_android.sh
```

This script:
1. Downloads JDK 21 to `~/jdk21/` if absent
2. Installs Android NDK 26.1 via sdkmanager (SDK at `~/android-sdk/`)
3. Clones SDL2 2.30.x to `android-native/SDL2/` if absent
4. Compiles the APK: `app/build/outputs/apk/debug/app-debug.apk`

## Install on device

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or copy the APK to the phone, enable "Install from unknown sources" in Settings, and open the file.

## Subsequent builds

```bash
cd android-native
export JAVA_HOME=~/jdk21/jdk-21.0.5+11
export ANDROID_HOME=~/android-sdk
./gradlew assembleDebug --no-daemon
```

## Controls on touchscreen

| Gesture | Action |
|---------|--------|
| Tap on key grid | Play note |
| Tap + hold joystick area | Move joystick |
| Tap on pot area | Set pot value |
| No physical keyboard needed | — |
