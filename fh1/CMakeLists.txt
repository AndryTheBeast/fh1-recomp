# fh1 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fh1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: keep function names for crash reports (src/fh1_crash_report.cpp). Line tables
# only, so optimisation and build time are unchanged; the linker writes a .pdb next to
# each binary. Set before rexglue.cmake so the SDK built as a subdirectory gets it too.
if(WIN32)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-gline-tables-only>)
    add_link_options(-g)
    # The binaries are given to other people (the pre-release): no path of the PC that built them. Source
    # names in log lines and crash reports start at "fh1-recomp", and each binary names its .pdb without a
    # folder (the crash report finds it next to the binary).
    get_filename_component(FH1_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_compile_options(-ffile-prefix-map=${FH1_REPO}=fh1-recomp)
    add_link_options(-Wl,/pdbaltpath:%_PDB%)
endif()

# GCC (Switch) fuses a multiply and an add into one FMA by default (-ffp-contract=fast), so the recompiled game code
# would round differently from the Xbox 360. The SDK turns that off only for its own targets; the game's targets
# made here need it too (found by GoatHonks on the console: 359k differences in Carbon's mixer validation).
# Clang (PC) is not touched.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-ffp-contract=off)
endif()

include(generated/rexglue.cmake)

# GCC (Switch): the function registration list is one function of about 4 MB of source that runs once at
# start. GCC's optimizer had not finished it after 33 minutes (2026-10-06); unoptimized it compiles in seconds.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE generated/default/fh1_register.cpp
                        generated/xmediafacade_default/fh1_register.cpp
                        generated/speechfacade_default/fh1_register.cpp
                 APPEND PROPERTY COMPILE_OPTIONS -O0)
endif()

# Sources
set(FH1_SOURCES
    src/main.cpp
    src/fh1_crash_report.cpp
    src/fh1_autoplay.cpp
    src/fh1_unpack_image.cpp
    src/fh1_perf_overlay.cpp
    src/fh1_trace_load.cpp
    src/fh1_nan_trap.cpp
    src/fh1_trace_epa.cpp
    src/fh1_profiler.cpp
    src/fh1_d3d_census.cpp
    src/fh1_d3d_census_report.cpp
    src/fh1_perf_hooks.cpp
    src/fh1_fps60.cpp
    src/fh1_shader_dump.cpp
    src/fh1_static_modules.cpp
    # FH1's native renderer (--fh1_renderer=native). Started from nfsc-recomp's (GoatHonks), itself nfsmw-nx's
    # (StevensND), GPL-3.0: see src/native/README.md.
    src/native/fh1_native_system.cpp
    src/native/fh1_native_targets.cpp
    src/native/fh1_native_shaders.cpp
    src/native/fh1_shader_library.cpp
    src/native/fh1_extra_shaders.cpp
    src/native/fh1_native_hooks.cpp
    src/native/fh1_native_draws.cpp
    src/native/fh1_native_texture_pool.cpp
    src/native/fh1_native_capture.cpp
    src/native/fh1_graphics_settings.cpp
    src/native/fh1_native_stubs.cpp
    src/native/fh1_short_wait.cpp
    src/native/ring_progress.cpp
)

if(WIN32)
    # The icon of fh1.exe: the drawn one of the repository, or this PC's own (res/fh1_local.ico, ignored by git)
    # when there is one. See res/fh1.rc.in and tools/fh1_make_icon.py.
    set(FH1_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/fh1.ico")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/fh1_local.ico")
        set(FH1_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/fh1_local.ico")
    endif()
    message(STATUS "fh1.exe icon: ${FH1_ICON}")
    configure_file(res/fh1.rc.in "${CMAKE_CURRENT_BINARY_DIR}/fh1_icon.rc" @ONLY)
    list(APPEND FH1_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/fh1_icon.rc")
    add_executable(fh1 WIN32 ${FH1_SOURCES})
else()
    add_executable(fh1 ${FH1_SOURCES})
endif()

# GPU_PLUGINS xenos builds the SDK's Xbox 360 GPU emulation and stages it next to fh1.exe.
rexglue_setup_target(fh1 GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(fh1 PRIVATE dbghelp)
endif()

# The native renderer's shipped pipeline list (state keys and shader fingerprints, no game data; made by
# tools/fh1_pipelines.py) goes next to fh1.exe, where LoadListPipelines reads it.
add_custom_command(TARGET fh1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/data/fh1_pipelines.nfpl $<TARGET_FILE_DIR:fh1>/fh1_pipelines.nfpl)

# With the SDK built as a subdirectory (REXSDK_DIR, as tools/build_windows.ps1 does),
# rex_app.cpp and main.cpp include imgui.h, but rexruntime links imgui privately, so its
# include path does not reach the executable. Same for renderdoc_app.h, which
# rex/ui/vulkan/instance.h includes. (Fix from nfsmw-nx's app/CMakeLists.txt.)
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# xxhash.h for the native renderer's shader library (src/native/fh1_shader_library.cpp, fh1_native_shaders.cpp).
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/xxHash)
# The SDK presenter's SPIR-V for drawing the game image (fh1_native_targets.cpp).
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/ui/shaders)
# The native renderer: its headers and two SDK headers nfsc-recomp added (compat/rex).
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/native ${CMAKE_CURRENT_SOURCE_DIR}/src/native/compat)
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Nintendo Switch: package the executable as fh1-nx.nro (devkitPro elf2nro). The Switch-only build files (toolchain
# file, NRO build script, Mesa patch, SD package) are in the separate fh1-nx repository, which configures this
# project with its toolchain file. Layout and options follow GoatHonks' Carbon port and StevensND's nfsmw-nx.
if(REXGLUE_PLATFORM_SWITCH)
    # LTO on the game code and the app (not on the SDK libraries), as in nfsmw-nx (its step 6). Off by default: the
    # whole-program phase over FH1's generated code (about 320 MB of C++) needs far more memory than Carbon's.
    option(FH1_LTO "LTO on the game code and the app (Switch)" OFF)
    if(FH1_LTO)
        foreach(target_lto fh1_recomp fh1)
            if(TARGET ${target_lto})
                target_compile_options(${target_lto} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(fh1 PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    rexglue_switch_add_nro(fh1 OUTPUT "fh1-nx" NAME "Forza Horizon" AUTHOR "AndryTheBeast" VERSION "0.0.1")
endif()
