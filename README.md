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

**Phase 3 and 5 complete — the types, the clock, format queries, conversion, the three containers, the device
layer on four backends (`null`, ALSA, PipeWire and sndio), the units and their graph, and AUv3's object surface
over them.** Measured on this machine: the null and ALSA legs of the HAL suite, the PipeWire leg against the
session server that is actually running here, and 24 576 frames of a 0.5 tone that server pulled in half a
second. **`sndio` compiles and links here and is not measured here** — it is the BSDs' server and this machine
has no `sndiod`; the label, and what would lift it, are in the phase note below.

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
    AudioQueue.h         the buffered model, both ways: playback and capture
    AudioServices.h      the system sounds, and a generated alert
    AudioComponent.h     what a unit is before it is an instance
    AudioUnit.h          scopes, elements, properties and the pull
    AudioOutputUnit.h    starting a unit that emits audio
    AUGraph.h            units wired to each other
    AUParameters.h       a parameter tree: nodes, groups, leaves, observers
    AUAudioUnit.h        a unit as an object: busses, a render block, the hardware doors
    AUAudioUnit_internal.h  what a bus asks its unit (not installed)
HostTime.c               the host clock over clock_gettime(CLOCK_MONOTONIC)
AudioFormat.c            the four properties answered, and the frame arithmetic behind them
AudioResampler.c         the windowed-sinc, arbitrary-ratio interpolator
AudioConverter.c         the three-step pipeline: float, resample, back out
AudioFile.c              the three containers, read and written here
ExtAudioFile.c           format and rate conversion inside the file door
AudioFileStream.c        incremental parsing, with a listener and a packet callback
AudioHardware.c          the object table, the property switch, the listeners, the mixer
AudioHardware-null.c     one device that exists everywhere, with a real data thread
AudioHardware-alsa.c     the second backend: `snd_pcm_open` on a name, paced by its own writes,
                         and recording from the same name when that PCM can record
AudioHardware-pipewire.c the third: the session's own server, its sinks, and two streams per started
                         device — playback to a sink, capture from the session's default source
AudioHardware-sndio.c    the fourth: the BSDs' sound server (written and compiled; not measured here),
                         with a second handle for recording
