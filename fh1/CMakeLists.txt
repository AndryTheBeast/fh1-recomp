# fh1 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fh1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FH1_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(fh1 WIN32 ${FH1_SOURCES})
else()
    add_executable(fh1 ${FH1_SOURCES})
endif()

# GPU_PLUGINS xenos builds the SDK's Xbox 360 GPU emulation and stages it next to fh1.exe.
rexglue_setup_target(fh1 GPU_PLUGINS xenos)

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
