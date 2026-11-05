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

**Phase 2 — the types, the clock, format queries, conversion, the three containers, and the device layer.**
Measured on this machine, on the backend that exists everywhere.

```
libagora-audio/CoreAudio/
    CoreAudio.h          the umbrella
    CoreAudioTypes.h     the structures, the time stamps, the flag vocabulary, the status codes
    HostTime.h           the host clock
    AudioHardwareBase.h  the object model: ids, addresses, selectors, listeners
    AudioHardware.h      the five object functions, the io procs, the deprecated doors
libagora-audio/AudioToolbox/
    AudioToolbox.h       the umbrella
    AudioFormat.h        the format property door
    AudioConverter.h     format and sample-rate conversion
    AudioFile.h          WAV, AIFF and CAF
    ExtendedAudioFile.h  a file that speaks the client's format
    AudioFileStream.h    the same containers, parsed as they arrive
HostTime.c               the host clock over clock_gettime(CLOCK_MONOTONIC)
AudioFormat.c            the four properties answered, and the frame arithmetic behind them
AudioResampler.c         the windowed-sinc, arbitrary-ratio interpolator
AudioConverter.c         the three-step pipeline: float, resample, back out
AudioFile.c              the three containers, read and written here
ExtAudioFile.c           format and rate conversion inside the file door
AudioFileStream.c        incremental parsing, with a listener and a packet callback
AudioHardware.c          the object table, the property switch, the listeners, the mixer
AudioHardware-null.c     one device that exists everywhere, with a real data thread
AudioHardware-alsa.c     the second backend: `snd_pcm_open` on a name, paced by its own writes
AudioHardwareDeprecated.c  the pre-10.7 doors, translated into the object API
tools/refshim/           sixty lines of ours, so clang on Linux can parse Apple's headers
tools/api-manifest.py    generates tests/api/manifest-10.12.txt from a MacOSX10.12.sdk
tests/api/               the four identity gates
```

**Phase 2 — the device layer, addressed through one object API, with two backends: the `null` device that
exists everywhere, and ALSA for a machine that has a sound card.** Still to come: the PipeWire backend behind
the same door, then `AudioQueue`, `AudioServices` and `AudioUnit`.
`../AUDIO-PLAN.md` §9 is the schedule and §6 the header-by-header commitment.

The three containers are ours rather than a library's: WAV is a little-endian chunk list, AIFF is a
big-endian one with an 80-bit sample rate, and CAF is a big-endian chunk list with a 64-bit size. A
compressed format is refused with `kAudioFileUnsupportedDataFormatError`; the codec phase brings the
packets that would make the word interesting.

```
$ readelf -d build/libagora-audio.so | grep -E 'NEEDED|RUNPATH'
  Shared library: [libagora-core.so]
  Shared library: [libc.so.6]
  Library runpath: [$ORIGIN]
```

**Three things the manifest corrected, which is why it exists:** the converter's stream-description
selectors are `'acid'` and `'acod'` (not the `'acic'`/`'acoc'` they look like); the audio file's
unsupported-property error is `'pty?'`, not `'prop'`; and **10.12 has no
`kAudioFilePropertyFileLengthFrames` at all** — for linear PCM the *packet* count is the frame count, and
the constant this repository had invented was deleted rather than implemented.

No audio library is linked yet, and none will be until the HAL: PipeWire, ALSA and sndio are the *device
seam*, not the API, and they stay out of every public header.

## How "identical to 10.12" is kept true

| mechanism | what it catches |
|---|---|
| `tests/api/layout.c` | a size, an alignment or a field offset — `_Static_assert`, from the manifest's numbers |
| `tests/api/macros.c` | a constant's value and the flag-decoding macro, checked by value at run time |
| `tests/api/compile-cxx.cpp` | headers that only compile as C: `extern "C"`, the inline helpers Apple ships |
| `tests/api/prototypes.c` | a changed parameter, a dropped `const`, a `UInt32 *` that became `UInt32` — 69 prototypes through pointers typed from the manifest |
| `tests/api/manifest-10.12.txt` | the record itself: 31 records, 936 enumeration constants, 88 typedefs, 105 prototypes |

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
- **`format`** — the frame arithmetic a non-interleaved description needs (one channel, not all of them),
  24 significant bits in a 4-byte container, the three layout questions, and four refusals by name.
- **`converter`** — int16 → float32 → int16 **bit-exact on every value** (the scale is 2^15 both ways);
  interleaved → non-interleaved keeps its channels; and 44.1 kHz → 48 kHz of a 1 kHz sine measures
  **SNR 85.2 dB**, printed by the suite, with the threshold at the 60 dB a resampler has to clear.
- **`hal`** — on the `null` backend: the default device and its name (a `CFString`), the rate and the quantum,
  the output stream and its float32 stereo format, a volume write that reads back, a listener that fires once
  and then goes quiet when removed, the deprecated door answering the same value, two io procs calling at the
  device's rate (**12 000 frames in 250 ms, ±20%**), the mixer summing them to **0.375**, mute silencing it,
  stop stopping it, and a sample time translating to a host time. **The same suite runs a second time on the
  ALSA backend** against its `null` PCM — a real `snd_pcm_open`, a real write loop, no sound card — where
  everything holds except the pacing assertion, which is a fact about the plugin rather than about this
  library.
- **`file`** — WAV, AIFF and CAF each written, closed, reopened and compared with `memcmp`: **bit-exact**,
  with the byte count, packet count, duration and data format checked separately; then `ExtAudioFile`
  converting int16 → float32 → int16 exactly, and a 44.1 kHz client writing a 48 kHz file, which lands
  within 2% of the expected frame count; then the same WAV fed to `AudioFileStream` **seven bytes at a
  time**, which announces the format and readiness once each and hands over all 2000 packets; then four
  refusals by name.

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
