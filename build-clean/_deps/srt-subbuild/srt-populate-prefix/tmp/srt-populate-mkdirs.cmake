# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-src"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-build"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/tmp"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/src/srt-populate-stamp"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/src"
  "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/src/srt-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/src/srt-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/Code/C++/SRT-Weak-Network-Video/build-clean/_deps/srt-subbuild/srt-populate-prefix/src/srt-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
