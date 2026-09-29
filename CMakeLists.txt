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

# Some of dynarmic's bundled dependencies declare cmake_minimum_required < 3.5, which CMake 4 rejects
# unless told the project is fine with 3.5 policies.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# --- Boost headers (needed by dynarmic: boost::icl and boost::variant, header-only) -------------
# SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so Boost is only downloaded and
# extracted, never configured as a sub-project.
FetchContent_Declare(boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(boost)
set(BOOST_ROOT ${boost_SOURCE_DIR} CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR ${boost_SOURCE_DIR} CACHE PATH "" FORCE)
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)

# --- dynarmic (ARM -> x86-64 JIT), A32 frontend only -----------------------------------------------
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dynarmic
  GIT_REPOSITORY https://github.com/lioncash/dynarmic.git
  GIT_TAG a41c380246d3d9f9874f0f792d234dc0cc17c180
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(dynarmic)

# --- SDL2 (window, OpenGL context, audio, gamepads): official prebuilt Visual C++ package -------
FetchContent_Declare(sdl2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(sdl2)

# --- carmadroid ---------------------------------------------------------------------------------
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp src/hle/*.cpp src/pcimport/*.cpp)
add_executable(carmadroid ${SOURCES} third_party/miniz/miniz.c)
target_include_directories(carmadroid PRIVATE src third_party/miniz ${sdl2_SOURCE_DIR}/include)
target_link_libraries(carmadroid PRIVATE dynarmic ${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib opengl32 winmm)
target_compile_definitions(carmadroid PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED)
if(MSVC)
  target_compile_options(carmadroid PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /utf-8>)
endif()

add_custom_command(TARGET carmadroid POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll $<TARGET_FILE_DIR:carmadroid>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/README.md $<TARGET_FILE_DIR:carmadroid>)
