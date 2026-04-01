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
    src/mcp3008.c
    src/process_audio.cpp
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
)

install(TARGETS audio_bridge RUNTIME DESTINATION bin)