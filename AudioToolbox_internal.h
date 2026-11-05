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

#endif /* AGORA_AUDIOTOOLBOX_INTERNAL_H */
