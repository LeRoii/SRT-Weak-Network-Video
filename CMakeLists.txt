cmake_minimum_required(VERSION 3.20)

project(srt_weak_network_video LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(ENABLE_SHARED ON CACHE BOOL "" FORCE)
set(ENABLE_STATIC OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_BONDING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    srt
    GIT_REPOSITORY https://github.com/Haivision/srt.git
    GIT_TAG v1.5.5
    GIT_SHALLOW TRUE
    PATCH_COMMAND
        ${CMAKE_COMMAND}
        -DSRT_SOURCE_DIR=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_srt.cmake
)
FetchContent_MakeAvailable(srt)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(ISAL REQUIRED IMPORTED_TARGET libisal)

set(COMMON_SOURCES
    src/common/adaptation.cpp
    src/common/network_quality.cpp
    src/common/utils.cpp
    src/fec/reed_solomon.cpp
    src/protocol/frame_assembler.cpp
    src/protocol/messages.cpp
    src/transport/srt_transport.cpp
)

add_executable(srt_weak_video
    ${COMMON_SOURCES}
    src/main.cpp
    src/receiver.cpp
    src/sender.cpp
    src/video/h264_decoder.cpp
    src/video/video_file_reader.cpp
    src/video/video_renderer.cpp
)

target_include_directories(srt_weak_video PRIVATE
    src
    ${srt_SOURCE_DIR}/srtcore
    ${srt_BINARY_DIR}
)
target_link_libraries(srt_weak_video PRIVATE
    srt_shared
    PkgConfig::FFMPEG
    PkgConfig::SDL2
    PkgConfig::ISAL
    Threads::Threads
)
target_compile_options(srt_weak_video PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()
add_executable(protocol_tests
    src/common/utils.cpp
    src/fec/reed_solomon.cpp
    src/protocol/frame_assembler.cpp
    src/protocol/messages.cpp
    tests/protocol_tests.cpp
)
target_include_directories(protocol_tests PRIVATE src)
target_link_libraries(protocol_tests PRIVATE PkgConfig::ISAL)
target_compile_options(protocol_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(adaptation_tests
    src/common/adaptation.cpp
    tests/adaptation_tests.cpp
)
target_include_directories(adaptation_tests PRIVATE src)
target_compile_options(adaptation_tests PRIVATE
    -UNDEBUG
    -Wall
    -Wextra
    -Wpedantic
)
add_test(NAME adaptation_tests COMMAND adaptation_tests)

add_executable(network_quality_tests
    src/common/network_quality.cpp
    tests/network_quality_tests.cpp
)
target_include_directories(network_quality_tests PRIVATE src)
target_compile_options(network_quality_tests PRIVATE
    -UNDEBUG
    -Wall
    -Wextra
    -Wpedantic
)
add_test(NAME network_quality_tests COMMAND network_quality_tests)
