/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox_internal.h — the pieces the conversion path shares, and the resampler it keeps.
 *
 * NOT INSTALLED (the install rule excludes `*_internal.h`), and nothing Apple declares appears here: this
 * is where our own arithmetic lives, so that the public headers stay a transcription of 10.12 rather than
 * a place our decisions leaked into.
 */

#ifndef AGORA_AUDIOTOOLBOX_INTERNAL_H
#define AGORA_AUDIOTOOLBOX_INTERNAL_H

#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioConverter.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------------------
 * Container byte order. Shared so that the file door and the stream door cannot disagree about a chunk
 * header, which is the kind of disagreement that reads as "works on this file, not that one".
 * ---------------------------------------------------------------------------------------------- */

uint16_t fn_rd16le(const uint8_t *p);
uint32_t fn_rd32le(const uint8_t *p);
uint16_t fn_rd16be(const uint8_t *p);
uint32_t fn_rd32be(const uint8_t *p);
uint64_t fn_rd64be(const uint8_t *p);
double   fn_rd_extended(const uint8_t *p);

/* ------------------------------------------------------------------------------------------------
 * Linear PCM descriptions.
 * ---------------------------------------------------------------------------------------------- */

/* The bits each sample occupies in the frame, which is not `mBitsPerChannel` when the format is packed
 * into fewer... or more: 24-bit samples in 32-bit containers are `mBitsPerChannel = 24` with a 4-byte
 * stride, and that difference is the whole reason this is a function. */
UInt32 fn_pcm_bytes_per_sample(const AudioStreamBasicDescription *asbd);

/* The frame size the description implies, one channel's worth for a non-interleaved format. */
UInt32 fn_pcm_bytes_per_frame(const AudioStreamBasicDescription *asbd);

bool fn_pcm_is_supported(const AudioStreamBasicDescription *asbd, OSStatus *out_status);

/* ------------------------------------------------------------------------------------------------
 * The decoders.
 *
 * **A CODEC IS A STREAM**: MPEG Layer III's bit reservoir means frame N's samples depend on the frames before it,
 * so a decoder is a handle fed in order rather than a call that answers "decode these bytes". `fn_codec_reset` is
 * what a seek says to it. The decoder produces INTERLEAVED FLOAT32, which is the currency of the conversion path.
 *
 * `fn_codec_is_decodable` answers for a description without opening anything, so a door can refuse an unplayable
 * file before it allocates. Every one of them answers "no" on a build without the decoder compiled in, and
 * `fn_codec_decode` reports the channel count it actually produced so the caller can refuse a stream whose frames
 * disagree with the description the container gave.
 * ---------------------------------------------------------------------------------------------- */
typedef struct AudioCodec AudioCodec;

bool        fn_codec_is_decodable(const AudioStreamBasicDescription *asbd);
AudioCodec *fn_codec_open(const AudioStreamBasicDescription *asbd);
void        fn_codec_close(AudioCodec *codec);
void        fn_codec_reset(AudioCodec *codec);

/* Feed compressed bytes; write interleaved float32 frames into `out`; the return is frames written, which stops at
 * `capacity`. Zero frames with a non-zero feed means the decoder wants more bytes, not that the file has ended. */
UInt32      fn_codec_decode(AudioCodec *codec, const void *bytes, UInt32 byte_count,
                            float *out, UInt32 capacity, UInt32 *out_channels);

/* ------------------------------------------------------------------------------------------------
 * The MPEG frame header.
 *
 * **ONE FOUR-BYTE HEADER, TWO DOORS THAT WALK IT**: `AudioFile.c` indexes a whole file and `AudioFileStream.c`
 * reports packets as they arrive, and they must agree about what a frame is. There is nothing to keep in sync
 * between them because there is nothing to keep: a frame header is a pure function of its four bytes, and the
 * length it implies is the caller's to check against the bytes it actually holds.
 * ---------------------------------------------------------------------------------------------- */
typedef struct fn_mpeg_frame {
	UInt32 version;	/* 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5 (1 is reserved and never returned) */
	UInt32 bitrate;	/* kbit/s, from the table index */
	UInt32 rate;	/* Hz */
	UInt32 channels;	/* 1 for single-channel mode, 2 for every other mode */
	UInt32 samples;	/* samples per frame: 1152 for MPEG-1, 576 for MPEG-2 and 2.5 */
	UInt32 size;	/* the frame's length in bytes, its padding included */
	UInt32 has_crc;	/* the protection bit: a CRC adds two bytes before the side information */
} fn_mpeg_frame;

