cmake_minimum_required(VERSION 3.21)

project(mhd_test
    DESCRIPTION "Web TV server: live stream remuxed to fragmented MP4 over HTTP"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Supported perimeter
# ---------------------------------------------------------------------------

# The sources are Windows-only: stdafx.h pulls in <conio.h>, and the file server
# root is a hardcoded drive path. Say so here instead of letting a Linux build
# fail with a wall of preprocessor errors.
if(NOT MSVC)
    message(FATAL_ERROR
        "mhd_test builds only with MSVC: the sources are Windows-only (<conio.h>). "
        "Toolchain: Visual Studio 2022 Build Tools, toolset v143.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Only x64 is supported. Configure with -A x64, or use the 'win64' preset. "
        "The vendored libmicrohttpd ships only its x86_64 variant.")
endif()

# Debug and Release are the only configurations this project ever had. Pinning
# the list keeps MinSizeRel and RelWithDebInfo out of the generated solution
# rather than carrying variants nobody builds — and libmicrohttpd is only
# mapped for these two below.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# Two of the three live outside this repository, resolved by relative path so
# the layout documented in the README needs no configuration. Override any of
# them on the command line when your checkout differs.

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/../ffmpeg" CACHE PATH
    "FFmpeg win64-gpl-shared SDK root, with include/ lib/ bin/ inside")
set(COMMON_ROOT "${CMAKE_SOURCE_DIR}/../common" CACHE PATH
    "Sibling checkout of github.com/averagemean101/common")
set(MHD_ROOT "${CMAKE_SOURCE_DIR}/third_party/libmicrohttpd-0.9.73-w32-bin/x86_64/VS2019"
    CACHE PATH "Vendored libmicrohttpd 0.9.73 win64 binary distribution, VS2019 layout")

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg not found under FFMPEG_ROOT=${FFMPEG_ROOT}\n"
        "Expected a win64-gpl-shared build from github.com/BtbN/FFmpeg-Builds. "
        "See the README, or pass -DFFMPEG_ROOT=<path>.")
endif()

if(NOT EXISTS "${COMMON_ROOT}/avpp.cpp")
    message(FATAL_ERROR
        "'common' not found under COMMON_ROOT=${COMMON_ROOT}\n"
        "Clone github.com/averagemean101/common next to this repository, "
        "or pass -DCOMMON_ROOT=<path>.")
endif()

if(NOT EXISTS "${MHD_ROOT}/Release-dll/libmicrohttpd-dll.lib")
    message(FATAL_ERROR
        "Vendored libmicrohttpd not found under MHD_ROOT=${MHD_ROOT}")
endif()

# libmicrohttpd, as a DLL in both configurations.
#
# Do not "improve" this into a static link. The vendored Release-static
# libmicrohttpd.lib carries /GL (LTCG) objects produced by the VS2019 compiler,
# and linking those together with our VS2022 /GL objects fails outright:
#
#   LINK : fatal error C1047: object or library file ... was created with a
#   different version of the compiler than other objects
#
# Dropping /GL on our side does let it link, but then the whole program loses
# WholeProgramOptimization and every static-lib object emits LNK4099, because
# the Release-static distribution ships no .pdb. The DLL variant has neither
# problem. This is also why the original MSBuild project linked Release against
# the DLL and Debug against the static lib.
add_library(microhttpd SHARED IMPORTED)
set_target_properties(microhttpd PROPERTIES
    IMPORTED_IMPLIB_DEBUG     "${MHD_ROOT}/Debug-dll/libmicrohttpd-dll_d.lib"
    IMPORTED_IMPLIB_RELEASE   "${MHD_ROOT}/Release-dll/libmicrohttpd-dll.lib"
    IMPORTED_LOCATION_DEBUG   "${MHD_ROOT}/Debug-dll/libmicrohttpd-dll_d.dll"
    IMPORTED_LOCATION_RELEASE "${MHD_ROOT}/Release-dll/libmicrohttpd-dll.dll"
    INTERFACE_INCLUDE_DIRECTORIES
        "$<IF:$<CONFIG:Debug>,${MHD_ROOT}/Debug-dll,${MHD_ROOT}/Release-dll>"
    # Makes microhttpd.h declare its entry points __declspec(dllimport). The
    # MSBuild project defined neither MHD_W32LIB nor MHD_W32DLL, which worked
    # but only by accident.
    INTERFACE_COMPILE_DEFINITIONS MHD_W32DLL
)

