/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioResampler.c — the arbitrary-ratio sample-rate converter.
 *
 * A WINDOWED-SINC INTERPOLATOR, EVALUATED AT THE FRACTIONAL POSITION OF EACH OUTPUT FRAME. The alternative
 * designs each buy something we do not need yet: a polyphase table needs a rational ratio and a table per
 * phase (faster, less general); a linear interpolator is fast and measurably bad (its images sit ~40 dB
 * down at the top of the band, which the suite would show and a listener would hear as grit); an FFT
 * overlap-add is a much larger machine for the same answer.
 *
 * WHY THE RATIO NEED NOT BE RATIONAL: a device clock and a file's sample rate are two crystals, and their
 * ratio is 44100/48000 only in a specification. The position advances by `1/ratio` in input frames, in
 * double precision, for as long as the stream lasts.
 *
 * THE CUTOFF MOVES WITH THE RATIO. Downconverting asks the filter to remove what will not fit — everything
 * above the output Nyquist — so the cutoff is `0.95 * min(1, ratio)` of the input Nyquist. Converting up
 * keeps the input band whole.
 *
 * STATE IS A HISTORY OF INPUT, NOT OF OUTPUT: the filter for the first output frame of a call reaches back
 * into input frames the caller handed over on a previous call, so a rate change cannot be done in one
 * shot. That is also why `AudioConverterConvertBuffer` refuses a rate change and `FillComplexBuffer` does
 * not.
 */

#include "AudioToolbox_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 32 taps: a Hann window over twice the radius, which puts the first image far enough down that the suite
 * measures signal-to-noise in the tens of decibels rather than the units. */
#define AGORA_RESAMPLER_TAPS 32
/* 512 fractional positions: a quarter of a thousandth of a sample of phase error, which is far below what the
 * window's own stopband can resolve, and 32 x 512 floats is 64 KB. */
#define AGORA_RESAMPLER_PHASES 512
#define AGORA_RESAMPLER_RADIUS (AGORA_RESAMPLER_TAPS / 2)

static double fn_kernel(const agora_resampler *rs, double distance);

static void fn_ensure_capacity(agora_resampler *rs, UInt32 frames)
{
	UInt32 capacity = rs->hist_capacity ? rs->hist_capacity : 4096;

	if (frames <= rs->hist_capacity) {
		return;
	}

	while (capacity < frames) {
		capacity *= 2;
	}

	rs->hist = realloc(rs->hist, (size_t)capacity * rs->channels * sizeof(float));
	rs->hist_capacity = capacity;
}

bool fn_resampler_init(agora_resampler *rs, UInt32 channels, double ratio, bool linear, UInt32 quality)
{
	if (rs == NULL || channels == 0 || channels > 64 || ratio <= 0.0) {
		return false;
	}

	memset(rs, 0, sizeof(*rs));
	rs->channels = channels;
	rs->ratio = ratio;
	rs->linear = linear;
	rs->taps = (quality >= kAudioConverterQuality_Medium) ? AGORA_RESAMPLER_TAPS : AGORA_RESAMPLER_TAPS / 2;

	fn_ensure_capacity(rs, 4096);
	if (rs->hist == NULL) {
		return false;
	}

	/* The table: one row per fractional position, one column per tap. */
	rs->phases = AGORA_RESAMPLER_PHASES;
	rs->kernel = calloc((size_t)rs->phases * rs->taps, sizeof(float));
	if (rs->kernel == NULL) {
		return false;
	}
	{
		UInt32 phase;
		UInt32 tap;

		for (phase = 0; phase < rs->phases; phase++) {
			double fraction = (double)phase / (double)rs->phases;

			for (tap = 0; tap < rs->taps; tap++) {
				/* Tap `tap` is input frame `base - taps/2 + 1 + tap`, so its distance from the output position
				 * `base + fraction` is the fraction less the tap's offset. */
				double distance = fraction - (double)((int)tap - (int)(rs->taps / 2) + 1);

				rs->kernel[(size_t)phase * rs->taps + tap] = (float)fn_kernel(rs, distance);
			}
		}
	}

	fn_resampler_reset(rs);
	return true;
}

void fn_resampler_free(agora_resampler *rs)
{
	if (rs == NULL) {
		return;
	}

	free(rs->hist);
	free(rs->kernel);
	memset(rs, 0, sizeof(*rs));
}

void fn_resampler_reset(agora_resampler *rs)
{
	UInt32 radius = rs->taps / 2;

	if (rs->hist == NULL) {
		return;
	}

	/* LEADING SILENCE, THEN THE INPUT: the history starts `radius` frames of zeros "before" input frame
	 * 0, so the first output frame's filter is centred on frame 0 and reaches back into silence. That is
	 * what `kConverterPrimeMethod_None` describes, and it costs one buffer fill. */
	memset(rs->hist, 0, (size_t)radius * rs->channels * sizeof(float));
	rs->hist_frames = radius;
	rs->pos = (double)radius;
}

