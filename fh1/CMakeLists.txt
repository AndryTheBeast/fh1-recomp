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
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
