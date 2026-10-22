cmake_minimum_required(VERSION 3.25)
project(pgr4 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "pgr4 needs clang-cl (the recompiled code uses clang builtins)")
endif()

add_subdirectory(thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# Recompiled game code (out/ppc: tools/recomp.sh from your own disc and title update; never committed).
set(PGR4_PPC_DIR "${CMAKE_SOURCE_DIR}/out/ppc")
file(GLOB PGR4_PPC_SOURCES CONFIGURE_DEPENDS "${PGR4_PPC_DIR}/*.cpp")
if(NOT PGR4_PPC_SOURCES)
    message(FATAL_ERROR "out/ppc is empty: run tools/recomp.sh first")
endif()
add_library(pgr4_ppc STATIC ${PGR4_PPC_SOURCES})
target_include_directories(pgr4_ppc PUBLIC "${PGR4_PPC_DIR}" thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(pgr4_ppc PRIVATE -Wno-everything)
target_compile_options(pgr4_ppc PUBLIC -msse4.1 -fno-strict-aliasing)
# Function names in crash reports and profiles.
target_compile_options(pgr4_ppc PRIVATE -gline-tables-only "/FI${CMAKE_SOURCE_DIR}/src/ppc_hooks.h")

# The recompiled code reads the host's raw timestamp counter for mftb; give it the console's
# timebase instead (tools/patch_mftb.py, GuestTimebase in src/kernel/runtime.cpp).
execute_process(COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/patch_mftb.py" "${PGR4_PPC_DIR}"
                OUTPUT_VARIABLE PGR4_MFTB_RESULT RESULT_VARIABLE PGR4_MFTB_CODE OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT PGR4_MFTB_CODE EQUAL 0)
    message(FATAL_ERROR "tools/patch_mftb.py failed on ${PGR4_PPC_DIR}")
endif()
message(STATUS "mftb: ${PGR4_MFTB_RESULT}")

# Local SDKs, not committed: the AMD FidelityFX SDK (MIT; ReGPU builds FSR 1's shaders from its headers)
# and a shared LGPL FFmpeg build (XMA audio). Defaults are under thirdparty/.
set(PGR4_FFX "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX" CACHE PATH "FidelityFX SDK Kits/FidelityFX")
set(PGR4_FFMPEG "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg" CACHE PATH "FFmpeg shared build (include, lib, bin)")
if(NOT EXISTS "${PGR4_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg is missing: extract a win64 LGPL shared build to thirdparty/ffmpeg (or set PGR4_FFMPEG)")
endif()

# ReGPU: the GPU.
set(REGPU_XENONRECOMP "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp" CACHE PATH "" FORCE)
set(REGPU_PPC_DIR "${PGR4_PPC_DIR}" CACHE PATH "" FORCE)
set(REGPU_FFX "${PGR4_FFX}" CACHE PATH "" FORCE)
add_subdirectory(thirdparty/ReGPU)

# Weak fallbacks for every import the recompiled code calls; real implementations in src/ win.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/stubs.cpp"
    COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/genstubs.py" "${PGR4_PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_BINARY_DIR}/generated/stubs.cpp"
    DEPENDS "${PGR4_PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_SOURCE_DIR}/tools/genstubs.py"
    COMMENT "Generating import stubs")

file(GLOB_RECURSE PGR4_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(pgr4 ${PGR4_SOURCES} "${CMAKE_BINARY_DIR}/generated/stubs.cpp")
# The game's headers first, then ReGPU's shared helpers (log.h, live_switch.h, png.h, precise_sleep.h):
# ReGPU provides them, the port uses the same ones.
target_include_directories(pgr4 PRIVATE src thirdparty/ReGPU/src)
target_link_libraries(pgr4 PRIVATE regpu pgr4_ppc XenonUtils synchronization winmm onecore)
target_compile_definitions(pgr4 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(pgr4 PRIVATE -gline-tables-only)
target_link_options(pgr4 PRIVATE /STACK:0x1000000 /DEBUG)
target_link_libraries(pgr4 PRIVATE dbghelp xaudio2 bcrypt winhttp)
# Microsoft's HLSL compiler beside pgr4.exe (ReGPU loads it from there; Wine's can't compile its shaders).
add_custom_command(TARGET pgr4 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REGPU_D3DCOMPILER}" $<TARGET_FILE_DIR:pgr4>)

target_include_directories(pgr4 PRIVATE "${PGR4_FFMPEG}/include")
target_link_libraries(pgr4 PRIVATE "${PGR4_FFMPEG}/lib/avcodec.lib" "${PGR4_FFMPEG}/lib/avutil.lib")
file(GLOB PGR4_FFMPEG_DLLS "${PGR4_FFMPEG}/bin/avcodec-*.dll" "${PGR4_FFMPEG}/bin/avutil-*.dll" "${PGR4_FFMPEG}/bin/swresample-*.dll")
add_custom_command(TARGET pgr4 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PGR4_FFMPEG_DLLS} $<TARGET_FILE_DIR:pgr4>)
