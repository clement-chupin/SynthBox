cmake_minimum_required(VERSION 3.18)
project(grvep_android CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ---- Paths (relative to this CMakeLists.txt directory) ----
# Structure: GrvEP/android-native/app/src/main/cpp/CMakeLists.txt
#   4 levels up (../../../../)           = GrvEP/android-native/
#   5 levels up (../../../../../)        = GrvEP/
get_filename_component(ANDROID_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
get_filename_component(GRVEP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

set(SDL2_DIR       "${ANDROID_NATIVE_ROOT}/SDL2")
set(SIM_DIR        "${GRVEP_ROOT}/simulator")
set(AMY_SRC        "${GRVEP_ROOT}/lib/AMY Synthesizer/src")
set(U8G2_SRC       "${GRVEP_ROOT}/lib/U8g2/src")
set(U8G2_CLIB      "${U8G2_SRC}/clib")

# ---- SDL2 ----
# SDL2's CMakeLists detects Android via the NDK toolchain and compiles accordingly
add_subdirectory("${SDL2_DIR}" SDL2_build)

# ---- AMY sources ----
set(AMY_SOURCES
    "${AMY_SRC}/algorithms.c"
    "${AMY_SRC}/amy.c"
    "${AMY_SRC}/amy_midi.c"
    "${AMY_SRC}/api.c"
    "${AMY_SRC}/custom.c"
    "${AMY_SRC}/delay.c"
    "${AMY_SRC}/envelope.c"
    "${AMY_SRC}/filters.c"
    "${AMY_SRC}/instrument.c"
    "${AMY_SRC}/interp_partials.c"
    "${AMY_SRC}/libminiaudio-audio.c"
    "${AMY_SRC}/log2_exp2.c"
    "${AMY_SRC}/midi_mappings.c"
    "${AMY_SRC}/oscillators.c"
    "${AMY_SRC}/parse.c"
    "${AMY_SRC}/patches.c"
    "${AMY_SRC}/pcm.c"
    "${AMY_SRC}/sequencer.c"
    "${AMY_SRC}/transfer.c"
    "${AMY_SRC}/cv_trigger.c"
)

# ---- U8g2 clib sources ----
file(GLOB U8G2_SOURCES "${U8G2_CLIB}/*.c")

# ---- GrvEP core sources ----
set(GRVEP_SOURCES
    "${GRVEP_ROOT}/src/main.cpp"
    "${GRVEP_ROOT}/src/audio_engine.cpp"
    "${GRVEP_ROOT}/src/NoteMap.cpp"
)

# ---- Simulator replacement sources ----
set(SIM_SOURCES
    "${SIM_DIR}/sim_main.cpp"
    "${SIM_DIR}/sim_keyboard.cpp"
    "${SIM_DIR}/sim_mux.cpp"
    "${SIM_DIR}/sim_oled.cpp"
    "${SIM_DIR}/sim_leds.cpp"
    "${SIM_DIR}/sim_midi.cpp"
    "${SIM_DIR}/sim_state.cpp"
    "${SIM_DIR}/sim_window.cpp"
    "${SIM_DIR}/sim_mp3dec.cpp"
)

# ---- libmain.so — loaded by SDLActivity ----
add_library(main SHARED
    ${GRVEP_SOURCES}
    ${SIM_SOURCES}
    ${AMY_SOURCES}
    ${U8G2_SOURCES}
)

# ---- Include paths ----
target_include_directories(main PRIVATE
    # Simulator HAL (shadows Arduino.h, U8g2lib.h, etc.)
    "${SIM_DIR}/hal"
    "${SIM_DIR}/hal/freertos"
    "${SIM_DIR}"
    # GrvEP project headers
    "${GRVEP_ROOT}/include"
    "${GRVEP_ROOT}/src"
    # AMY headers
    "${AMY_SRC}"
    # U8g2
    "${U8G2_CLIB}"
    "${U8G2_SRC}"
    # SDL2 headers (from the SDL2 build target)
    "$<TARGET_PROPERTY:SDL2,INCLUDE_DIRECTORIES>"
)

# ---- Compile definitions ----
target_compile_definitions(main PRIVATE
    SIMULATOR=1
    OOPSIE_LED_FLAG=1
    # Enables render_wavetable() and the 5 built-in wavetables baked into
    # pcm_samples_tiny.h (111/BRAIDS01/PPG_WA00/SINE2SAW/VIRAL) — used by the modular synth.
    AMY_WAVETABLE=1
    # Suppress PROGMEM / pgm_read_byte for Android (already defined in hal/pgmspace.h)
)

# ---- Compile options ----
target_compile_options(main PRIVATE
    -O2
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-deprecated-declarations
    -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
)

# ---- Link libraries ----
target_link_libraries(main PRIVATE
    SDL2        # SDL2 shared library (libSDL2.so)
    android     # Android NDK platform lib
    log         # Logcat (printf replacement on Android)
    OpenSLES    # Audio backend for miniaudio (older devices)
    # aaudio is included via miniaudio's detection on API 26+
    EGL
    GLESv1_CM
    GLESv2
    # pthread is part of Android libc — do not link explicitly
    m
    dl
    atomic
)
