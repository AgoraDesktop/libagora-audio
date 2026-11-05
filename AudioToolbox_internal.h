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
	bool         (*start)(AudioDeviceID id);
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

/* Called by a driver's data thread. Fills `mix` (interleaved float32, `frames` long) from every attached io
 * proc, applies the device's volume, and returns nothing — the driver plays it or drops it. */
void fn_agora_device_render(AudioDeviceID id, float *mix, UInt32 frames);

/* Test hooks, not part of the API and never installed: what the mixer produced, and how many frames it has
 * rendered, so a suite can assert that two io procs summed rather than that they were both called. */
void fn_agora_audio_test_peaks(AudioDeviceID id, float *out_peak, UInt64 *out_frames);

#endif /* AGORA_AUDIOTOOLBOX_INTERNAL_H */
