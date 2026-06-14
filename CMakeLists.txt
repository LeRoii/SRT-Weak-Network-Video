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

function(first_existing_target output_var)
    set(found_target "")
    foreach(candidate IN LISTS ARGN)
        if(TARGET "${candidate}")
            set(found_target "${candidate}")
            break()
        endif()
    endforeach()
    set("${output_var}" "${found_target}" PARENT_SCOPE)
endfunction()

function(add_imported_library target_name include_dir library_path)
    if(NOT TARGET "${target_name}")
        add_library("${target_name}" UNKNOWN IMPORTED)
        set_target_properties("${target_name}" PROPERTIES
            IMPORTED_LOCATION "${library_path}"
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}"
        )
    endif()
endfunction()

function(target_project_warnings target_name)
    if(MSVC)
        target_compile_options("${target_name}" PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options("${target_name}" PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

function(target_test_compile_options target_name)
    if(MSVC)
        target_compile_options("${target_name}" PRIVATE
            /UNDEBUG
            /W4
            /permissive-
            /EHsc
            /utf-8
        )
    else()
        target_compile_options("${target_name}" PRIVATE
            -UNDEBUG
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

if(WIN32)
    find_package(FFmpeg CONFIG QUIET)
    find_package(FFMPEG CONFIG QUIET)
    find_package(unofficial-ffmpeg CONFIG QUIET)

    first_existing_target(ffmpeg_all_target
        FFMPEG::FFMPEG
        FFmpeg::FFmpeg
        FFmpeg::ffmpeg
        unofficial::ffmpeg::ffmpeg
    )
    set(FFMPEG_TARGETS)
    if(ffmpeg_all_target)
        set(FFMPEG_TARGETS "${ffmpeg_all_target}")
        set(FFMPEG_TARGETS_FOUND TRUE)
    else()
        set(FFMPEG_TARGETS_FOUND TRUE)
        foreach(component IN ITEMS avcodec avformat avutil swscale)
            first_existing_target(ffmpeg_component_target
                "FFmpeg::${component}"
                "FFMPEG::${component}"
                "unofficial::ffmpeg::${component}"
            )
            if(ffmpeg_component_target)
                list(APPEND FFMPEG_TARGETS "${ffmpeg_component_target}")
            else()
                set(FFMPEG_TARGETS_FOUND FALSE)
            endif()
        endforeach()
    endif()

    if(NOT FFMPEG_TARGETS_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
                libavcodec
                libavformat
                libavutil
                libswscale
            )
        endif()
        if(TARGET PkgConfig::FFMPEG)
            set(FFMPEG_TARGETS PkgConfig::FFMPEG)
            set(FFMPEG_TARGETS_FOUND TRUE)
        endif()
    endif()

    if(NOT FFMPEG_TARGETS_FOUND)
        find_path(FFMPEG_INCLUDE_DIR NAMES libavcodec/avcodec.h)
        find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec libavcodec)
        find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat libavformat)
        find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil libavutil)
        find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale libswscale)
        if(NOT FFMPEG_INCLUDE_DIR OR
           NOT FFMPEG_AVCODEC_LIBRARY OR
           NOT FFMPEG_AVFORMAT_LIBRARY OR
           NOT FFMPEG_AVUTIL_LIBRARY OR
           NOT FFMPEG_SWSCALE_LIBRARY)
            message(FATAL_ERROR
                "FFmpeg was not found. On Windows, install FFmpeg development "
                "libraries with vcpkg and configure CMake with the vcpkg "
                "toolchain file, or set CMAKE_PREFIX_PATH to your FFmpeg SDK."
            )
        endif()
        add_imported_library(local::ffmpeg_avcodec
            "${FFMPEG_INCLUDE_DIR}" "${FFMPEG_AVCODEC_LIBRARY}")
        add_imported_library(local::ffmpeg_avformat
            "${FFMPEG_INCLUDE_DIR}" "${FFMPEG_AVFORMAT_LIBRARY}")
        add_imported_library(local::ffmpeg_avutil
            "${FFMPEG_INCLUDE_DIR}" "${FFMPEG_AVUTIL_LIBRARY}")
        add_imported_library(local::ffmpeg_swscale
            "${FFMPEG_INCLUDE_DIR}" "${FFMPEG_SWSCALE_LIBRARY}")
        set(FFMPEG_TARGETS
            local::ffmpeg_avcodec
            local::ffmpeg_avformat
            local::ffmpeg_avutil
            local::ffmpeg_swscale
        )
    endif()

    find_package(SDL2 CONFIG QUIET)
    first_existing_target(SDL2_TARGET
        SDL2::SDL2
        SDL2::SDL2-static
        unofficial::sdl2::SDL2
    )
    if(NOT SDL2_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
        endif()
        if(TARGET PkgConfig::SDL2)
            set(SDL2_TARGET PkgConfig::SDL2)
        endif()
    endif()
    if(NOT SDL2_TARGET)
        find_path(SDL2_INCLUDE_DIR NAMES SDL2/SDL.h)
        find_library(SDL2_LIBRARY NAMES SDL2 SDL2d)
        if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
            message(FATAL_ERROR
                "SDL2 was not found. On Windows, install SDL2 with vcpkg and "
                "configure CMake with the vcpkg toolchain file, or set "
                "CMAKE_PREFIX_PATH to your SDL2 SDK."
            )
        endif()
        add_imported_library(local::sdl2 "${SDL2_INCLUDE_DIR}" "${SDL2_LIBRARY}")
        set(SDL2_TARGET local::sdl2)
    endif()

    find_package(unofficial-isal CONFIG QUIET)
    first_existing_target(ISAL_TARGET
        unofficial::isal::isal
        unofficial::isa-l::isa-l
        isal::isal
        ISA-L::ISA-L
    )
    if(NOT ISAL_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(ISAL QUIET IMPORTED_TARGET libisal)
        endif()
        if(TARGET PkgConfig::ISAL)
            set(ISAL_TARGET PkgConfig::ISAL)
        endif()
    endif()
    if(NOT ISAL_TARGET)
        find_path(ISAL_INCLUDE_DIR NAMES isa-l/erasure_code.h)
        find_library(ISAL_LIBRARY NAMES isal isa-l libisal)
        if(NOT ISAL_INCLUDE_DIR OR NOT ISAL_LIBRARY)
            message(FATAL_ERROR
                "ISA-L was not found. On Windows, install isa-l with vcpkg and "
                "configure CMake with the vcpkg toolchain file, or set "
                "CMAKE_PREFIX_PATH to your ISA-L SDK."
            )
        endif()
        add_imported_library(local::isal "${ISAL_INCLUDE_DIR}" "${ISAL_LIBRARY}")
        set(ISAL_TARGET local::isal)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswscale
    )
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(ISAL REQUIRED IMPORTED_TARGET libisal)
    set(FFMPEG_TARGETS PkgConfig::FFMPEG)
    set(SDL2_TARGET PkgConfig::SDL2)
    set(ISAL_TARGET PkgConfig::ISAL)
endif()

set(COMMON_SOURCES
    src/common/adaptation.cpp
    src/common/latency_stats.cpp
    src/common/network_quality.cpp
    src/common/runtime_config.cpp
    src/common/utils.cpp
    src/fec/reed_solomon.cpp
    src/protocol/frame_assembler.cpp
    src/protocol/messages.cpp
    src/transport/srt_transport.cpp
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
    src/video/video_source_reader.cpp
    src/video/video_renderer.cpp
)

target_include_directories(srt_weak_video PRIVATE
    src
    ${srt_SOURCE_DIR}/common
    ${srt_SOURCE_DIR}/srtcore
    ${srt_BINARY_DIR}
)
target_link_libraries(srt_weak_video PRIVATE
    srt_shared
    ${FFMPEG_TARGETS}
    ${SDL2_TARGET}
    ${ISAL_TARGET}
    Threads::Threads
)
if(WIN32)
    target_compile_definitions(srt_weak_video PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        SDL_MAIN_HANDLED
    )
    target_link_libraries(srt_weak_video PRIVATE ws2_32)
endif()
target_project_warnings(srt_weak_video)

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
if(WIN32)
    target_compile_definitions(protocol_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_project_warnings(protocol_tests)
add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(adaptation_tests
    src/common/adaptation.cpp
    tests/adaptation_tests.cpp
)
target_include_directories(adaptation_tests PRIVATE src)
target_test_compile_options(adaptation_tests)
add_test(NAME adaptation_tests COMMAND adaptation_tests)

add_executable(network_quality_tests
    src/common/network_quality.cpp
    tests/network_quality_tests.cpp
)
target_include_directories(network_quality_tests PRIVATE src)
target_test_compile_options(network_quality_tests)
add_test(NAME network_quality_tests COMMAND network_quality_tests)

add_executable(runtime_config_tests
    src/common/runtime_config.cpp
    tests/runtime_config_tests.cpp
)
target_include_directories(runtime_config_tests PRIVATE src)
target_test_compile_options(runtime_config_tests)
add_test(NAME runtime_config_tests COMMAND runtime_config_tests)

add_executable(latency_stats_tests
    src/common/latency_stats.cpp
    tests/latency_stats_tests.cpp
)
target_include_directories(latency_stats_tests PRIVATE src)
target_test_compile_options(latency_stats_tests)
add_test(NAME latency_stats_tests COMMAND latency_stats_tests)