void fn_resampler_push(agora_resampler *rs, const float *frames, UInt32 count)
{
	if (count == 0) {
		return;
	}

	fn_ensure_capacity(rs, rs->hist_frames + count);
	if (rs->hist == NULL) {
		return;
	}

	memcpy(rs->hist + (size_t)rs->hist_frames * rs->channels, frames,
	       (size_t)count * rs->channels * sizeof(float));
	rs->hist_frames += count;
}

UInt32 fn_resampler_available(const agora_resampler *rs)
{
	double limit;

	if (rs->linear) {
		limit = (double)rs->hist_frames - 1.0;
	} else {
		limit = (double)rs->hist_frames - (double)(rs->taps / 2);
	}

	if (rs->pos >= limit) {
		return 0;
	}

	/* Each output frame advances the position by 1/ratio input frames. */
	{
		double frames = (limit - rs->pos) * rs->ratio;

		return frames <= 0.0 ? 0 : (UInt32)frames;
	}
}

/* The window, and the interpolator's kernel: a sinc whose argument is scaled by the cutoff so that the
 * transition band lands at the output Nyquist, under a Hann window that keeps the truncation ripple low. */
static double fn_kernel(const agora_resampler *rs, double distance)
{
	double radius = (double)(rs->taps / 2);
	double cutoff;
	double x;
	double window;
	double sinc;
	double pi = 3.14159265358979323846;

	if (distance <= -radius || distance >= radius) {
		return 0.0;
	}

	cutoff = 0.95 * (rs->ratio < 1.0 ? rs->ratio : 1.0);
	x = distance / radius;
	window = 0.5 * (1.0 + cos(pi * x));

	sinc = (distance == 0.0) ? 1.0 : sin(pi * cutoff * distance) / (pi * cutoff * distance);

	return sinc * window * cutoff;
}

UInt32 fn_resampler_pull(agora_resampler *rs, float *out, UInt32 count)
{
	UInt32 produced = 0;

	while (produced < count) {
		UInt32 channels = rs->channels;
		UInt32 channel;
		double position = rs->pos;

		if (rs->linear) {
			if (rs->pos + 1.0 >= (double)rs->hist_frames) {
				break;
			}
		} else {
			if (rs->pos + (double)(rs->taps / 2) >= (double)rs->hist_frames) {
				break;
			}
		}

		for (channel = 0; channel < channels; channel++) {
			float sample = 0.0f;

			if (rs->linear) {
				UInt32 index = (UInt32)position;
				double fraction = position - (double)index;

				if (index + 1 < rs->hist_frames) {
					float a = rs->hist[(size_t)index * channels + channel];
					float b = rs->hist[(size_t)(index + 1) * channels + channel];

					sample = (float)((1.0 - fraction) * a + fraction * b);
				} else if (index < rs->hist_frames) {
					sample = rs->hist[(size_t)index * channels + channel];
				}
			} else {
				/* The coefficients for this fractional position are one row of the table, and the weight that
				 * normalises them is the row's sum over the taps the history actually holds — the same normalisation
				 * the per-tap `fn_kernel` calls did, without the transcendentals. */
				int32_t base = (int32_t)floor(position);
				int32_t first = base - (int32_t)(rs->taps / 2) + 1;
				int32_t last = base + (int32_t)(rs->taps / 2);
				const float *row = rs->kernel + (size_t)rs->phase * rs->taps;
				double sum = 0.0;
				double weight = 0.0;
				int32_t k;

				for (k = first; k <= last; k++) {
					int32_t tap = k - first;
					int64_t index;

					if (k < 0) {
						continue;
					}
					index = (int64_t)k;
					if (index >= (int64_t)rs->hist_frames) {
						break;
					}
					sum += (double)row[tap] * rs->hist[(size_t)index * channels + channel];
					weight += (double)row[tap];
				}

				if (weight > 1e-9) {
					sample = (float)(sum / weight);
				}
			}

			out[(size_t)produced * channels + channel] = sample;
		}

		rs->pos += 1.0 / rs->ratio;
		{
			double fraction = rs->pos - floor(rs->pos);
			UInt32 phase = (UInt32)(fraction * (double)rs->phases);

			rs->phase = (phase < rs->phases) ? phase : rs->phases - 1;
		}
		produced++;
	}

	/* One move per call rather than one per frame: the history is a window, and sliding it is O(frames)
	 * however many output frames the caller asked for. */
	fn_resampler_compact(rs);

	return produced;
}

void fn_resampler_compact(agora_resampler *rs)
{
	UInt32 radius = rs->taps / 2;
	UInt32 drop;

	if (rs->hist == NULL || rs->hist_frames == 0 || rs->pos <= (double)radius) {
		return;
	}

	/* The filter for position p reads back to floor(p) - radius + 1; everything before that is spent. */
	drop = (UInt32)(rs->pos - (double)radius) + 1;
	if (drop == 0) {
		return;
	}

	if (drop > rs->hist_frames) {
		drop = rs->hist_frames;
	}

	memmove(rs->hist, rs->hist + (size_t)drop * rs->channels,
	        (size_t)(rs->hist_frames - drop) * rs->channels * sizeof(float));
	rs->hist_frames -= drop;
	rs->pos -= (double)drop;
}