/* **THE XING/INFO TAG IS THE ONLY LENGTH AN MPEG STREAM DECLARES** — a frame count, a byte count and a 100-entry
 * seek table, in an otherwise unindexed format. `Info` is the same tag spelled for a constant-bitrate file, and both
 * answer here. Everything in it is optional, which is why each field has its own flag rather than a sentinel. */
typedef struct fn_mpeg_xing {
	bool    present;
	bool    has_frames;
	UInt32  frames;	/* the count in the tag: it does NOT include the frame that carries it */
	bool    has_bytes;
	UInt32  bytes;	/* the audio's length, the tag's own frame included */
	bool    has_toc;
	uint8_t toc[100];
	bool    has_gapless;	/* the LAME/ffmpeg extension declared the encoder's delay and padding */
	UInt32  delay;		/* samples the encoder padded at the start, to be discarded on playback */
	UInt32  padding;	/* samples it padded at the end */
} fn_mpeg_xing;

/* Parse the tag out of the FIRST frame of a stream. False when the frame carries none (the common case for a file
 * cut out of a longer one, and for anything not written by LAME). */
bool   fn_mpeg_xing_parse(const uint8_t *frame, UInt32 available, const fn_mpeg_frame *header,
                           fn_mpeg_xing *out);


/* Parse the header at `at` out of `available` bytes. False when those bytes are not a Layer III header this door
 * reads, or when a reserved field value says the sync word was a coincidence. */
bool   fn_mpeg_frame_header(const uint8_t *data, UInt32 available, UInt32 at, fn_mpeg_frame *out);

/* 1 with `out_length` filled when an ID3v2 tag begins here, 0 when it does not, -1 when more bytes are needed to
 * answer. The caller skips the tag by this length; the tag may contain anything, including sync words. */
int    fn_mpeg_id3v2_length(const uint8_t *data, UInt32 available, UInt32 *out_length);

/* The largest frame bytes-per-frame this rate and samples-per-frame allow, for a stream's `PacketSizeUpperBound`. */
UInt32 fn_mpeg_packet_size_upper_bound(const AudioStreamBasicDescription *asbd);

/* Read one frame from `src` into interleaved float32; write one frame from interleaved float32 into
 * `dst`. Both take the *frame* stride the description implies. */
void fn_pcm_frame_to_float(const AudioStreamBasicDescription *asbd, const uint8_t *src, float *dst);
void fn_pcm_frame_from_float(const AudioStreamBasicDescription *asbd, const float *src, uint8_t *dst);

/* ------------------------------------------------------------------------------------------------
 * The resampler.
 *
 * A windowed-sinc interpolator with a Kaiser-shaped window, evaluated at the fractional position of each
 * output frame. It is not the fastest possible design — a polyphase table would be — but it is exact
 * enough to be measured by signal-to-noise rather than by "it ran", and it handles any ratio without
 * needing that ratio to be rational, which is what a device clock actually gives you.
 *
 * ITS STATE IS THE POINT: a rate change cannot be done in one call, because the filter for the first
 * output frame reaches back into input the caller has already handed over. So the history lives here, in
 * interleaved float32, and `pos` is the fractional input position of the next output frame measured from
 * the start of that history.
 * ---------------------------------------------------------------------------------------------- */

typedef struct agora_resampler {
	UInt32   channels;
	double   ratio;          /* output frames per input frame */
	double   pos;            /* fractional input position of the next output frame */
	UInt32   taps;           /* filter length, in input frames */
	float *  hist;           /* interleaved float32 history */
	UInt32   hist_frames;    /* frames held in `hist` */
	UInt32   hist_capacity;  /* frames `hist` can hold */
	bool     linear;         /* the caller asked for the linear complexity */
	UInt32   primes;         /* frames of silence prepended at the start */
} agora_resampler;

bool fn_resampler_init(agora_resampler *rs, UInt32 channels, double ratio, bool linear, UInt32 quality);
void fn_resampler_free(agora_resampler *rs);
void fn_resampler_reset(agora_resampler *rs);

/* Append input frames (interleaved float32) to the history. */
void fn_resampler_push(agora_resampler *rs, const float *frames, UInt32 count);

