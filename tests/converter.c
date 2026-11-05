/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * converter.c — what the converter converts, measured rather than asserted.
 *
 * THREE KINDS OF EVIDENCE, because the three failure modes are different:
 *
 *   1. FORMAT, BY SAMPLE VALUE. int16 → float32 → int16 must come back within one least-significant bit
 *      everywhere and exactly in the range where the two scale factors agree. This catches a wrong
 *      divisor, a wrong clamp, and the sign convention on 8-bit integers, none of which a compiler sees.
 *   2. RATE, BY SIGNAL-TO-NOISE. A 1 kHz sine at 44.1 kHz converted to 48 kHz is fitted with the least
 *      squares sine at 1 kHz, and what is left over is the noise the interpolator added. The suite prints
 *      the number it measured; the threshold below is set from that measurement, not from hope.
 *   3. LAYOUT, BY CHANNEL. Stereo out to two non-interleaved buffers must not have swapped the channels.
 *
 * AND ONE REFUSAL: a one-shot conversion across a rate change has nowhere to keep the filter's history, so
 * it must say so instead of producing a click at every call.
 */

#include <AudioToolbox/AudioConverter.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

static AudioStreamBasicDescription fn_pcm(double rate, UInt32 channels, UInt32 bits, AudioFormatFlags flags)
{
	AudioStreamBasicDescription asbd;

	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate = rate;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = flags;
	asbd.mChannelsPerFrame = channels;
	asbd.mBitsPerChannel = bits;
	asbd.mBytesPerFrame = (bits / 8) * ((flags & kAudioFormatFlagIsNonInterleaved) ? 1 : channels);
	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	asbd.mFramesPerPacket = 1;

	return asbd;
}

/* ================================================================================================
 * 1. Format, by sample value.
 * ============================================================================================== */

