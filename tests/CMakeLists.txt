# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# libagora-audio's own suites.
#
# THE API GATES ARE COMPILE-TIME, SO THEY COST NOTHING AND CANNOT DRIFT QUIETLY: `api-layout` asserts
# the sizes and offsets recorded in `api/manifest-10.12.txt`, `api-macros` the constant values and the
# flag-decoding macro, and `api-cxx` that the doors survive a C++ compiler. A wrong layout is silent
# corruption in a client, not a compile error, which is why it is a test and not a promise.

# C++ only for the suites: the library is C, and a release configures no test target at all.
enable_language(CXX)

add_executable(api_layout api/layout.c)
target_link_libraries(api_layout PRIVATE Audio)
add_test(NAME api-layout COMMAND api_layout)

add_executable(api_macros api/macros.c)
target_link_libraries(api_macros PRIVATE Audio)
add_test(NAME api-macros COMMAND api_macros)

add_executable(api_cxx api/compile-cxx.cpp)
target_link_libraries(api_cxx PRIVATE Audio)
set_target_properties(api_cxx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
add_test(NAME api-cxx COMMAND api_cxx)

add_executable(api_prototypes api/prototypes.c)
target_link_libraries(api_prototypes PRIVATE Audio)
add_test(NAME api-prototypes COMMAND api_prototypes)

add_executable(format format.c)
target_link_libraries(format PRIVATE Audio)
add_test(NAME format COMMAND format)

add_executable(converter converter.c)
target_link_libraries(converter PRIVATE Audio m)
add_test(NAME converter COMMAND converter)

add_executable(file file.c)
target_link_libraries(file PRIVATE Audio m)
add_test(NAME file COMMAND file)

add_executable(hal hal.c)
target_link_libraries(hal PRIVATE Audio m dispatch)
# The suite asserts on the mixer's own output, which is a test hook in the library's private header.
target_include_directories(hal PRIVATE "${CMAKE_SOURCE_DIR}")
add_test(NAME hal COMMAND hal)
# The same suite against the second backend, on ALSA's null PCM: a real `snd_pcm_open` and a real write loop,
# with no sound card involved. The pacing assertion is the only part it does not make.
if(ALSA_FOUND)
    add_test(NAME hal-alsa COMMAND hal alsa)
endif()

add_executable(queue queue.c)
target_link_libraries(queue PRIVATE Audio m dispatch)
target_include_directories(queue PRIVATE "${CMAKE_SOURCE_DIR}")
add_test(NAME queue COMMAND queue)

add_executable(unit unit.c)
target_link_libraries(unit PRIVATE Audio m dispatch)
target_include_directories(unit PRIVATE "${CMAKE_SOURCE_DIR}")
add_test(NAME unit COMMAND unit)

add_executable(hosttime hosttime.c)
target_link_libraries(hosttime PRIVATE Audio)
add_test(NAME hosttime COMMAND hosttime)

# The suites load the library from the build tree, not from an installed prefix: the runpath rules
# that make an installed bundle relocatable do not apply to a test binary, and a stale copy in the
# prefix is exactly what makes a test measure the wrong code.
set(_agora_runtime_dirs
    "$<TARGET_FILE_DIR:Audio>"
    "$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
list(REMOVE_DUPLICATES _agora_runtime_dirs)
string(REPLACE ";" ":" _agora_ld_path "${_agora_runtime_dirs}")
set(_agora_audio_tests api-layout api-macros api-cxx api-prototypes format converter file hal queue unit hosttime)
if(ALSA_FOUND)
    list(APPEND _agora_audio_tests hal-alsa)
endif()
set_tests_properties(${_agora_audio_tests} PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_agora_ld_path}")
