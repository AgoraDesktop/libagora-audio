# libagora-audio

**Core Audio for Agora** — the audio framework a Cocoa application expects to find: the host clock, the
formats and their structures, then files, conversion, queues, services, the device layer and the units,
each in the shape macOS 10.12 gives it.

Agora is an installable desktop environment for Linux and the BSDs; see the project README in the parent
directory (`../README.md`). `libagora-audio` is what `NSSound` and `NSBeep` will sit on, and what an
application that plays or records audio links.

**The API is macOS 10.12's, declaration for declaration**, not a design in its spirit. The headers are
written here; the facts they are written from are generated from the reference SDK and committed
(`tests/api/manifest-10.12.txt`); and three compile-time suites fail when a size, an offset, a constant
or a prototype drifts. See [Provenance](#provenance) and `PROVENANCE.md`.

## Status

**Phase 0 — the specification, the skeleton, and the first door. Measured on this machine.**

```
libagora-audio/CoreAudio/
    CoreAudio.h          the umbrella
    CoreAudioTypes.h     the structures, the time stamps, the flag vocabulary, the status codes
    HostTime.h           the host clock
HostTime.c               the host clock over clock_gettime(CLOCK_MONOTONIC)
tools/refshim/           sixty lines of ours, so clang on Linux can parse Apple's headers
tools/api-manifest.py    generates tests/api/manifest-10.12.txt from a MacOSX10.12.sdk
tests/api/               the three identity gates, and the host clock's own suite
```

The device layer (`CoreAudio/AudioHardware.h`), `AudioToolbox/` and `AudioUnit/` arrive with their phases;
`../AUDIO-PLAN.md` §9 is the schedule and §6 the header-by-header commitment.

```
$ readelf -d build/libagora-audio.so | grep -E 'NEEDED|RUNPATH'
  Shared library: [libagora-core.so]
  Shared library: [libc.so.6]
  Library runpath: [$ORIGIN]
```

No audio library is linked yet, and none will be until the HAL: PipeWire, ALSA and sndio are the *device
seam*, not the API, and they stay out of every public header.

## How "identical to 10.12" is kept true

| mechanism | what it catches |
|---|---|
| `tests/api/layout.c` | a size, an alignment or a field offset — `_Static_assert`, from the manifest's numbers |
| `tests/api/macros.c` | a constant's value and the flag-decoding macro, checked by value at run time |
| `tests/api/compile-cxx.cpp` | headers that only compile as C: `extern "C"`, the inline helpers Apple ships |
| `tests/api/manifest-10.12.txt` | the record itself: 13 records, 320 enumeration constants, 24 typedefs, 5 prototypes |

The manifest is generated, never hand-edited:

```sh
MacOSX10.12.sdk=/path/to/MacOSX10.12.sdk          # Apple's text; never committed
tools/api-manifest.py --sdk "$MacOSX10.12.sdk" \
    --out tests/api/manifest-10.12.txt \
    CoreAudio/CoreAudioTypes.h CoreAudio/HostTime.h
```

**Behaviour is graded, and `../AUDIO-PLAN.md` §1.3 says how.** A declaration that exists and refuses with
the documented status is honest; a missing symbol is not. `CoreAudioTypes.h` therefore carries the part of
Apple's tables that something here uses, and the rest is transcribed by the phase that needs it — the
manifest already holds every one of them.

## The suites

`ctest` runs four, none of which needs a sound card, a session server, or anyone listening:

- **`api-layout`** — every public structure's size, alignment and field offsets against the manifest,
  including the two traps (`AudioBufferList` and `AudioChannelLayout` are variable-length: a client
  allocates past their declared single element, so a wrong offset is silent memory corruption).
- **`api-macros`** — `kAudioFormatLinearPCM` is `'lpcm'` and not a number somebody remembered; the layout
  tags are `(label << 16) | count`; `TestAudioFormatNativeEndian` is right for native float, native
  big-endian and non-PCM formats.
- **`api-cxx`** — the same doors under `-std=c++17`, including `CalculateLPCMFlags` and the timestamp
  helpers.
- **`hosttime`** — the clock advances, never goes backwards, reports its own frequency, and the two
  conversions are inverses; a 20 ms sleep is at least 15 ms of host time and less than 5 s.

## Build and run

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
      -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
      -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
cmake --build build
cd build && ctest --output-on-failure
```

`-DBUILD_TESTING=OFF` (what the release driver passes) configures and compiles no test executable at all.

## Provenance

Written for Agora against the macOS 10.12 headers, as a specification rather than a source. No Apple text
is copied into this repository: the SDK is read outside it, and what is committed is a manifest of
measurements — names, sizes, offsets, values — with the declarations written from those. `PROVENANCE.md`
has the argument in full, including why this is different from vendoring and where the line is.

## Licence

LGPL-3.0-or-later for the library, GPL-3.0-or-later for `tests/` — see [`LICENSE`](LICENSE).