static void fn_test_int16_round_trip(void)
{
	AudioStreamBasicDescription in = fn_pcm(48000.0, 1, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription out = fn_pcm(48000.0, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioConverterRef converter = NULL;
	OSStatus status;
	int16_t source[256];
	float wide[256];
	UInt32 size;
	int i;
	int worst = 0;

	/* A ramp that includes both ends of the range, because the ends are where the scale factors differ. */
	for (i = 0; i < 256; i++) {
		source[i] = (int16_t)(-32768 + i * 257);
	}

	status = AudioConverterNew(&in, &out, &converter);
	if (status != 0 || converter == NULL) {
		fn_check("a 16-bit to float converter is created", 0);
		return;
	}

	size = sizeof(wide);
	status = AudioConverterConvertBuffer(converter, sizeof(source), source, &size, wide);
	fn_check("16-bit -> float32 converts", status == 0 && size == sizeof(wide));

	/* The value scale is the one a client will rely on: 32767 is just under 1.0, -32768 is exactly -1.0. */
	fn_check("full-scale positive int16 is just under 1.0", wide[255] < 1.0f && wide[255] > 0.999f);
	fn_check("minimum int16 is exactly -1.0", wide[0] == -1.0f);

	/* Back again: within one LSB, and exact wherever the two scale factors agree. */
	{
		int16_t back[256];
		AudioStreamBasicDescription back_out = fn_pcm(48000.0, 1, 16,
		                                              kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
		AudioConverterRef reverse = NULL;

		status = AudioConverterNew(&out, &back_out, &reverse);
		fn_check("a float to 16-bit converter is created", status == 0 && reverse != NULL);

		size = sizeof(back);
		status = AudioConverterConvertBuffer(reverse, sizeof(wide), wide, &size, back);
		fn_check("float32 -> int16 converts", status == 0 && size == sizeof(back));

		for (i = 0; i < 256; i++) {
			int error = (int)back[i] - (int)source[i];
			int magnitude = error < 0 ? -error : error;

			if (magnitude > worst) {
				worst = magnitude;
			}
		}

		fn_check("the int16 round trip is within one least-significant bit", worst <= 1);

		/* And the whole range is exact, because both directions scale by 2^15: the round trip through
		 * float32 loses nothing that int16 had. */
		{
			int inexact = 0;

			for (i = 0; i < 256; i++) {
				if (back[i] != source[i]) {
					inexact++;
				}
			}

			fn_check("every int16 sample round-trips exactly", inexact == 0);
		}

		AudioConverterDispose(reverse);
	}

	AudioConverterDispose(converter);
}

typedef struct {
	float left_value;
	float right_value;
	UInt32 remaining;
} stereo_source;

static OSStatus fn_stereo_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets,
                                AudioBufferList *ioData,
                                AudioStreamPacketDescription **outDescription, void *user)
{
	stereo_source *source = user;
	UInt32 wanted;
	float *frames;
	UInt32 i;
	(void)converter;
	(void)outDescription;

	if (source->remaining == 0) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	wanted = *ioNumberDataPackets < source->remaining ? *ioNumberDataPackets : source->remaining;
	frames = malloc((size_t)wanted * 2 * sizeof(float));
	if (frames == NULL) {
		*ioNumberDataPackets = 0;
		return kAudio_MemFullError;
	}

	for (i = 0; i < wanted; i++) {
		frames[i * 2 + 0] = source->left_value;
		frames[i * 2 + 1] = source->right_value;
	}

	source->remaining -= wanted;

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = 2;
	ioData->mBuffers[0].mDataByteSize = wanted * 2 * (UInt32)sizeof(float);
	ioData->mBuffers[0].mData = frames;
	*ioNumberDataPackets = wanted;
	return 0;
}

static void fn_test_interleaving(void)
{
	AudioStreamBasicDescription in = fn_pcm(48000.0, 2, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription out = fn_pcm(48000.0, 2, 32,
	                                         kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
	                                         | kAudioFormatFlagIsNonInterleaved);
	AudioConverterRef converter = NULL;
	float left[64];
	float right[64];
	OSStatus status;
	UInt32 wanted = 64;
	AudioBufferList list;
	stereo_source source;

	status = AudioConverterNew(&in, &out, &converter);
	fn_check("an interleaved to non-interleaved converter is created", status == 0 && converter != NULL);
	if (status != 0) {
		return;
	}

	memset(&list, 0, sizeof(list));
	list.mNumberBuffers = 2;
	list.mBuffers[0].mNumberChannels = 1;
	list.mBuffers[0].mDataByteSize = (UInt32)sizeof(left);
	list.mBuffers[0].mData = left;
	list.mBuffers[1].mNumberChannels = 1;
	list.mBuffers[1].mDataByteSize = (UInt32)sizeof(right);
	list.mBuffers[1].mData = right;

	source.left_value = 0.5f;
	source.right_value = -0.5f;
	source.remaining = 64;

	status = AudioConverterFillComplexBuffer(converter, fn_stereo_input, &source, &wanted, &list, NULL);
	fn_check("the split conversion runs", status == 0 && wanted == 64);
	fn_check("the split kept the left channel left", left[0] == 0.5f && left[63] == 0.5f);
	fn_check("the split kept the right channel right", right[0] == -0.5f && right[63] == -0.5f);

	AudioConverterDispose(converter);
}

/* ================================================================================================
 * 2. Rate, by signal-to-noise.
 * ============================================================================================== */

typedef struct {
	const float *samples;   /* interleaved mono, at the input rate */
	UInt32       frames;
	UInt32       position;
} sine_source;

static OSStatus fn_sine_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets,
                              AudioBufferList *ioData,
                              AudioStreamPacketDescription **outDescription, void *user)
{
	sine_source *source = user;
	UInt32 available;
	UInt32 wanted;
	(void)converter;
	(void)outDescription;

	if (source->position >= source->frames) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	available = source->frames - source->position;
	wanted = *ioNumberDataPackets < available ? *ioNumberDataPackets : available;

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = 1;
	ioData->mBuffers[0].mDataByteSize = wanted * (UInt32)sizeof(float);
	ioData->mBuffers[0].mData = (void *)(source->samples + source->position);

	source->position += wanted;
	*ioNumberDataPackets = wanted;
	return 0;
}

static void fn_test_rate_conversion(void)
{
	const UInt32 in_rate = 44100;
	const UInt32 out_rate = 48000;
	const UInt32 seconds = 1;
	const double tone = 1000.0;
	float *input;
	double *output;
	UInt32 in_frames = in_rate * seconds;
	UInt32 out_frames_total = out_rate * seconds + 4096;
	AudioStreamBasicDescription in = fn_pcm(in_rate, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription out = fn_pcm(out_rate, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioConverterRef converter = NULL;
	sine_source source;
	OSStatus status;
	UInt32 produced = 0;
	UInt32 i;

	input = malloc((size_t)in_frames * sizeof(float));
	output = malloc((size_t)out_frames_total * sizeof(double));
	if (input == NULL || output == NULL) {
		fn_check("scratch allocation", 0);
		return;
	}

	for (i = 0; i < in_frames; i++) {
		input[i] = (float)(0.5 * sin(2.0 * M_PI * tone * (double)i / (double)in_rate));
	}

	status = AudioConverterNew(&in, &out, &converter);
	if (status != 0) {
		fn_check("a 44.1 kHz to 48 kHz converter is created", 0);
		free(input);
		free(output);
		return;
	}

	/* DELIBERATELY IN SMALL CHUNKS: the resampler's history is only correct if it survives calls, and one
	 * big call would test nothing about that. */
	source.samples = input;
	source.frames = in_frames;
	source.position = 0;

	while (produced < out_frames_total) {
		float chunk[1024];
		AudioBufferList list;
		UInt32 wanted = 1024;

		if (wanted > out_frames_total - produced) {
			wanted = out_frames_total - produced;
		}

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = 1;
		list.mBuffers[0].mDataByteSize = (UInt32)sizeof(chunk);
		list.mBuffers[0].mData = chunk;

		status = AudioConverterFillComplexBuffer(converter, fn_sine_input, &source, &wanted, &list, NULL);
		if (status != 0) {
			fn_check("FillComplexBuffer reports success", 0);
			break;
		}

		if (wanted == 0) {
			break;
		}

		for (i = 0; i < wanted; i++) {
			output[produced + i] = chunk[i];
		}

		produced += wanted;
	}

	/* The rate change happened: about 48000 frames out of 44100 in, allowing for the filter's group delay
	 * and priming. */
	fn_check("the rate change produced roughly the right number of frames",
	         produced > out_rate - 64 && produced < out_rate + 64);

	/* SIGNAL-TO-NOISE, by least-squares projection onto the tone. The interpolator's group delay is unknown
	 * and is absorbed by the fit's two coefficients (sine and cosine), which is why this measures the
	 * waveform's shape rather than its phase. */
	{
		double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0, yy = 0.0;
		double a, b;
		double signal_energy, residual_energy;
		double snr;
		UInt32 n = produced > 4096 ? produced - 2048 : produced;  /* skip the ramp-in */

		for (i = 0; i < n; i++) {
			double s = sin(2.0 * M_PI * tone * (double)i / (double)out_rate);
			double c = cos(2.0 * M_PI * tone * (double)i / (double)out_rate);
			double y = output[i];

			ss += s * s;
			cc += c * c;
			sc += s * c;
			ys += y * s;
			yc += y * c;
			yy += y * y;
		}

		/* 2x2 normal equations. */
		{
			double det = ss * cc - sc * sc;

			a = (ys * cc - yc * sc) / det;
			b = (yc * ss - ys * sc) / det;
		}

		signal_energy = 0.0;
		residual_energy = 0.0;
		for (i = 0; i < n; i++) {
			double s = sin(2.0 * M_PI * tone * (double)i / (double)out_rate);
			double c = cos(2.0 * M_PI * tone * (double)i / (double)out_rate);
			double fitted = a * s + b * c;
			double residual = output[i] - fitted;

			signal_energy += fitted * fitted;
			residual_energy += residual * residual;
		}

		snr = 10.0 * log10(signal_energy / (residual_energy + 1e-30));
		printf("converter: 44.1 kHz -> 48 kHz, 1 kHz tone, %u frames, SNR %.1f dB\n", produced, snr);

		/* 60 dB is roughly 10 bits of accuracy: the floor a resampler must clear to be worth shipping, and
		 * comfortably under what this filter actually measures. */
		fn_check("the resampled tone is at least 60 dB above its noise", snr >= 60.0);

		/* The amplitude is preserved: half scale in, half scale out. */
		{
			double amplitude = sqrt(a * a + b * b);

			fn_check("the tone keeps its amplitude", amplitude > 0.48 && amplitude < 0.52);
		}
	}

	AudioConverterDispose(converter);
	free(input);
	free(output);
}

/* ================================================================================================
 * 3. Refusals.
 * ============================================================================================== */

static void fn_test_refusals(void)
{
	AudioStreamBasicDescription pcm = fn_pcm(48000.0, 1, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription aac = pcm;
	AudioConverterRef converter = NULL;
	OSStatus status;

	aac.mFormatID = kAudioFormatMPEG4AAC;
	status = AudioConverterNew(&aac, &pcm, &converter);
	fn_check("AAC input is refused by name", status == kAudioConverterErr_FormatNotSupported);

	status = AudioConverterNew(&pcm, &aac, &converter);
	fn_check("AAC output is refused by name", status == kAudioConverterErr_FormatNotSupported);

	/* Two channels in, one out: no map, no conversion. */
	{
		AudioStreamBasicDescription stereo = fn_pcm(48000.0, 2, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
		AudioStreamBasicDescription mono = fn_pcm(48000.0, 1, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);

		status = AudioConverterNew(&stereo, &mono, &converter);
		fn_check("a channel count change without a map is refused",
		         status == kAudioConverterErr_FormatNotSupported);
	}

	/* A rate change through the one-shot door. */
	{
		AudioStreamBasicDescription other_rate = fn_pcm(44100.0, 1, 16,
		                                                kAudioFormatFlagIsSignedInteger
		                                                | kAudioFormatFlagIsPacked);

		status = AudioConverterNew(&pcm, &other_rate, &converter);
	}
	fn_check("a converter across a rate change is created", status == 0 && converter != NULL);
	if (status == 0 && converter != NULL) {
		int16_t source[16];
		int16_t destination[16];
		UInt32 size = (UInt32)sizeof(destination);

		memset(source, 0, sizeof(source));
		status = AudioConverterConvertBuffer(converter, (UInt32)sizeof(source), source, &size, destination);
		fn_check("ConvertBuffer refuses a rate change",
		         status == kAudioConverterErr_FormatNotSupported);

		/* A selector nobody implements says so, rather than answering nonsense. */
		fn_check("an unknown converter property is refused by name",
		         AudioConverterGetPropertyInfo(converter, 0x7A7A7A7A, NULL, NULL)
		         == kAudioConverterErr_PropertyNotSupported);

		AudioConverterDispose(converter);
	}
}

int main(void);
int main(void)
{
	fn_test_int16_round_trip();
	fn_test_interleaving();
	fn_test_rate_conversion();
	fn_test_refusals();

	if (g_failures != 0) {
		fprintf(stderr, "converter: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("converter: sample values, channel layout, rate and refusals measured\n");
	return 0;
}
