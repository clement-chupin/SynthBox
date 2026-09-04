# CMake toolchain file for cross-compiling the GrvEP simulator into a native Windows
# .exe from Linux, via MinGW-w64. Used by build_windows.sh, which bootstraps the
# compiler + SDL2 dev files into .winbuild-toolchain/ (no root/sudo needed — everything
# is fetched as plain files, not installed system-wide) and then invokes CMake with
# -DCMAKE_TOOLCHAIN_FILE=this file. Not meant to be pointed at directly by hand; run
# build_windows.sh instead, which sets MINGW_TC_DIR to match where it bootstrapped things.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED MINGW_TC_DIR)
    set(MINGW_TC_DIR "${CMAKE_CURRENT_LIST_DIR}/.winbuild-toolchain")
endif()

# "-posix" variant: pthreads via winpthreads (AMY's audio render thread uses
# pthread_create directly, lib/AMY Synthesizer/src/libminiaudio-audio.c), statically
# linkable for a single self-contained .exe (see build_windows.sh's link flags).
set(CMAKE_C_COMPILER   "${MINGW_TC_DIR}/usr/bin/x86_64-w64-mingw32-gcc-posix")
set(CMAKE_CXX_COMPILER "${MINGW_TC_DIR}/usr/bin/x86_64-w64-mingw32-g++-posix")
set(CMAKE_RC_COMPILER  "${MINGW_TC_DIR}/usr/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR           "${MINGW_TC_DIR}/usr/bin/x86_64-w64-mingw32-gcc-ar-posix" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${MINGW_TC_DIR}/usr/bin/x86_64-w64-mingw32-gcc-ranlib-posix" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${MINGW_TC_DIR}/usr/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
