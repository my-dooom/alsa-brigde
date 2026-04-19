cmake_minimum_required(VERSION 3.13)

project(alsa_bridge VERSION 0.1.0 LANGUAGES C CXX)

# Default to Release when no build type is explicitly selected.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA alsa)

if(NOT ALSA_FOUND)
    message(FATAL_ERROR
        "ALSA development files were not found. On Raspberry Pi OS, install them with: "
        "sudo apt update && sudo apt install -y libasound2-dev pkg-config"
    )
endif()

add_executable(audio_bridge
    src/audio_bridge.cpp
    src/audio_config.cpp
    src/device_setup.cpp
    src/io/mcp3008.c
    src/process_audio.cpp
    src/process_block_mmap.cpp
    src/loudness_meter.cpp
    src/stream_format.cpp
)

target_include_directories(audio_bridge PRIVATE
    ${ALSA_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_compile_options(audio_bridge PRIVATE
    ${ALSA_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

target_link_directories(audio_bridge PRIVATE
    ${ALSA_LIBRARY_DIRS}
)

target_link_libraries(audio_bridge PRIVATE
    ${ALSA_LIBRARIES}
    pthread
)

install(TARGETS audio_bridge RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Optional Ableton Link tempo sync via Carabiner TCP bridge
# Requires a running Carabiner instance on the same host (default port 17000).
# Enable with: cmake -DENABLE_LINK_SYNC=ON ..
# ---------------------------------------------------------------------------
option(ENABLE_LINK_SYNC "Connect to Carabiner for Ableton Link tempo sync" OFF)

if(ENABLE_LINK_SYNC)
    target_sources(audio_bridge PRIVATE src/link_sync.cpp)
    target_compile_definitions(audio_bridge PRIVATE ENABLE_LINK_SYNC)
    message(STATUS "Ableton Link sync via Carabiner: ENABLED")
endif()

add_subdirectory(DeciBeliever EXCLUDE_FROM_ALL)

enable_testing()

add_executable(test_process_block
    tests/test_process_block.cpp
    src/process_audio.cpp
    src/process_block_mmap.cpp
    src/loudness_meter.cpp
    src/stream_format.cpp
)

add_executable(test_e2e
    tests/test_e2e.cc
    src/process_audio.cpp
    src/process_block_mmap.cpp
    src/loudness_meter.cpp
    src/stream_format.cpp
)

target_include_directories(test_process_block PRIVATE
    ${ALSA_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/DeciBeliever/include
)

target_include_directories(test_e2e PRIVATE
    ${ALSA_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/DeciBeliever/include
)

target_compile_options(test_process_block PRIVATE
    ${ALSA_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(test_e2e PRIVATE
    ${ALSA_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
)

target_link_directories(test_process_block PRIVATE
    ${ALSA_LIBRARY_DIRS}
)

target_link_directories(test_e2e PRIVATE
    ${ALSA_LIBRARY_DIRS}
)

target_link_libraries(test_process_block PRIVATE
    DeciBeliever_lib
    ${ALSA_LIBRARIES}
)

target_link_libraries(test_e2e PRIVATE
    DeciBeliever_lib
    ${ALSA_LIBRARIES}
)

add_test(NAME test_process_block COMMAND test_process_block)
add_test(NAME test_e2e COMMAND test_e2e)