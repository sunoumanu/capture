# CMakeLists.txt - portable build for ms2109_capture
#
# Quick start (Windows, PowerShell or cmd, MSVC + vcpkg):
#
#     cmake -S . -B build ^
#           -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
#     cmake --build build --config Release
#
#   The .exe lands at  build\Release\ms2109_capture.exe
#
# Quick start (Linux, with OpenCV from your distro):
#
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j
#
# Faster Windows builds with Ninja (optional - requires Ninja on PATH):
#
#     cmake -S . -B build -G Ninja ^
#           -DCMAKE_BUILD_TYPE=Release ^
#           -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
#     cmake --build build

cmake_minimum_required(VERSION 3.16)
project(ms2109_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if the user didn't pick a config (single-config generators).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# OpenCV: vcpkg's toolchain file makes this Just Work on Windows; on Linux
# pkg-config / system OpenCV is found the same way.
find_package(OpenCV REQUIRED)

add_executable(ms2109_capture ms2109_capture.cpp)
target_link_libraries(ms2109_capture PRIVATE ${OpenCV_LIBS})
target_include_directories(ms2109_capture PRIVATE ${OpenCV_INCLUDE_DIRS})

# MSVC-specific niceties: sane warning level, UTF-8 source, no min/max macros.
if(MSVC)
    target_compile_options(ms2109_capture PRIVATE /W3 /utf-8)
    target_compile_definitions(ms2109_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# On Windows, copy OpenCV's runtime DLLs next to the .exe after the build so
# you can just double-click the result without setting PATH yourself.
if(WIN32)
    add_custom_command(TARGET ms2109_capture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:ms2109_capture>
                $<TARGET_FILE_DIR:ms2109_capture>
        COMMAND_EXPAND_LISTS)
endif()