# FFmpeg. The DLLs are resolved from the PATH at runtime (ffmpeg\bin) rather
# than copied next to the executable: that directory is ~1.5 GB.
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
foreach(component IN ITEMS avcodec avformat avutil avfilter avdevice swscale)
    target_link_libraries(ffmpeg INTERFACE "${FFMPEG_ROOT}/lib/${component}.lib")
endforeach()

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

set(MHD_TEST_SOURCES
    mhd_test/mhd_test.cpp
    "${COMMON_ROOT}/avpp.cpp"
)
set(MHD_TEST_HEADERS
    mhd_test/stdafx.h
    "${COMMON_ROOT}/avpp.h"
    "${COMMON_ROOT}/HttpServer.h"
    "${COMMON_ROOT}/ThreadSafeQueue.h"
    "${COMMON_ROOT}/utils.h"
)

add_executable(mhd_test
    ${MHD_TEST_SOURCES}
    ${MHD_TEST_HEADERS}
    mhd_test/Todo.txt
)

# Mirrors the filters the .vcxproj.filters used to provide, so the generated
# solution stays navigable.
source_group("Source Files" FILES ${MHD_TEST_SOURCES})
source_group("Header Files" FILES ${MHD_TEST_HEADERS})

target_include_directories(mhd_test PRIVATE
    # Keep this entry: common/avpp.cpp does #include "stdafx.h", and that header
    # lives here, not in common/. Removing it breaks the compilation of a file
    # that belongs to another repository.
    mhd_test
    "${COMMON_ROOT}"
)

target_link_libraries(mhd_test PRIVATE ffmpeg microhttpd)

target_compile_definitions(mhd_test PRIVATE UNICODE _UNICODE _CONSOLE)

target_compile_options(mhd_test PRIVATE
    /sdl
    /permissive-
    $<$<CONFIG:Release>:/Gy>
    $<$<CONFIG:Release>:/Oi>
    # Release shipped a .pdb under MSBuild; keep it.
    $<$<CONFIG:Release>:/Zi>
)

target_link_options(mhd_test PRIVATE
    $<$<CONFIG:Release>:/DEBUG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

# The MSBuild project carried /NODEFAULTLIB:libcmt[d] because its Debug
# configuration linked the static libmicrohttpd, which is built against the
# static CRT. Both configurations now use the DLL, whose import library pulls in
# no CRT objects, so the exclusion is gone.

set_target_properties(mhd_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # WholeProgramOptimization in the MSBuild project.
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    VS_DEBUGGER_ENVIRONMENT "PATH=${FFMPEG_ROOT}/bin;$(PATH)"
)

# libmicrohttpd must sit next to the executable. This is the step that used to
# be done by hand; $<TARGET_FILE:microhttpd> resolves to the per-configuration
# IMPORTED_LOCATION, so Debug and Release each get their own DLL.
add_custom_command(TARGET mhd_test POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:microhttpd>" "$<TARGET_FILE_DIR:mhd_test>"
    COMMENT "Staging libmicrohttpd next to the executable"
    VERBATIM
)

# No precompiled header on purpose. The MSBuild project set PrecompiledHeader to
# Create on both translation units without naming a through-header, so both got
# a bare /Yc writing the same .pch — it only worked because the build was
# serial. Both .cpp files include stdafx.h on their first line, so plain
# compilation is correct, and on two translation units a PCH buys nothing.
