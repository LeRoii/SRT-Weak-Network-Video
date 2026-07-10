# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.29.0-rc2)

# Reject any attempt to use a toolchain file. We must not use one because
# we could be downloading it here. If the CMAKE_TOOLCHAIN_FILE environment
# variable is set, the cache variable will have been initialized from it.
unset(CMAKE_TOOLCHAIN_FILE CACHE)
unset(ENV{CMAKE_TOOLCHAIN_FILE})

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(srt-populate NONE)


# Pass through things we've already detected in the main project to avoid
# paying the cost of redetecting them again in ExternalProject_Add()
set(GIT_EXECUTABLE [==[D:/Program Files/Git/cmd/git.exe]==])
set(GIT_VERSION_STRING [==[2.44.0.windows.1]==])
set_property(GLOBAL PROPERTY _CMAKE_FindGit_GIT_EXECUTABLE_VERSION
  [==[D:/Program Files/Git/cmd/git.exe;2.44.0.windows.1]==]
)


include(ExternalProject)
ExternalProject_Add(srt-populate
                     "UPDATE_DISCONNECTED" "False" "GIT_REPOSITORY" "https://github.com/Haivision/srt.git" "EXTERNALPROJECT_INTERNAL_ARGUMENT_SEPARATOR" "GIT_TAG" "v1.5.5" "GIT_SHALLOW" "TRUE" "PATCH_COMMAND" "D:/cmake/bin/cmake.exe" "-DSRT_SOURCE_DIR=<SOURCE_DIR>" "-P" "D:/Code/C++/SRT-Weak-Network-Video/cmake/patch_srt.cmake"
                    SOURCE_DIR          "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-src"
                    BINARY_DIR          "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
                    USES_TERMINAL_PATCH     YES
)


