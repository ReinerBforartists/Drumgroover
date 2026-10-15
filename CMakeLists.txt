cmake_minimum_required(VERSION 3.16)

project(DrumGroover VERSION 0.8.0 LANGUAGES CXX)

set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/iPlug2")

# iPlug2 is not part of this repository (it is listed in .gitignore). The first configure fetches the exact revision
# DrumGroover is built against into ./iPlug2; raise DGG_IPLUG2_COMMIT to move to a newer iPlug2.
set(DGG_IPLUG2_URL "https://github.com/iPlug2/iPlug2.git")
set(DGG_IPLUG2_COMMIT "b64192fe18afd9bc9a1fe324db5aceb48f4a0eee")

if(NOT EXISTS "${IPLUG2_DIR}/iPlug2.cmake")
  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    message(FATAL_ERROR "iPlug2 not found at: ${IPLUG2_DIR}. "
      "Install git so CMake can fetch it, or clone ${DGG_IPLUG2_URL} (revision ${DGG_IPLUG2_COMMIT}) into that folder yourself.")
  endif()
  message(STATUS "Fetching iPlug2 ${DGG_IPLUG2_COMMIT} (first configure only, this can take a minute)")
  file(MAKE_DIRECTORY "${IPLUG2_DIR}")
  foreach(step
      "init"
      "remote add origin ${DGG_IPLUG2_URL}"
      "fetch --depth 1 origin ${DGG_IPLUG2_COMMIT}"
      "checkout --force FETCH_HEAD"
      "submodule update --init --recursive")
    separate_arguments(step_args UNIX_COMMAND "${step}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${step_args}
      WORKING_DIRECTORY "${IPLUG2_DIR}" RESULT_VARIABLE step_result)
    if(NOT step_result EQUAL 0)
      message(FATAL_ERROR "Fetching iPlug2 failed at 'git ${step}' (exit code ${step_result}). Check the network connection, "
        "delete the folder ${IPLUG2_DIR} and configure again.")
    endif()
  endforeach()
endif()

if(NOT EXISTS "${IPLUG2_DIR}/iPlug2.cmake")
  message(FATAL_ERROR "iPlug2.cmake not found in ${IPLUG2_DIR}")
endif()
include("${IPLUG2_DIR}/iPlug2.cmake")

find_package(iPlug2 REQUIRED)

# Web UI source files — actual location for BOTH Debug live-reload and
# Release bundling. Must be <project root>/resources/web/ (lowercase
# "resources"), NOT source/Resources/web/ — that's the folder
# _iplug_add_web_resources() in IPlugPlugin.cmake keys its subdirectory
# detection on ("/resources/web/" in the absolute path), and where
# PLUG_RESOURCES_DIR (${PROJECT_DIR}/resources) points.
file(GLOB DGG_WEB_RESOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/*"
)

# Optional AVX2 build. The DSP (snare/ride/cowbell engines) is float-Newton
# plus per-sample math, so it benefits a lot from AVX2 -- bakes finish much
# faster on AVX2 CPUs. Off by default to keep the binary runnable on older
# CPUs; enable with -DDGG_AVX2=ON. (No FMA: keeps results bit-comparable.)
option(DGG_AVX2 "Compile with AVX2 instructions (faster baking/rendering, requires an AVX2 CPU)" OFF)
if(DGG_AVX2)
  if(MSVC)
    add_compile_options(/arch:AVX2)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-mavx2)
  else()
    message(WARNING "DGG_AVX2=ON but no AVX2 flag is known for this compiler; ignoring.")
  endif()
endif()

# PROJECT_ROOT_PATH: lets DrumGroover.cpp call LoadIndexHtml() with a path
# whose parent directory is the project root (where resources/web/ lives),
# instead of __FILE__'s own directory (source/, one level too deep — see
# the WEB_RESOURCES comment above for why they don't match). Not a real
# file, just a string LoadIndexHtml() calls parent_path() on.
add_compile_definitions(
  PROJECT_ROOT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/x"
)

# DrumGroover_mac.mm: NSSavePanel/NSOpenPanel (Objective-C++) implementation
# of PlatformFileDialog.h, used by DrumGroover.cpp's Export/Save/Load Project
# handlers on macOS (see that file's OS_MAC branches). APPLE-only source —
# MSVC can't compile Objective-C++, so it must not be listed on Windows.
set(DGG_PLATFORM_SOURCES "")
if(APPLE)
  list(APPEND DGG_PLATFORM_SOURCES source/DrumGroover_mac.mm)
endif()

# Windows: the web UI is compiled into the plugin DLL (served from memory by the WebView), so the
# DLL needs no files next to it. Regenerated at configure time whenever a file in resources/web changes.
if(WIN32)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DggEmbedWeb.cmake")
  set(DGG_WEB_BUNDLE_CPP "${CMAKE_BINARY_DIR}/dgg_web_bundle.cpp")
  dgg_embed_web_resources("${DGG_WEB_BUNDLE_CPP}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/web")
  list(APPEND DGG_PLATFORM_SOURCES "${DGG_WEB_BUNDLE_CPP}")
endif()

# Windows: use our own WebView2 backend (source/platform/DggWebView_win.cpp) instead of the one in
# iPlug2, so the iPlug2 tree can stay a pristine checkout. WebView.cmake is normally only included
# inside iplug_add_plugin(); including it here first defines iPlug2::WebView so we can swap the source.
if(WIN32)
  include("${IPLUG2_DIR}/Scripts/cmake/WebView.cmake")
  get_target_property(_dgg_webview_srcs iPlug2::WebView INTERFACE_SOURCES)
  list(FILTER _dgg_webview_srcs EXCLUDE REGEX "IPlugWebView_win\\.cpp$")
  list(APPEND _dgg_webview_srcs "${CMAKE_CURRENT_SOURCE_DIR}/source/platform/DggWebView_win.cpp")
  set_property(TARGET iPlug2::WebView PROPERTY INTERFACE_SOURCES "${_dgg_webview_srcs}")
endif()

iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    source/DrumGroover.cpp
    ${DGG_PLATFORM_SOURCES}
    source/dsp/RhythmEngine.cpp
    source/dsp/Types.cpp
    source/dsp/ArticulationAndCCMapper.cpp
    source/dsp/MidiFileExporter.cpp
    source/dsp/MidiExportService.cpp
    source/dsp/GrooveGenerator.cpp
    source/dsp/Song.cpp
    source/dsp/Scheduler.cpp
    source/dsp/PreviewScheduler.cpp
  WEB_RESOURCES
    ${DGG_WEB_RESOURCES}
  FORMATS VST3
  UI WEBVIEW
)
