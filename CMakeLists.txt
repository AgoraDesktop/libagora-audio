# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: LGPL-3.0-or-later

#===----------------------------------------------------------------------===#
#
# Agora's build for Core Audio (libagora-audio).
#
# **THE API IS macOS 10.12'S, AND THE MANIFEST IS WHAT KEEPS IT SO.** `tests/api/manifest-10.12.txt` is
# generated from the reference SDK by `tools/api-manifest.py` and committed; the tests include it in
# spirit — every size, offset, constant value and prototype they assert comes from those numbers. The
# SDK itself is never part of a build, and `tools/refshim/` only exists so the generator can run.
#
# ONE LIBRARY, THREE DOORS LATER: `CoreAudio/` today (AudioToolbox/ and AudioUnit/ arrive with the
# phases that implement them), installed as directories under `include/`, so a client writes
# `#include <CoreAudio/CoreAudioTypes.h>` exactly as it does on macOS.
#
# WHAT THIS LINKS IS CORE FOUNDATION AND LIBC. No audio library is named here yet: the null device and
# the real backends arrive with the HAL (plan §9), and the moment one does, it is a PRIVATE link so
# that no type from PipeWire or ALSA can reach a public header.
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.24)

# C now; OBJC is added with AUv3 (10.12's `AUAudioUnit` imports Foundation), which is phase 5.
project(AgoraAudio LANGUAGES C)

# **EVERY LIBRARY FINDS ITS NEIGHBOURS, AND THE INSTALLED BUNDLE NEEDS NO ENVIRONMENT.** A `DT_RUNPATH`
# is not inherited by a dependency's own dependencies, so `$ORIGIN` is what makes a relocatable install
# genuinely relocatable. Same reasoning, and the same measured failure, as libagora-watch's.
set(CMAKE_INSTALL_RPATH "\$ORIGIN")

include(GNUInstallDirs)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(CoreFoundation CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Include shim: consumers write <CoreAudio/...>, as on macOS.
# ---------------------------------------------------------------------------
set(_agora_include_dir "${CMAKE_BINARY_DIR}/agora-include")
file(REMOVE_RECURSE "${_agora_include_dir}")
file(MAKE_DIRECTORY "${_agora_include_dir}")
foreach(_door CoreAudio AudioToolbox)
  file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${_door}" "${_agora_include_dir}/${_door}" SYMBOLIC)
endforeach()

add_library(Audio SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/HostTime.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioFormat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioResampler.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioConverter.c")

set_target_properties(Audio PROPERTIES OUTPUT_NAME agora-audio)

target_include_directories(Audio
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC
        "$<BUILD_INTERFACE:${_agora_include_dir}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Core Foundation is PUBLIC: `OSStatus`, `SInt32` and `Boolean` come from it, and a client of these
# headers has to have them. Threads is PRIVATE and not used yet — the HAL owns its threads in phase 2.
target_link_libraries(Audio
    PUBLIC
        CoreFoundation::CoreFoundation
    PRIVATE
        Threads::Threads)

# **THE TESTS ARE A DEVELOPMENT ARTEFACT, AND A RELEASE DOES NOT BUILD THEM.** `-DBUILD_TESTING=OFF`,
# which the release driver passes, configures and compiles no test executable at all.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(DIRECTORY CoreAudio AudioToolbox DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*_internal.h" EXCLUDE)
install(TARGETS Audio
    EXPORT AgoraAudioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CMakePackageConfigHelpers)

install(EXPORT AgoraAudioTargets
    FILE AgoraAudioTargets.cmake
    NAMESPACE AgoraAudio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraAudio)

configure_package_config_file(cmake/AgoraAudioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraAudioConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraAudio)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraAudioConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraAudioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraAudioConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraAudio)
