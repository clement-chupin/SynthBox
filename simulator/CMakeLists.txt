cmake_minimum_required(VERSION 3.16)
project(grvep_sim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ---- Paths ----
set(ROOT      ${CMAKE_SOURCE_DIR}/..)
set(AMY_SRC   ${ROOT}/lib/AMY\ Synthesizer/src)
set(U8G2_SRC  ${ROOT}/lib/U8g2/src)
set(U8G2_CLIB ${U8G2_SRC}/clib)
set(SDL2_INC  ${CMAKE_SOURCE_DIR}/sdl2_local/usr/include)
set(SDL2_LIB  ${CMAKE_SOURCE_DIR}/sdl2_local/usr/lib/x86_64-linux-gnu)

# ---- AMY sources (exclude ESP32/Web/Python-specific files) ----
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

# ---- GrvEP original sources (compiled with simulator HAL) ----
set(GRVEP_SOURCES
    ${ROOT}/src/main.cpp
    ${ROOT}/src/audio_engine.cpp
    ${ROOT}/src/NoteMap.cpp
    # Replaced: keyboard.cpp, mux.cpp, oled.cpp, Leds.cpp, midi_usb.cpp
)

# ---- Simulator replacement sources ----
set(SIM_SOURCES
    ${CMAKE_SOURCE_DIR}/sim_main.cpp
    ${CMAKE_SOURCE_DIR}/sim_keyboard.cpp
    ${CMAKE_SOURCE_DIR}/sim_mux.cpp
    ${CMAKE_SOURCE_DIR}/sim_oled.cpp
    ${CMAKE_SOURCE_DIR}/sim_leds.cpp
    ${CMAKE_SOURCE_DIR}/sim_midi.cpp
    ${CMAKE_SOURCE_DIR}/sim_state.cpp
    ${CMAKE_SOURCE_DIR}/sim_window.cpp
    ${CMAKE_SOURCE_DIR}/sim_mp3dec.cpp
)

add_executable(grvep_sim
    ${GRVEP_SOURCES}
    ${SIM_SOURCES}
    ${AMY_SOURCES}
    ${U8G2_SOURCES}
)

# ---- Include paths ----
target_include_directories(grvep_sim PRIVATE
    # Simulator HAL headers (must come BEFORE system headers so they shadow Arduino.h etc.)
    ${CMAKE_SOURCE_DIR}/hal
    ${CMAKE_SOURCE_DIR}/hal/freertos
    ${CMAKE_SOURCE_DIR}
    # GrvEP project headers
    ${ROOT}/include
    ${ROOT}/src
    # AMY headers
    ${AMY_SRC}
    # U8g2 headers
    ${U8G2_CLIB}
    ${U8G2_SRC}
    # SDL2 headers (include arch-specific dir for _real_SDL_config.h)
    ${SDL2_INC}
    ${SDL2_INC}/SDL2
    ${SDL2_INC}/x86_64-linux-gnu
    ${SDL2_INC}/x86_64-linux-gnu/SDL2
)

# ---- Compile definitions ----
target_compile_definitions(grvep_sim PRIVATE
    SIMULATOR=1
    OOPSIE_LED_FLAG=1
    # AMY platform (Linux, not ESP32)
    # Don't define ARDUINO — Amy checks !defined(ARDUINO) to use miniaudio
)

# ---- Compile options ----
target_compile_options(grvep_sim PRIVATE
    -O2
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-deprecated-declarations
    -fno-strict-aliasing
    # U8g2 clib compiles as C, needs relaxed settings
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
)

# ---- Link ----
target_link_libraries(grvep_sim PRIVATE
    # SDL2 — link to the system runtime directly (the local .so is a dangling symlink)
    /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0.3000.0
    # Pthreads
    pthread
    # Math
    m
    # Dynamic linking for miniaudio
    dl
)

# Set RPATH so the binary finds libSDL2.so at runtime
set_target_properties(grvep_sim PROPERTIES
    BUILD_RPATH "${SDL2_LIB};/usr/lib/x86_64-linux-gnu"
    INSTALL_RPATH "${SDL2_LIB}"
)
