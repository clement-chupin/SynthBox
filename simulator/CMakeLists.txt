cmake_minimum_required(VERSION 3.16)
project(grvep_sim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ---- Paths ----
set(ROOT      ${CMAKE_SOURCE_DIR}/..)
set(AMY_SRC   ${ROOT}/lib/AMY\ Synthesizer/src)
set(U8G2_SRC  ${ROOT}/lib/U8g2/src)
set(U8G2_CLIB ${U8G2_SRC}/clib)
if(WIN32)
    # Cross-compiling via toolchain-mingw64.cmake / build_windows.sh — SDL2_MINGW_DIR
    # is set by build_windows.sh to wherever it bootstrapped the SDL2 mingw devel package.
    if(NOT DEFINED SDL2_MINGW_DIR)
        set(SDL2_MINGW_DIR ${CMAKE_SOURCE_DIR}/.winbuild-toolchain/SDL2-mingw/x86_64-w64-mingw32)
    endif()
    set(SDL2_INC ${SDL2_MINGW_DIR}/include)
    set(SDL2_LIB ${SDL2_MINGW_DIR}/lib)
else()
    set(SDL2_INC  ${CMAKE_SOURCE_DIR}/sdl2_local/usr/include)
    set(SDL2_LIB  ${CMAKE_SOURCE_DIR}/sdl2_local/usr/lib/x86_64-linux-gnu)
endif()

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
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:NOMINMAX>
    # ^ SDL2 pulls in <windows.h> transitively on this target; without
    # WIN32_LEAN_AND_MEAN it drags in <dlgs.h>, whose own `CRGB` typedef (a Windows
    # GDI dialog-template struct, unrelated to and unluckily same-named as FastLED's
    # CRGB used throughout main.cpp) collides at global scope. NOMINMAX avoids the
    # matching min/max macro collision some other Windows header would introduce.
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
if(WIN32)
    # Fully static single-file .exe: no SDL2.dll / libwinpthread-1.dll / libstdc++-6.dll
    # to ship alongside it. -mwindows = no console window (a GUI app, like any other
    # Windows program); -static(-libgcc/-libstdc++) + -lwinpthread statically pull in
    # the mingw/pthread runtime instead of needing their DLLs at runtime. The rest of
    # the list is SDL2's own static-link dependency set for Windows (from its
    # sdl2-config --static-libs), no dl/pthread (POSIX-only, not on this target).
    target_link_libraries(grvep_sim PRIVATE
        -static -static-libgcc -static-libstdc++ -lwinpthread
        -mwindows
        mingw32 SDL2main SDL2
        m dinput8 dxguid dxerr8 user32 gdi32 winmm imm32 ole32 oleaut32 shell32 setupapi version uuid
    )
    target_link_directories(grvep_sim PRIVATE ${SDL2_LIB})
else()
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
endif()
