cmake_minimum_required(VERSION 3.16)
project(grvep_web CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ---- Paths ----
set(ROOT      ${CMAKE_SOURCE_DIR}/..)
set(AMY_SRC   ${ROOT}/lib/AMY\ Synthesizer/src)
set(U8G2_SRC  ${ROOT}/lib/U8g2/src)
set(U8G2_CLIB ${U8G2_SRC}/clib)

# ---- AMY sources (same as simulator, minus SDL-specific files) ----
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
    ${AMY_SRC}/libminiaudio-audio.c   # miniaudio auto-detects Emscripten → WebAudio backend
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

# ---- GrvEP original sources ----
set(GRVEP_SOURCES
    ${ROOT}/src/main.cpp
    ${ROOT}/src/audio_engine.cpp
    ${ROOT}/src/NoteMap.cpp
)

# ---- Web HAL replacement sources ----
set(WEB_SOURCES
    ${CMAKE_SOURCE_DIR}/web_main.cpp
    ${CMAKE_SOURCE_DIR}/web_keyboard.cpp
    ${CMAKE_SOURCE_DIR}/web_mux.cpp
    ${CMAKE_SOURCE_DIR}/web_oled.cpp
    ${CMAKE_SOURCE_DIR}/web_leds.cpp
    ${CMAKE_SOURCE_DIR}/web_midi.cpp
    ${CMAKE_SOURCE_DIR}/web_state.cpp
    ${CMAKE_SOURCE_DIR}/web_amy_stubs.c   # stubs for emscripten_lock_* (single-threaded build)
    ${CMAKE_SOURCE_DIR}/web_mp3dec.cpp    # no-op Helix MP3 stubs (no filesystem in WASM)
)

add_executable(grvep_web
    ${GRVEP_SOURCES}
    ${WEB_SOURCES}
    ${AMY_SOURCES}
    ${U8G2_SOURCES}
)

# ---- Include paths ----
target_include_directories(grvep_web PRIVATE
    ${CMAKE_SOURCE_DIR}/hal
    ${CMAKE_SOURCE_DIR}/hal/freertos
    ${CMAKE_SOURCE_DIR}
    ${ROOT}/include
    ${ROOT}/src
    ${AMY_SRC}
    ${U8G2_CLIB}
    ${U8G2_SRC}
)

# ---- Compile definitions ----
target_compile_definitions(grvep_web PRIVATE
    SIMULATOR=1
    OOPSIE_LED_FLAG=1
)

# ---- Compile options ----
target_compile_options(grvep_web PRIVATE
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

# ---- Emscripten link flags ----
# Exported functions: input injectors + output buffer accessors
set(EXPORTED_FUNCS
    "_main"
    "_simKeyPress"
    "_getJoyXPtr"
    "_getJoyYPtr"
    "_getJoySWPtr"
    "_getSliderPtr"
    "_getOledBuffer"
    "_getOledWidth"
    "_getOledHeight"
    "_getLedsBuffer"
    "_getLedsCount"
    "_isOledDirty"
    "_isLedsDirty"
    "_malloc"
    "_free"
)
string(JOIN "," EXPORTED_FUNCS_STR ${EXPORTED_FUNCS})

target_link_options(grvep_web PRIVATE
    -sEXPORTED_FUNCTIONS=[${EXPORTED_FUNCS_STR}]
    -sEXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPU8","HEAPF32","HEAP8"]
    -sINITIAL_MEMORY=67108864      # 64 MB
    -sALLOW_MEMORY_GROWTH=1
    -sASYNCIFY                     # required for miniaudio WebAudio callback
    -sASYNCIFY_STACK_SIZE=65536
    -sMODULARIZE=1
    -sEXPORT_NAME=createGrvEP
    -sSINGLE_FILE=1                # embed WASM inline so GitHub Pages serves it without CORS issues
    -lm
    --no-entry
)

# Output to docs/ so GitHub Pages serves it directly
set_target_properties(grvep_web PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ROOT}/docs
    OUTPUT_NAME              grvep
    SUFFIX                   .js
)
