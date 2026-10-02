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
endif()

include(generated/rexglue.cmake)

# Sources
set(FH1_SOURCES
    src/main.cpp
    src/fh1_crash_report.cpp
    src/fh1_autoplay.cpp
    src/fh1_trace_load.cpp
    src/fh1_nan_trap.cpp
    src/fh1_trace_epa.cpp
    src/fh1_profiler.cpp
    src/fh1_d3d_census.cpp
    src/fh1_d3d_census_report.cpp
    src/fh1_perf_hooks.cpp
    src/fh1_shader_dump.cpp
    src/fh1_native_system.cpp
    src/fh1_native_shaders.cpp
    src/fh1_native_targets.cpp
    src/fh1_shader_library.cpp
    # Native renderer from nfsc-recomp (GoatHonks), itself nfsmw-nx's (StevensND), GPL-3.0: see src/native/README.md.
    # Run with --nfsmw_renderizador=nativo.
    src/native/nfsmw_nativo_sistema.cpp
    src/native/nfsmw_nativo_destinos.cpp
    src/native/nfsmw_nativo_shaders.cpp
    src/native/nfsmw_shader_library.cpp
    src/native/nfsmw_nativo_ganchos.cpp
    src/native/nfsmw_nativo_dibujos.cpp
    src/native/nfsmw_nativo_texturas_pool.cpp
    src/native/nfsmw_nativo_captura.cpp
    src/native/nfsmw_ajustes_graficos.cpp
    src/native/nfsc_native_stubs.cpp
    src/native/nfsc_espera_corta.cpp
    src/native/ring_progress.cpp
)

if(WIN32)
    add_executable(fh1 WIN32 ${FH1_SOURCES})
else()
    add_executable(fh1 ${FH1_SOURCES})
endif()

# GPU_PLUGINS xenos builds the SDK's Xbox 360 GPU emulation and stages it next to fh1.exe.
rexglue_setup_target(fh1 GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(fh1 PRIVATE dbghelp)
endif()

# With the SDK built as a subdirectory (REXSDK_DIR, as tools/build_windows.ps1 does),
# rex_app.cpp and main.cpp include imgui.h, but rexruntime links imgui privately, so its
# include path does not reach the executable. Same for renderdoc_app.h, which
# rex/ui/vulkan/instance.h includes. (Fix from nfsmw-nx's app/CMakeLists.txt.)
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# xxhash.h for the native renderer's shader library (fh1_shader_library.cpp, fh1_native_shaders.cpp).
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/xxHash)
# The SDK presenter's SPIR-V for drawing the game image (fh1_native_targets.cpp).
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/src/ui/shaders)
# The copied native renderer: its headers, two SDK headers nfsc-recomp added (compat/rex), and the
# NFSC_NATIVE_RENDERER switch that keeps Most Wanted's game hooks out.
target_include_directories(fh1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/native ${CMAKE_CURRENT_SOURCE_DIR}/src/native/compat)
target_compile_definitions(fh1 PRIVATE NFSC_NATIVE_RENDERER=1)
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
