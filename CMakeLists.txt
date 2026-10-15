cmake_minimum_required(VERSION 3.20)
project(island-capsule VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "island-capsule is Windows-only")
endif()

if(MSVC)
  # Link the C/C++ runtime statically (/MT, /MTd in Debug): island_loader.exe is then one self-contained file that
  # runs on a PC without the Visual C++ Redistributable. Must be set before the target is created.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  # NOTE: no /await. C++/WinRT + C++20 uses the standard <coroutine>.
  # /FS serializes PDB writes so parallel cl.exe processes don't collide on vc145.pdb (error C1041).
  add_compile_options(/EHsc /W3 /permissive- /Zc:__cplusplus /utf-8 /FS)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Console window (logs + keyboard shortcuts) is handy while developing. Turn it off for a release
# build you want to run silently / from "Start with Windows":  cmake -DISLAND_CONSOLE=OFF ..
option(ISLAND_CONSOLE "Build with a console window" ON)

# Phase 0-5: single executable (pill window + GSMTC media core + capsule UI + album color
# + visualizer + settings)
add_executable(island_loader
  src/loader.cpp
  src/audio/spectrum.cpp
  src/audio/volume.cpp
  src/lyrics/lrc.cpp
  src/lyrics/lrclib.cpp
  src/lyrics/lyrics_cache.cpp
  src/lyrics/lyrics_text.cpp
  src/media/gsmtc.cpp
  src/ui/pill.cpp
  src/ui/pill_input.cpp
  src/ui/pill_menu.cpp
  src/ui/pill_render.cpp
  src/ui/dwtext.cpp
  src/ui/transport.cpp
  src/ui/tray.cpp
  src/ui/settings_window.cpp
  src/ui/visualizer.cpp
  src/settings/settings.cpp
  src/theme/accent.cpp
  src/theme/brushes.cpp
)

# Exe icon + version info (Properties > Details). The version comes from project(... VERSION x.y.z) above,
# so bump it in one place. The .rc is generated into the build tree.
if(MSVC)
  enable_language(RC)
  set(ISLAND_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/island_capsule.ico")
  configure_file(resources/island_capsule.rc.in ${CMAKE_BINARY_DIR}/island_capsule.rc @ONLY)
  target_sources(island_loader PRIVATE ${CMAKE_BINARY_DIR}/island_capsule.rc)
  set_source_files_properties(${CMAKE_BINARY_DIR}/island_capsule.rc PROPERTIES OBJECT_DEPENDS ${ISLAND_ICON_PATH})
endif()

# Version header (ISLAND_VERSION_W) generated from the project version, so the startup log never goes stale.
configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/island_version.hpp @ONLY)

target_include_directories(island_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)

target_compile_definitions(island_loader PRIVATE UNICODE _UNICODE)

target_link_libraries(island_loader PRIVATE
  user32 gdi32 shcore shell32 advapi32 ole32 msimg32 gdiplus dwmapi d2d1 dwrite
  windowsapp runtimeobject winhttp
)

if(MSVC AND NOT ISLAND_CONSOLE)
  # keep wmain() as the entry point but without a console window
  target_link_options(island_loader PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
endif()

# Host-side unit tests for the pure lyrics code (LRC parser, JSON, title cleaning, ranking, cache format):
#   cmake -DISLAND_TESTS=ON .. && cmake --build . --target lyrics_test && bin/lyrics_test
option(ISLAND_TESTS "Build the lyrics unit tests" OFF)
if(ISLAND_TESTS)
  add_executable(lyrics_test tests/lyrics_test.cpp src/lyrics/lrc.cpp src/lyrics/lyrics_text.cpp src/lyrics/lyrics_cache.cpp)
  target_include_directories(lyrics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
