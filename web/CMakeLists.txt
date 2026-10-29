cmake_minimum_required(VERSION 3.16)
project(grvep_web CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ---- Paths ----
set(ROOT      ${CMAKE_SOURCE_DIR}/..)
set(AMY_SRC   ${ROOT}/lib/AMY\ Synthesizer/src)
set(U8G2_SRC  ${ROOT}/lib/U8g2/src)
set(U8G2_CLIB ${U8G2_SRC}/clib)

# ---- AMY sources (same set as simulator/CMakeLists.txt — exclude ESP32/Python-
# specific files). libminiaudio-audio.c provides the Emscripten web-audio backend
# (its own __EMSCRIPTEN__ branch) used here; web_amy_stubs.c (below) supplies the
# emscripten_lock_* symbols amy.c's __EMSCRIPTEN__ branch calls that don't exist in
# a single-threaded (no -pthread) build.
set(AMY_SOURCES
    ${AMY_SRC}/algorithms.c
    ${AMY_SRC}/amy.c
    ${AMY_SRC}/amy_midi.c
    ${AMY_SRC}/api.c
    ${AMY_SRC}/custom.c
    ${AMY_SRC}/delay.c
    ${AMY_SRC}/envelope.c
    ${AMY_SRC}/filters.c
    ${AMY_SRC}/instrument.c
    ${AMY_SRC}/interp_partials.c
    ${AMY_SRC}/libminiaudio-audio.c
    ${AMY_SRC}/log2_exp2.c
    ${AMY_SRC}/midi_mappings.c
    ${AMY_SRC}/oscillators.c
    ${AMY_SRC}/parse.c
    ${AMY_SRC}/patches.c
    ${AMY_SRC}/pcm.c
    ${AMY_SRC}/sequencer.c
    ${AMY_SRC}/transfer.c
    ${AMY_SRC}/cv_trigger.c
)

# ---- U8g2 clib sources ----
file(GLOB U8G2_SOURCES ${U8G2_CLIB}/*.c)

# ---- Vendored engine sources (SevenistSynth_RitualMachine's framework/engine, see
# ~/.claude/plans/mutable-nibbling-melody.md) — mirrors simulator/CMakeLists.txt's own
# ENGINE_SOURCES addition. Added alongside AMY_SOURCES, not replacing it (main.cpp still
# has raw AMY call sites until a later phase relocates them). The engine's own code has
# no Emscripten-specific requirement — it's plain portable C++17 DSP. ----
file(GLOB_RECURSE ENGINE_SOURCES
    ${ROOT}/engine/core/*.cpp
    ${ROOT}/engine/dsp/*.cpp
    ${ROOT}/engine/modules/*.cpp
    ${ROOT}/engine/mi/*.cpp
    ${ROOT}/engine/sampler/*.cpp
    ${ROOT}/engine/offline/*.cpp
)

# ---- GrvEP original sources (compiled with the web HAL) ----
set(GRVEP_SOURCES
    ${ROOT}/src/main.cpp
    ${ROOT}/src/audio_engine.cpp
    ${ROOT}/src/smp_import.cpp
    ${ROOT}/src/NoteMap.cpp
    # Replaced: keyboard.cpp, mux.cpp, oled.cpp, Leds.cpp, midi_usb.cpp, mp3dec.cpp
)

# ---- Web replacement sources ----
set(WEB_SOURCES
    ${CMAKE_SOURCE_DIR}/web_main.cpp
    ${CMAKE_SOURCE_DIR}/web_keyboard.cpp
    ${CMAKE_SOURCE_DIR}/web_mux.cpp
    ${CMAKE_SOURCE_DIR}/web_oled.cpp
    ${CMAKE_SOURCE_DIR}/web_leds.cpp
    ${CMAKE_SOURCE_DIR}/web_midi.cpp
    ${CMAKE_SOURCE_DIR}/web_mp3dec.cpp
    ${CMAKE_SOURCE_DIR}/web_state.cpp
    ${CMAKE_SOURCE_DIR}/web_tasks.cpp
    ${CMAKE_SOURCE_DIR}/web_amy_stubs.c
)

add_executable(grvep_web
    ${GRVEP_SOURCES}
    ${WEB_SOURCES}
    ${AMY_SOURCES}
    ${ENGINE_SOURCES}
    ${U8G2_SOURCES}
)

# ---- Include paths ----
target_include_directories(grvep_web PRIVATE
    # Web HAL headers (must come BEFORE system headers so they shadow Arduino.h etc.)
    ${CMAKE_SOURCE_DIR}/hal
    ${CMAKE_SOURCE_DIR}/hal/freertos
    ${CMAKE_SOURCE_DIR}
    # GrvEP project headers
    ${ROOT}/include
    ${ROOT}/src
    # Vendored engine headers (included as "engine/core/engine.h" etc.)
    ${ROOT}
    # AMY headers
    ${AMY_SRC}
    # U8g2 headers
    ${U8G2_CLIB}
    ${U8G2_SRC}
)

# ---- Compile definitions ----
# SIMULATOR=1 reuses every "not real ESP32 hardware" code path already present in
# src/main.cpp/src/audio_engine.cpp for the desktop simulator (AMY_AUDIO_IS_MINIAUDIO,
# the audioHandlerTask-queue-drain fallback in loop(), the on-screen control-label
# mirror) — all of which the web build needs too. __EMSCRIPTEN__ is defined
# automatically by emcc itself (used by src/audio_engine.cpp's audioInit() and by
# AMY's amy.c/amy.h/libminiaudio-audio.c — see web_amy_stubs.c's own comment).
# MA_ENABLE_AUDIO_WORKLETS: this vendored miniaudio.h's __EMSCRIPTEN__ backend has NO
# non-worklet fallback at all (confirmed by reading it — every code path under
# `#if defined(__EMSCRIPTEN__)` goes through emscripten_create_wasm_audio_worklet_node()),
# so this define (and -sAUDIO_WORKLET=1/-sWASM_WORKERS=1/-pthread below) aren't optional
# extras, they're the only way this backend produces sound at all. Matches AMY's own
# upstream web build (lib/AMY Synthesizer/Makefile's `web` target) for this exact file.
target_compile_definitions(grvep_web PRIVATE
    SIMULATOR=1
    OOPSIE_LED_FLAG=1
    AMY_WAVETABLE=1
    # Vendored engine (framework/engine/dsp/config.h defaults): match AMY's 44.1kHz,
    # same as simulator/CMakeLists.txt's own ENGINE_SR define.
    ENGINE_SR=44100
    MA_ENABLE_AUDIO_WORKLETS=1
)

# ---- Compile options ----
# -pthread: required at COMPILE time too, not just link — without it, libminiaudio-audio.c
# (and anything else) calling pthread_mutex_*/pthread_create fails to link with "undefined
# symbol", since Emscripten only provides real pthread_* implementations in a -pthread build
# (see the matching -pthread in target_link_options below for why this is unavoidable here).
target_compile_options(grvep_web PRIVATE
    -O2
    -pthread
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-deprecated-declarations
    -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
)

# ---- Emscripten link flags (structure/WEB_SIMULATOR.md's exact flag set, plus
# EXPORTED_FUNCTIONS/EXPORTED_RUNTIME_METHODS so the bridge functions and
# ccall/cwrap are actually reachable from docs/index.html's JS) ----
#   -sMODULARIZE=1 -sEXPORT_NAME=createGrvEP   module factory instead of an auto-run global
#   -sASYNCIFY               needed for this build's Emscripten-fiber-based FreeRTOS task
#                             emulation (web_tasks.cpp) — separate from, and in addition to,
#                             the audio worklet thread below
#   -pthread -sAUDIO_WORKLET=1 -sWASM_WORKERS=1   real Web Audio Worklet support, the only
#                             way this miniaudio.h backend produces sound in a browser (see
#                             target_compile_definitions' comment). Emscripten's own
#                             documentation (settings.js) states AUDIO_WORKLET "depends on
#                             WASM_WORKERS and Wasm SharedArrayBuffer" — i.e. -pthread is not
#                             optional once AUDIO_WORKLET is requested, and the resulting
#                             SharedArrayBuffer usage means the page MUST be served
#                             cross-origin-isolated (COOP/COEP response headers) in any
#                             browser enforcing Spectre mitigations. GitHub Pages cannot set
#                             custom response headers at all — docs/coi-serviceworker.js
#                             (vendored, see docs/index.html's own comment) works around
#                             that entirely client-side, so this still works as a plain
#                             GitHub Pages static site with no server-side config.
#                             ⚠️ Incompatible with -sSINGLE_FILE=1 (Emscripten hard-errors:
#                             "-sAUDIO_WORKLET does not support -sSINGLE_FILE") — dropped;
#                             the extra .wasm/.aw.js/.ww.js files are same-origin static
#                             assets GitHub Pages serves exactly like any other file, no
#                             CORS involved (CORS only applies cross-origin).
#   -sINITIAL_MEMORY=67108864 / -sALLOW_MEMORY_GROWTH=1   64MB start, grow as needed
#   --no-entry                no auto-called main(); JS calls grvepBoot() itself
# Each flag is passed as its own target_link_options() list element (rather than one
# combined LINK_FLAGS string) so CMake quotes the EXPORTED_FUNCTIONS/EXPORTED_RUNTIME_METHODS
# values (which contain brackets, commas and single quotes) correctly regardless of generator.
target_link_options(grvep_web PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=createGrvEP
    -sASYNCIFY
    -pthread
    -sAUDIO_WORKLET=1
    -sWASM_WORKERS=1
    -sINITIAL_MEMORY=67108864
    -sALLOW_MEMORY_GROWTH=1
    --no-entry
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPF32']"
    "-sEXPORTED_FUNCTIONS=['_grvepBoot','_simKeyPress','_getJoyXPtr','_getJoyYPtr','_getJoySWPtr','_getSliderPtr','_getOledBuffer','_getOledWidth','_getOledHeight','_getLedsBuffer','_getLedsCount','_isOledDirty','_isLedsDirty','_malloc','_free']"
    -lm
)

set_target_properties(grvep_web PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${ROOT}/docs"
    OUTPUT_NAME "grvep"
)
