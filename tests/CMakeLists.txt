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

# The Objective-C face has its own gate, because the C one is `_Static_assert` over numbers and this one is
# type compatibility over message sends. `-Werror=objc-method-access` is what makes a removed declaration a
# build failure rather than a warning nobody reads.
add_executable(api_objc api/objc.m)
target_link_libraries(api_objc PRIVATE Audio objc)
target_compile_options(api_objc PRIVATE -Werror=objc-method-access -Werror=incompatible-pointer-types)
add_test(NAME api-objc COMMAND api_objc)

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
# The same suite against the session server, when one is running; 77 means "this machine has no server", and
# ctest reports that as skipped rather than failed.
if(PIPEWIRE_FOUND)
    add_test(NAME hal-pipewire COMMAND hal pipewire)
    set_tests_properties(hal-pipewire PROPERTIES SKIP_RETURN_CODE 77)
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

# The Objective-C door: the same units, measured through their object faces and against the C doors they
# describe. Its assertions come in pairs for that reason.
add_executable(auv3 auv3.m)
target_link_libraries(auv3 PRIVATE Audio m dispatch objc)
target_include_directories(auv3 PRIVATE "${CMAKE_SOURCE_DIR}")
add_test(NAME auv3 COMMAND auv3)

# The suites load the library from the build tree, not from an installed prefix: the runpath rules
# that make an installed bundle relocatable do not apply to a test binary, and a stale copy in the
# prefix is exactly what makes a test measure the wrong code.
set(_agora_runtime_dirs
    "$<TARGET_FILE_DIR:Audio>"
    "$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
list(REMOVE_DUPLICATES _agora_runtime_dirs)
string(REPLACE ";" ":" _agora_ld_path "${_agora_runtime_dirs}")
set(_agora_audio_tests api-layout api-macros api-cxx api-objc api-prototypes format converter file hal queue unit hosttime auv3)
if(ALSA_FOUND)
    list(APPEND _agora_audio_tests hal-alsa)
endif()
if(PIPEWIRE_FOUND)
    list(APPEND _agora_audio_tests hal-pipewire)
endif()
set_tests_properties(${_agora_audio_tests} PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_agora_ld_path}")
