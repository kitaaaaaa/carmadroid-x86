cmake_minimum_required(VERSION 3.20)
project(carmadroid-x86 C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "carmadroid-x86 currently targets 64-bit Windows (MSVC).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- Unicorn (ARM CPU emulator), built from source with only the ARM backend -------------------
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(UNICORN_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(unicorn
  GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
  GIT_TAG v2.1.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unicorn)

# --- SDL2 (window, OpenGL context, audio, gamepads): official prebuilt Visual C++ package -------
FetchContent_Declare(sdl2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(sdl2)

# --- carmadroid ---------------------------------------------------------------------------------
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp src/hle/*.cpp)
add_executable(carmadroid ${SOURCES} third_party/miniz/miniz.c)
target_include_directories(carmadroid PRIVATE src third_party/miniz ${sdl2_SOURCE_DIR}/include)
target_link_libraries(carmadroid PRIVATE unicorn ${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib opengl32 winmm)
target_compile_definitions(carmadroid PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED)
if(MSVC)
  target_compile_options(carmadroid PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /utf-8>)
endif()

add_custom_command(TARGET carmadroid POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll $<TARGET_FILE_DIR:carmadroid>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/README.md $<TARGET_FILE_DIR:carmadroid>)
