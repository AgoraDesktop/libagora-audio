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

# **THE FIRST COMPRESSED FORMAT, AND THE FIXTURES ARE AN ENCODER'S OUTPUT.** `ffmpeg` writes two tones while the suite
# is configured — 44.1 kHz stereo MPEG-1 and 22.05 kHz mono MPEG-2, whose frames hold 1152 and 576 samples — because an
# MP3 is an encoder's output and no checked-in blob would explain itself. Two questions are asked that a frame count
# alone cannot answer: whether the container's own headers match the file, and whether the samples are the tone that
# went in.
#
# **THE TEST IS REGISTERED WHATEVER HAPPENS, AND SKIPS WHEN IT HAS NOTHING TO READ.** A suite that is not built at all
# is a suite that is missing without saying so; a `Skipped` line in ctest's own report is the honest version, and it is
# the same shape `hal-sndio` uses for a server that is not running. The same skip covers a build with
# `-DAGORA_AUDIO_WITH_MPG123=OFF`, where the container still reads and there is no decoder behind it.
add_executable(mp3 mp3.c)
target_link_libraries(mp3 PRIVATE Audio CoreFoundation::CoreFoundation m)
set(_mp3_fixtures)
find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
    set(_mpeg1 "${CMAKE_CURRENT_BINARY_DIR}/tone-mpeg1-stereo.mp3")
    set(_mpeg2 "${CMAKE_CURRENT_BINARY_DIR}/tone-mpeg2-mono.mp3")
    set(_mp3_fixtures "${_mpeg1}" "${_mpeg2}")

    if(NOT EXISTS "${_mpeg1}")
        execute_process(
            COMMAND "${FFMPEG_EXECUTABLE}" -loglevel error -y -f lavfi
                    -i "sine=frequency=440:duration=3" -ar 44100 -ac 2 -b:a 192k "${_mpeg1}"
            RESULT_VARIABLE _mp3_status)
        if(NOT _mp3_status EQUAL 0)
            message(WARNING "libagora-audio: ffmpeg could not write ${_mpeg1}; the MP3 suite will skip")
            file(REMOVE "${_mpeg1}")
        endif()
    endif()
    if(NOT EXISTS "${_mpeg2}")
        execute_process(
            COMMAND "${FFMPEG_EXECUTABLE}" -loglevel error -y -f lavfi
                    -i "sine=frequency=330:duration=2" -ar 22050 -ac 1 -q:a 4 "${_mpeg2}"
            RESULT_VARIABLE _mp3_status)
        if(NOT _mp3_status EQUAL 0)
            message(WARNING "libagora-audio: ffmpeg could not write ${_mpeg2}; the MP3 suite will skip")
            file(REMOVE "${_mpeg2}")
        endif()
    endif()
else()
    message(STATUS "libagora-audio: no ffmpeg, so the MP3 suite will report a skip")
endif()
add_test(NAME mp3 COMMAND mp3 ${_mp3_fixtures})
set_tests_properties(mp3 PROPERTIES SKIP_RETURN_CODE 77)

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
# sndio needs a `sndiod`; on a Linux box with libsndio-dev and no server this leg SKIPS, which is the honest
# report for a backend that is written and compiled but not measured here.
if(SNDIO_FOUND)
    add_test(NAME hal-sndio COMMAND hal sndio)
    set_tests_properties(hal-sndio PROPERTIES SKIP_RETURN_CODE 77)
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
if(TARGET mp3)
    list(APPEND _agora_audio_tests mp3)
endif()
if(ALSA_FOUND)
    list(APPEND _agora_audio_tests hal-alsa)
endif()
if(PIPEWIRE_FOUND)
    list(APPEND _agora_audio_tests hal-pipewire)
endif()
if(SNDIO_FOUND)
    list(APPEND _agora_audio_tests hal-sndio)
endif()
set_tests_properties(${_agora_audio_tests} PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_agora_ld_path}")