AudioHardwareDeprecated.c  the pre-10.7 doors, translated into the object API
AudioQueue.c             buffers in, played frames out — and the same object filled from the device
AudioServices.c          a file and a number; the alert is generated, not vendored
AudioUnit.c              the registry of three, and the three units behind it
AudioGraph.c             nodes, connections, and the pull that goes down them
AUParameters.m           the tree built from the C parameter table, and the write-through observers
AUAudioUnit.m            the object face of a C unit: busses, blocks, the tree, the hardware doors
tools/refshim/           a few hundred lines of ours, so clang on Linux can parse Apple's headers
                         (Foundation's declarations are there only for the AUv3 parse)
tools/api-manifest.py    generates tests/api/manifest-10.12.txt from a MacOSX10.12.sdk
tests/api/               the five identity gates
```

**Phase 5b — AUv3, in process.** `AUAudioUnit`, `AUAudioUnitBus`, `AUAudioUnitBusArray`,
`AUAudioUnitPreset` and the four `AUParameter` classes are `NSObject`s **over** the C units in `AudioUnit.c`,
which is Apple's own answer for a v2 unit (their name for it is `AUAudioUnitV2Bridge`): a bus is
`kAudioUnitProperty_ElementCount` and `StreamFormat`, `-renderBlock` is `AudioUnitRender`,
`-startHardwareAndReturnError:` is `AudioOutputUnitStart`, and the parameter tree is
`kAudioUnitProperty_ParameterList` plus `ParameterInfo`. Nothing is implemented twice, so nothing can
disagree.

Three things about this door are **not** Apple's, and each is stated where it is declared:

- a bus's format is an `AudioStreamBasicDescription`, not an `AVAudioFormat *` — formats live in
  AVFoundation on macOS and this library has no AVFoundation (`../AUDIO-PLAN.md` §5);
- `AUParameterAddress` encodes the C coordinates — `scope << 32 | element << 16 | id` — because a v2
  parameter is three numbers and an address is one;
- the parameter classes do not claim `NSSecureCoding`, and `+instantiateWithComponentDescription:…` reports
  an error instead of loading anything: the registry is three built-ins and there is no plug-in ABI, so
  in-process is the only instantiation there is. App-extension hosting (`AUAudioUnitFactory`) is out of
  scope, and the class is not declared — a name that cannot be loaded is worse than an absent one.

**Phase 3 — PipeWire.** `AudioHardware-pipewire.c` connects once, freezes what the registry says are sinks
(`media.class = Audio/Sink`) into a device table after two round trips, and puts the server's default sink first
so that `kAudioHardwarePropertyDefaultOutputDevice` — the first output device in this HAL — *is* the default.
Each started device gets one `pw_stream` aimed at its sink by `target.object`; the process callback asks the
mixer for the frames the server's buffer holds and hands them back. It is deliberately **not** the realtime
callback (`PW_STREAM_FLAG_RT_PROCESS`): the mixer takes a lock, so a late buffer is the smaller problem. The
node shows up in `pw-dump` as `Stream/Output/Audio` named `agora-audio-output`, routed to the sink it was given.

**Phase 7 — the first decoder: MPEG Layer III, and it is a SYSTEM LIBRARY.** `libmpg123` 1.32.10 (LGPL-2.1) is linked
PRIVATE behind `-DAGORA_AUDIO_WITH_MPG123=AUTO|ON|OFF`, so a build without it refuses an MP3 by name and one with it
plays the file. The split of work is the important part: **`AudioFile.c` walks the stream's own frame headers into a
packet table** — there is no length field in an MP3, so the file's duration is a measurement, not a division — and
**`ExtAudioFile.c` decodes the packets into the client's format**, because a decoder keeps state across frames and a
converter is a ring. `AudioConverter` therefore still refuses MPEG input, exactly as its suite asserts. MEASURED: a
three-second 44.1 kHz stereo tone decodes to 132 300 frames at the tone's own frequency, which is what `ffmpeg`
produces for the same file (`tests/mp3.c`).

**A seek is frame-accurate, and that is the finest anyone can be in this format.** MPEG's bit reservoir makes frame
N depend on the frames before it, so `ExtAudioFileSeek` restarts the decoder at the frame that contains the sample
asked for and drops the samples before it inside that frame; a seek back to the start is therefore bit-exact, while a
seek into the middle is exact to the frame and not to the sample, because the encoder's delay is declared only in the
stream's first frame. That is also why the container's packet count is *larger* than the decoded frame count: the
encoder pads to whole frames and libmpg123 trims the padding it declares. Both numbers are asserted, separately, by
`tests/mp3.c`.

**The incremental door reads MPEG too, and it is the door that needed the packet descriptions most.** A frame's length
is in its own header, so a streamed MP3 reports every packet with an `AudioStreamPacketDescription` — where it starts,
its size and its 1152 or 576 samples — where a PCM stream's packets have none. Fed a byte at a time, seven bytes at a
time or a buffer at a time, `tests/mp3.c` compares every packet against the file door's: 117 of 117 and 80 of 80,
byte for byte, with the audio's offset correctly past ffmpeg's own tag and past a synthetic one whose 200 bytes of
0xFF filler are the sync words a naive walker would index as audio.

**A stream also knows how long it is, as soon as it has seen one frame.** The first frame can carry a Xing or Info
tag — the same tag, spelled for a variable- or constant-bitrate file — holding a frame count, a byte count and a
hundred-entry seek table, and `AudioMpeg.c` reads it out from after the side information, whose size depends on the
version and the channel mode. From the first 4 KiB of the file the door answers `AudioDataPacketCount` and
`AudioDataByteCount` exactly, and `AudioFileStreamSeek` returns a byte offset: exact for packet zero, and an
interpolation of the table's hundredths otherwise, flagged `kAudioFileStreamSeekFlag_OffsetIsEstimated` because that is
what it is. A stream whose first frame is not the tag — a file cut out of a longer one — has no table, and the door says
so: `ValueUnknown` for the count and `kAudioFileStreamError_IllegalOperation` for the seek.

**And the length a client reads is the audio's, not the container's.** The same tag carries the encoder's delay and
padding, so `kAudioFilePropertyPacketTableInfo` answers three numbers that add up — valid, priming, remainder — and
`kExtAudioFileProperty_FileLengthFrames` reports the first. A three-second file is 132 300 frames, which is exactly what
the decoder produces and what `ffmpeg` reports, where the packets hold 134 784. The frame carrying the tag counts as
priming: it is the encoder's declaration, not a bar of music. A file cut out of a longer one declares nothing, so it
keeps the container's count and no table — the honest answer, and the one a trimmed length would guess at.

Writing is refused: there is no encoder, and `AudioFileCreateWithURL` answers `kAudioFileUnsupportedFileTypeError` for
`kAudioFileMP3Type`. The fixtures the suite reads are `ffmpeg`'s output, written when the suite is configured; with no
`ffmpeg` the test reports a skip instead of disappearing.

**Phase 6a — sndio, the BSDs' server: WRITTEN, COMPILED HERE, NOT MEASURED HERE.** `AudioHardware-sndio.c` is
the fourth backend and the one this tree cannot exercise where it is developed: `libsndio-dev` 1.10.0 is
installed on this machine, so the file is compiled and linked against the real header and library and its
`hal-sndio` leg runs and **skips (exit 77)** — no `sndiod` is listening. What a BSD run would measure is what a
real server does with the parameters: the sample format it grants (`sio_getpar` after `sio_setpar`, refused by
name rather than played as noise when it is not 32-bit signed native-endian), the buffer it paces with, and
whether `sio_write`'s blocking back-pressure behaves as documented. Until then the README, the CMake line and
`AUDIO-PLAN.md` §5 all say "written, not measured" rather than implying the others are the same.

sndio has **no float format** — `struct sio_par` describes signed or unsigned integers of 8, 16, 24 or 32 bits —
so the driver converts the mixer's interleaved float32 to 32-bit signed in its own thread, clamping rather than
wrapping at the rails. That is the one conversion the other three backends do not need, and it is the
interface's, not a choice.

**A queue also has a timeline, and the system sounds overlap.** `AudioQueueCreateTimeline` is a remembered moment —
what the queue's time base was at the caller's last look — so `AudioQueueGetCurrentTime` can answer the one question a
caller mixing its own clock needs: whether a start, a stop or a reset broke the time since then. It is reported once,
not on every look. `AudioServicesPlaySystemSound` keeps a pool of four queues, so a second alert does not restart the
first, and a completion block is called when the buffer comes back from the queue rather than before a sample has been
heard.

**And a queue can render without a device.** `AudioQueueOfflineRender` consumes the queued buffers into the caller's
buffer in the client's format, hands each finished buffer back through the same callback the io proc uses, and fills
what is left with silence — the path a caller writes a file through, and the one that makes the callback path testable
on a machine with no sound card at all.

**A tap is what made the render path stage its source.** A processing tap sees the queue's *decoded* audio in the
client's own format, and the device path used to convert straight out of the caller's buffers into the device's — so
there was never a moment when the audio existed in one piece, in the format the client asked for, for a tap to look
at. The source is now gathered into a short FIFO in the client's format (the *stage*), the tap runs on that, and the
conversion, the channel mapping and the volume are the tail of the pipeline after it. `AudioQueueProcessingTapNew`
takes one tap per queue and only on a stopped one; the tap pulls the source with
`AudioQueueProcessingTapGetSourceAudio`, may process it in place (the pointers it is given point into the stage), and
answers with `AudioQueueProcessingTapGetQueueTime` — which is only answerable after the pull, and only from inside the
callback. `kAudioQueueProcessingTap_StartOfStream` and `_EndOfStream` say when the source is discontinuous and when it
has run out, and a **siphon** is shown the source without pulling it and without changing what the device plays. The
only thing the flags do not buy here is a difference between `PreEffects` and `PostEffects`: this queue has no effects
between them, so both hear the same audio and only the flag they are reported under differs.

**And a queue records, which is the same object running backwards.** `AudioQueueNewInput` enqueues *empty* buffers,
fills them from the default input device and hands them back through the caller's callback: the device's channels are
mapped to the client's (a mono client hears the sum of two channels over two), the rate and the sample type are
converted the *other* way (`AudioConverterNew(device, client)` rather than `(client, device)`), and the tap runs on the
cycle's client-format audio before the caller's buffer does — so a tap on a recording works and its
`AudioQueueProcessingTapGetQueueTime` refuses with `kAudioQueueErr_InvalidTapType`, because nothing is waiting to be
played. A draining stop returns at once: a capture queue has nothing to play out. **It is also the client that asks the
device for its input** (`fn_agora_device_set_input_wanted`), which is why a queue that only plays never opens one. A capture queue's volume parameter is
stored and not applied, since a recording has no playback gain.

**The HAL grew an input side to make that testable.** `fn_agora_device_render` now takes the cycle's capture as well —
with **its own frame count**, because a session server's two directions are negotiated separately — and hands it to every io proc as
its `inInputData` (read-only: it is the microphone, and one buffer belongs to the whole cycle).

**AND A DRIVER OPENS ITS CAPTURE SIDE ONLY WHEN SOMEBODY ASKED.** `AudioDeviceStart` is what opens a device, and a
playback client is not a reason to hold a microphone: `fn_agora_device_set_input_wanted` is the HAL's one flag for it,
set by an input queue before it starts the device, and the driver seam's `start(id, want_input)` carries it. A device
already running for playback is **restarted once** so the driver can open the other direction, which is the case a
person hits by pressing record during a sound. MEASURED on this machine's session: a playback queue publishes **no**
`agora-audio-input` node at all, and an input queue publishes one — where every device start used to publish one, and
hold the microphone open for the length of the playback.

The null device — the one every build has — describes an input stream and *hears a ramp*: frame `n` of its
life is `(n % rate) / rate` on the left and half that on the right. A ramp rather than a tone because the question a
capture test asks is not "is this audio" but "are these the frames, in this order, without a gap", and one subtraction
answers it.

**ALSA records from the same PCM it plays to, and asks for the two directions separately.** `fn_alsa_capture_opens` is
a real `snd_pcm_open(SND_PCM_STREAM_CAPTURE)` probe — cached, like the playback one — and the driver opens, configures
and prepares a capture handle with the same format, channels and rate. The data thread reads *before* it asks the mixer
for output, so both directions are one cycle of one device; a short read (an xrun) leaves that cycle without input
rather than handing io procs a buffer half-filled with the last one, and capture is optional throughout — a PCM that
plays but cannot record, or grants another rate, leaves playback alone. MEASURED here: on ALSA's `null` PCM capture
opens and returns the full quantum every cycle with every sample 0.0, because that plugin records silence; on this
machine's `default` PCM it returns **14 cycles of 512 frames in 150 ms** — the card's own clock — with the card's own
samples. That is why the ALSA leg of the suite asserts *delivery* and the null backend asserts *values*.

**PipeWire records from the session's default source, beside the sink it plays to.** A PipeWire device here is a
sink, while playback and capture are different nodes (`Audio/Sink`, `Audio/Source`) — so the device a sink describes
offers a capture stream too, autoconnected to whatever source the session calls default, with the graph's adapter
bringing it to the device's channels and rate — **and only while a client wants it**, because a session server
publishes a node per stream. `has_input` is true only when the server has at least one source node,
which is the same "a description is not an answer" rule the sink list follows. Both streams' callbacks run on the same
thread loop, so the capture callback leaves its quantum in the engine, the playback callback passes it to the mixer
**once** — a source that stalls yields no input rather than the previous buffer for ever — and then clears it.

**THE INPUT HAS ITS OWN LENGTH, AND IT HAD TO.** The graph negotiates the two quanta separately: MEASURED in this
machine's session, the capture stream is given **1024-frame** quanta while the playback stream's buffers are **4096**.
`fn_agora_device_render` therefore takes the input's frame count as well as the output's, and the io proc's
`inInputData` is as long as what actually arrived. Before that, the driver bent one quantum to the other, which either
dropped audio or claimed samples the device never heard.

**sndio records too, with a second handle and the conversion the other way.** `sio_open(name, SIO_REC, 0)` is its own
handle — sndio has no duplex mode — configured with `rchan` where playback uses `pchan`, read *before* the mixer is
asked for output so the two directions are one cycle, and scaled from its 32-bit signed samples back to float32,
because `SIO_PCM` has no float. Capture is optional here as everywhere: a server with no recording device, or one that
grants another rate or channel count, leaves playback alone. **Like the rest of this backend it is written and compiled
here but not measured here** (phase 6a above); what a BSD run would measure is listed there, and now includes the
granted recording parameters in `sio_getpar` and `sio_read`'s blocking back-pressure.

Still to come: `AUGraph` sub-graphs (their Carbon doors are not declared), and `AudioFormat`'s
`kAudioFormatProperty_FormatList` (the constant is in the manifest but its specifier's shape is not, so the door stays
refused rather than guessed). `../AUDIO-PLAN.md` §9 is the schedule and §6 the header-by-header commitment.

The three PCM containers are ours rather than a library's: WAV is a little-endian chunk list, AIFF is a
big-endian one with an 80-bit sample rate, and CAF is a big-endian chunk list with a 64-bit size. MPEG audio is
the fourth container and the first one this door does not invent: an MP3 has no header at all, so the frame
table is built by walking sync words, and the decoder behind it is `libmpg123` (phase 7, above). Every other
compressed format is still refused with `kAudioFileUnsupportedDataFormatError`.

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

ALSA and PipeWire are the *device seam*, not the API: both are PRIVATE links, so no type from either reaches a
public header. Foundation is PUBLIC on the Objective-C doors for the same reason Core Foundation is — a client
naming `AUAudioUnit` needs `NSObject` — and that is why this component sits above `libagora-base` in
`release/components.conf`.

**Which backend a client gets without asking.** `AGORA_AUDIO_DRIVER` names one (`null`, `alsa`, `pipewire`,
`sndio`) and is the test-and-debug door. With no variable set, the library asks in the order a platform has them —
**PipeWire, then sndio, then ALSA** — and uses the null device if none answers. All three probes are real, and for the
same reason: a description is not an answer. PipeWire describes no sinks without its daemon, sndio opens no handle
without its server, and **ALSA's device count is a real `snd_pcm_open`/`close`** — which is what made it safe to put in
this chain at all. Before that probe, "no sound card" and "a sound card" were the same answer from ALSA, so choosing it
automatically would have turned a container into playback errors; MEASURED on this machine, with PipeWire unreachable
(`XDG_RUNTIME_DIR` pointing at an empty directory) the probe reports no ALSA device and the library falls back to
`null`, while with PipeWire reachable the default is the session's own sink. On a desktop session this means a beep
reaches the speakers; in a container it means silence and no error.

```
$ readelf -d build-prefix/lib/libagora-audio.so | grep -E 'NEEDED|RUNPATH'
  Shared library: [libagora-base.so]
  Shared library: [libasound.so.2]
  Shared library: [libpipewire-0.3.so.0]
  Shared library: [libagora-core.so]
  Shared library: [libobjc.so.4]
  Library runpath: [$ORIGIN]
```

## How "identical to 10.12" is kept true

| mechanism | what it catches |
|---|---|
| `tests/api/layout.c` | a size, an alignment or a field offset — `_Static_assert`, from the manifest's numbers |
| `tests/api/macros.c` | a constant's value and the flag-decoding macro, checked by value at run time |
| `tests/api/compile-cxx.cpp` | headers that only compile as C: `extern "C"`, the inline helpers Apple ships |
| `tests/api/prototypes.c` | a changed parameter, a dropped `const`, a `UInt32 *` that became `UInt32` — 69 prototypes through pointers typed from the manifest |
| `tests/api/objc.m` | the Objective-C surface: every class, and each property and method's qualified type, by `__builtin_types_compatible_p`. Built with `-Werror=objc-method-access`, so a *removed* declaration is a build failure too |
| `tests/api/manifest-10.12.txt` | the record itself: 87 records, 1853 enumeration constants, 269 typedefs, **250** prototypes, 9 classes, 43 methods |

The manifest is generated, never hand-edited:

```sh
MacOSX10.12.sdk=/path/to/MacOSX10.12.sdk          # Apple's text; never committed
tools/api-manifest.py --sdk "$MacOSX10.12.sdk" --scratch /tmp/agora-api-stage \
    --out tests/api/manifest-10.12.txt \
    CoreAudio/CoreAudioTypes.h CoreAudio/HostTime.h
```

**`--scratch` is what makes a regeneration a diff about the API.** A record's field types are written down with the
file they were read from, so a `mkdtemp` staging directory rewrites those lines on every run — a diff that says nothing
and hides the ones that do. Naming the directory makes the same input produce the same file, byte for byte.

**Behaviour is graded, and `../AUDIO-PLAN.md` §1.3 says how.** A declaration that exists and refuses with
the documented status is honest; a missing symbol is not. `CoreAudioTypes.h` therefore carries the part of
Apple's tables that something here uses, and the rest is transcribed by the phase that needs it — the
manifest already holds every one of them.

## The suites

`ctest` runs fifteen, none of which needs a sound card, and only one of which needs a session server (and
skips, exit 77, when there is none):

- **`api-layout`** — every public structure's size, alignment and field offsets against the manifest,
  including the two traps (`AudioBufferList` and `AudioChannelLayout` are variable-length: a client
  allocates past their declared single element, so a wrong offset is silent memory corruption).
- **`api-macros`** — `kAudioFormatLinearPCM` is `'lpcm'` and not a number somebody remembered; the layout
  tags are `(label << 16) | count`; `TestAudioFormatNativeEndian` is right for native float, native
  big-endian and non-PCM formats.
- **`api-cxx`** — the same doors under `-std=c++17`, including `CalculateLPCMFlags` and the timestamp
  helpers.
- **`api-objc`** — the class surface, compile-time, against the manifest's `[objc-class]`, `[objc-method]` and
  `[objc-property]` records.
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
  library. **A third leg runs the same suite on `pipewire`**, against the live session server: it asserts what
  a server can promise (a sink exists, the default one is first in the device list, the format is float32 PCM,
  the io procs are called) and skips the numbers the server chooses — rate, quantum, channel count, pacing —
  with the suite naming which of the two it is measuring. **A fourth leg is the same suite on `sndio`**, and on
  this machine it reports the honest thing: `hal-sndio` skips (77), because the backend is compiled against
  libsndio 1.10.0 and no `sndiod` is listening. **The input side is measured on the null and ALSA legs**, because those
  are the two drivers that describe a capture stream: a default input device exists, it has one input-scope stream
  whose `kAudioStreamPropertyDirection` says input, and an io proc on it is handed a cycle of the device's input as
  `inInputData`. On the null backend the samples are the device's ramp — every consecutive pair one step apart, and
  each cycle's first frame continuing from the last cycle's last, so a dropped or duplicated frame fails the assertion
  either inside a buffer or at the seam between two. ALSA and PipeWire choose their own samples (a plugin's silence, a
  room), so those legs measure delivery and say so; `sndio` records the same way but its leg skips here — no `sndiod`
  listens on this machine, and Debian's `sndiod` binary cannot open a device on it either. **The suite also measures
  the demand**: a device started by a playback client hands over **no** input, and the same device started again while
  running — the caller does not stop it — gains the other direction once a client asks, so "nothing arrives without a
  reason" and "it arrives with one" are both assertions rather than intentions.
- **`unit`** — the registry finds a built-in and does not invent a fourth; an output unit with a callback
  renders 0.5 and the device measures it; a converter turns int16 0.25 into float32 0.25; a two-bus mixer sums
  0.25 + 0.125 to **0.375**, and halving one bus's volume halves its contribution; and a graph of mixer → output
  reaches the device with that same 0.375, while a connection that would close a cycle is refused.
- **`queue`** — a buffer of 0.5 played through a refilling callback measures **0.5** at the device, **0.25**
  after the queue's volume is halved, **0.0** after a stop; a buffer enqueued twice is refused by name; **a
  16-bit mono 8 kHz buffer is audible on the stereo device**, which is the channel mapping the queue does because
  the converter — like Apple's — refuses a channel-count change; and the system sounds are audible: a WAV loaded
  by URL, and the **generated** alert tone. Then the tap, measured at both ends: the tap is shown **0.500** and the
  device plays **0.250**, so "the callback ran" and "the audio was processed" are separate claims; a **siphon**
  hears 0.5 and the device still plays 0.5; the source flags report the **start** and, once the queue runs dry, the
  **end**; `GetQueueTime` answers the quantum's frame count; a second tap, a tap on a running queue and a dispose of
  one are each refused by name; `GetSourceAudio` and `GetQueueTime` outside the callback answer
  `kAudioQueueErr_InvalidTapContext`; and `AudioQueueOfflineRender` goes through the tap too, so a caller writing a
  file gets the processed audio. **And it records**: an input queue is filled from the device, the ramp's step is what
  proves the frames are in order, a mono client measures **0.75** of it (the two channels summed and halved rather than
  one of them), a tap on the recording halves that step and is refused a queue time by name, and a draining stop on a
  capture queue returns instead of waiting for its own empty buffers.
- **`auv3`** — every assertion in pairs, because the object is a view of the C unit: the default output unit's
  component description, name, one input bus and one output bus, the bus format **as the unit's own
  `StreamFormat` property reports it**, a format written through the bus arriving at the C door, the maximum
  slice, resources allocated, `-renderBlock` rendering silence through `AudioUnitRender`, hardware start and
  stop on the null device. Then the mixer: two buses, four parameter nodes (**two ids over two elements**),
  a write through the tree landing in `AudioUnitGetParameter`, an observer hearing the value the unit
  clamped to, an originator not echoed, a removed observer going quiet, and the render block mixing **0.25 +
  0.125 × 0.25 = 0.28125**. Then the refusal: out-of-process instantiation answers with nil and an error.
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
      -DCMAKE_OBJC_COMPILER=/usr/bin/clang-19 \
      -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
# -DAGORA_AUDIO_WITH_{ALSA,PIPEWIRE,SNDIO}=OFF each drop a backend; all three default to AUTO.
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