/* How many output frames can be produced from the history as it stands. */
UInt32 fn_resampler_available(const agora_resampler *rs);

/* Produce up to `count` output frames into `out` (interleaved float32); returns how many. */
UInt32 fn_resampler_pull(agora_resampler *rs, float *out, UInt32 count);

/* Drop history that no longer contributes to any future output frame. */
void fn_resampler_compact(agora_resampler *rs);

/* ------------------------------------------------------------------------------------------------
 * The HAL's driver seam, and the two hooks the suites use.
 *
 * A DRIVER DESCRIBES DEVICES AND RUNS A DATA THREAD; IT DOES NOT ANSWER PROPERTIES. Everything above —
 * the object table, the property switch, the listeners, the mixer — is portable, which is what makes the
 * same suite able to measure the null backend today and a PipeWire backend later without changing a test.
 * ---------------------------------------------------------------------------------------------- */

#include <CoreAudio/AudioHardware.h>

typedef struct agora_device_desc {
	AudioDeviceID id;
	const char *  name;
	const char *  uid;
	const char *  manufacturer;
	UInt32        transport;
	Float64       nominal_rate;
	UInt32        buffer_frames;
	UInt32        channels;
	bool          has_input;
	bool          has_output;
	UInt32        terminal_type;
} agora_device_desc;

typedef struct agora_driver {
	const char * name;
	UInt32       (*device_count)(void);
	bool         (*device_describe)(UInt32 index, agora_device_desc *out);
	/* **A DEVICE IS STARTED FOR A REASON, AND THE REASON DECIDES WHAT IS OPENED.** `want_input` is the HAL's answer
	 * to "has any client of this device asked for its input?" — false unless somebody is recording. A backend that
	 * ignores it pays for it: ALSA holds a capture PCM open, and a session server publishes a capture *node* (which
	 * wakes the microphone, shows in everyone's mixer, and asks for permission on some setups) for a client that
	 * only wanted to play. */
	bool         (*start)(AudioDeviceID id, bool want_input);
	void         (*stop)(AudioDeviceID id);
} agora_driver;

/* The backends. The null one is always there; the others are compiled only when their system library is
 * found, and answer NULL otherwise. */
const agora_driver * fn_agora_driver_null(void);
const agora_driver * fn_agora_driver_alsa(void);
const agora_driver * fn_agora_driver_pipewire(void);
const agora_driver * fn_agora_driver_sndio(void);

/* The backend to use: `AGORA_AUDIO_DRIVER` if set; `NULL` here or no variable at all means "what this machine
 * would use by itself", which is PipeWire when its server answers and the null device otherwise. A suite calls
 * the `fn_` form with a name so a developer's shell cannot change what a test measured. */
void fn_agora_audio_set_driver(const char *name);

/* Called by a driver's data thread. `capture` is what the device heard during this cycle — interleaved float32,
 * `capture_frames` long and `channels` wide — or NULL (or empty) for a device with no input; it is handed to every io
 * proc as its `inInputData`, unchanged and read-only, because on a real device the input belongs to the cycle rather
 * than to a client. **THE INPUT HAS ITS OWN LENGTH**: a session server's capture and playback quanta are negotiated
 * separately, and a driver that had to bend one to the other would either drop audio or claim samples it does not
 * have. `mix` (interleaved float32, `frames` long) is filled from every attached io proc and the device's volume is
 * applied to it, and the driver plays it or drops it. */
void fn_agora_device_render(AudioDeviceID id, const float *capture, UInt32 capture_frames, float *mix,
                            UInt32 frames);

/* **TELL THE DEVICE THAT A CLIENT WANTS ITS INPUT, BEFORE STARTING IT.** An input `AudioQueue` is the caller; the
 * HAL remembers it and hands it to the driver at `start`, and a device already running for playback is restarted
 * once so the driver can open the other direction. */
void fn_agora_device_set_input_wanted(AudioDeviceID id, bool wanted);

/* Test hooks, not part of the API and never installed: what the mixer produced, and how many frames it has
 * rendered, so a suite can assert that two io procs summed rather than that they were both called. */
void fn_agora_audio_test_peaks(AudioDeviceID id, float *out_peak, UInt64 *out_frames);

#endif /* AGORA_AUDIOTOOLBOX_INTERNAL_H */
