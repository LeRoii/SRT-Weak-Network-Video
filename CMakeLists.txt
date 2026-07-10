cmake_minimum_required(VERSION 3.20)

project(srt_weak_network_video LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32 AND DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(ENV{PKG_CONFIG_PATH}
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig;${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/pkgconfig"
    )
endif()

if(MSVC)
    set(PROJECT_WARNING_FLAGS /W4)
    set(TEST_WARNING_FLAGS /UNDEBUG /W4)
else()
    set(PROJECT_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    set(TEST_WARNING_FLAGS -UNDEBUG -Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
set(ENABLE_SHARED ON CACHE BOOL "" FORCE)
set(ENABLE_STATIC OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_BONDING OFF CACHE BOOL "" FORCE)
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
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
find_package(Python3 COMPONENTS Interpreter)
set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_LINK_LIBS "")
set(SDL2_LINK_LIBS "")

if(WIN32)
    find_package(FFMPEG REQUIRED)
    find_package(SDL2 CONFIG REQUIRED)
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
    set(FFMPEG_LINK_LIBS ${FFMPEG_LIBRARIES})
    set(SDL2_LINK_LIBS SDL2::SDL2)
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswscale
    )
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(FFMPEG_LINK_LIBS PkgConfig::FFMPEG)
    set(SDL2_LINK_LIBS PkgConfig::SDL2)
endif()

set(ISAL_TARGET "")
if(WIN32)
    find_package(unofficial-isal CONFIG QUIET)
    if(TARGET unofficial::isal::isal)
        set(ISAL_TARGET unofficial::isal::isal)
    endif()
endif()

if(NOT ISAL_TARGET)
    pkg_check_modules(ISAL REQUIRED IMPORTED_TARGET libisal)
    set(ISAL_TARGET PkgConfig::ISAL)
endif()

set(COMMON_SOURCES
    src/common/adaptation.cpp
    src/common/frame_pacer.cpp
    src/common/latency_stats.cpp
    src/common/network_quality.cpp
    src/common/runtime_config.cpp
    src/common/utils.cpp
    src/fec/reed_solomon.cpp
    src/protocol/frame_assembler.cpp
    src/protocol/messages.cpp
    src/transport/srt_transport.cpp
    src/transport/udp_transport.cpp
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime-config.yaml
    ${CMAKE_CURRENT_BINARY_DIR}/runtime-config.yaml
    COPYONLY
)

add_executable(srt_weak_video
    ${COMMON_SOURCES}
    src/main.cpp
    src/receiver.cpp
    src/sender.cpp
    src/video/h264_decoder.cpp
    src/video/output_processor.cpp
    src/video/video_source_reader.cpp
    src/video/video_renderer.cpp
)

target_include_directories(srt_weak_video PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
    ${srt_SOURCE_DIR}/common
    ${srt_SOURCE_DIR}/srtcore
    ${srt_BINARY_DIR}
)
if(MSVC)
    target_compile_definitions(srt_weak_video PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        SDL_MAIN_HANDLED
    )
endif()
target_link_libraries(srt_weak_video PRIVATE
    srt_shared
    ${FFMPEG_LINK_LIBS}
    ${SDL2_LINK_LIBS}
    ${ISAL_TARGET}
    Threads::Threads
)
target_compile_options(srt_weak_video PRIVATE ${PROJECT_WARNING_FLAGS})

enable_testing()
add_executable(protocol_tests
    src/common/utils.cpp
    src/fec/reed_solomon.cpp
    src/protocol/frame_assembler.cpp
    src/protocol/messages.cpp
    tests/protocol_tests.cpp
)
target_include_directories(protocol_tests PRIVATE src)
target_link_libraries(protocol_tests PRIVATE ${ISAL_TARGET})
target_compile_options(protocol_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(frame_pacer_tests
    src/common/frame_pacer.cpp
    tests/frame_pacer_tests.cpp
)
target_include_directories(frame_pacer_tests PRIVATE src)
target_compile_options(frame_pacer_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME frame_pacer_tests COMMAND frame_pacer_tests)

add_executable(adaptation_tests
    src/common/adaptation.cpp
    tests/adaptation_tests.cpp
)
target_include_directories(adaptation_tests PRIVATE src)
target_compile_options(adaptation_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME adaptation_tests COMMAND adaptation_tests)

add_executable(network_quality_tests
    src/common/network_quality.cpp
    tests/network_quality_tests.cpp
)
target_include_directories(network_quality_tests PRIVATE src)
target_compile_options(network_quality_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME network_quality_tests COMMAND network_quality_tests)

add_executable(runtime_config_tests
    src/common/runtime_config.cpp
    tests/runtime_config_tests.cpp
)
target_include_directories(runtime_config_tests PRIVATE src)
target_compile_options(runtime_config_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME runtime_config_tests COMMAND runtime_config_tests)

add_executable(latency_stats_tests
    src/common/latency_stats.cpp
    tests/latency_stats_tests.cpp
)
target_include_directories(latency_stats_tests PRIVATE src)
target_compile_options(latency_stats_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME latency_stats_tests COMMAND latency_stats_tests)

add_executable(output_processing_tests
    src/video/output_processor.cpp
    tests/output_processing_tests.cpp
)
target_include_directories(output_processing_tests PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(output_processing_tests PRIVATE ${FFMPEG_LINK_LIBS})
target_compile_options(output_processing_tests PRIVATE ${TEST_WARNING_FLAGS})
add_test(NAME output_processing_tests COMMAND output_processing_tests)

if(Python3_Interpreter_FOUND)
    add_test(NAME weaknet_acceptance_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/weaknet_acceptance_tests.py
    )
else()
    message(STATUS
        "Python3 interpreter not found; skipping weaknet_acceptance_tests. "
        "Set Python3_EXECUTABLE to an Anaconda python.exe if you want to enable them."
    )
endif()
