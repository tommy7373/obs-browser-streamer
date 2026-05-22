cmake_minimum_required(VERSION 3.24)
project(obs-web-preview VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pass at configure time:
#   -DCMAKE_PREFIX_PATH="C:/obs-plugin-work/.deps/obs-deps-2025-07-11-x64;C:/obs-plugin-work/.deps/obs-deps-qt6-2025-07-11-x64;C:/obs-plugin-work/.deps"

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
find_package(LibDataChannel REQUIRED)
qt_standard_project_setup()

# avcodec for our inline Opus encoder (OpusAudioCapture). Bundled in obs-deps;
# the configured CMAKE_PREFIX_PATH already points there.
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVCODEC_LIBRARY NAMES avcodec REQUIRED)
find_library(AVUTIL_LIBRARY  NAMES avutil  REQUIRED)

add_library(obs-web-preview MODULE
    src/plugin-main.cpp
    src/WebPreviewPlugin.cpp
    src/WebPreviewOutput.cpp
    src/WebPreviewDock.cpp
    src/BrowserStreamerSettings.cpp
    src/OpusAudioCapture.cpp
)

target_link_libraries(obs-web-preview PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
    LibDataChannel::LibDataChannel
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
)

target_include_directories(obs-web-preview PRIVATE ${AVCODEC_INCLUDE_DIR})

target_include_directories(obs-web-preview PRIVATE src third-party)

# Windows version + MSVC suppression for httplib
target_compile_definitions(obs-web-preview PRIVATE
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set(OBS_PLUGIN_BASE "C:/ProgramData/obs-studio/plugins/obs-web-preview"
    CACHE PATH "OBS plugin install base directory")

install(TARGETS obs-web-preview
    LIBRARY DESTINATION "${OBS_PLUGIN_BASE}/bin/64bit")

install(DIRECTORY data/
    DESTINATION "${OBS_PLUGIN_BASE}/data")

# Copy datachannel.dll alongside the plugin DLL
get_target_property(_dc_dll LibDataChannel::LibDataChannel IMPORTED_LOCATION_RELEASE)
if(_dc_dll)
    install(FILES "${_dc_dll}" DESTINATION "${OBS_PLUGIN_BASE}/bin/64bit")
endif()
