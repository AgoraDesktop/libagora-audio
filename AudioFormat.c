/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioFormat.c — the questions a format description can be asked.
 *
 * FOUR PROPERTIES, AND A REFUSAL THAT NAMES ITSELF FOR THE REST. Apple's 10.12 table has around forty
 * selectors; most of them are questions about a *file* (its format list), a *codec* (its bit rates) or a
 * *device* (its channel layouts), and each belongs to the layer that can answer it. A selector that is
 * declared and answers `kAudioFormatUnsupportedPropertyError` is a better citizen than one that is
 * missing, because a client compiled against macOS still links, still runs, and gets told why.
 *
 * THE ARITHMETIC HERE IS THE PART MACOS DOES NOT DOCUMENT IN PROSE. `kAudioFormatProperty_FormatInfo`
 * fills in a half-written description, and the rules for "how big is a frame" are the rules everyone gets
 * wrong once: a 24-bit sample in a 32-bit container is 4 bytes; a non-interleaved description's
 * `mBytesPerFrame` counts *one* channel, not all of them.
 */

#include <AudioToolbox/AudioFormat.h>
#include "AudioToolbox_internal.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------
 * Linear PCM arithmetic.
 * ---------------------------------------------------------------------------------------------- */

UInt32 fn_pcm_bytes_per_sample(const AudioStreamBasicDescription *asbd)
{
	/* `mBitsPerChannel` is the SIGNIFICANT bits; the container is what the bytes say. A description with
	 * `mBytesPerFrame` already filled in answers this directly. */
	UInt32 channels_in_frame = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1U
	                                                                                  : asbd->mChannelsPerFrame;

	if (asbd->mBytesPerFrame != 0 && channels_in_frame != 0) {
		return asbd->mBytesPerFrame / channels_in_frame;
	}

	/* No stride given: a sample occupies whole bytes, rounded up. 20 significant bits in 3 bytes is the
	 * common shape, and 24 bits in 3 bytes is the usual one. */
	if (asbd->mBitsPerChannel == 0) {
		return 0;
	}

	return (asbd->mBitsPerChannel + 7) / 8;
}

UInt32 fn_pcm_bytes_per_frame(const AudioStreamBasicDescription *asbd)
{
	UInt32 bytes_per_sample = fn_pcm_bytes_per_sample(asbd);
	UInt32 channels = asbd->mChannelsPerFrame;

	if (!(asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
		return bytes_per_sample * channels;
	}

	/* Non-interleaved: one buffer per channel, so a frame in any one buffer is one sample long. */
	return bytes_per_sample;
}

bool fn_pcm_is_supported(const AudioStreamBasicDescription *asbd, OSStatus *out_status)
{
	UInt32 bytes_per_sample;

	if (asbd == NULL) {
		if (out_status) { *out_status = kAudio_ParamError; }
		return false;
	}

	if (asbd->mFormatID != kAudioFormatLinearPCM) {
		/* No codec exists here yet, so this is the honest answer for AAC, ALAC and MP3 alike. */
		if (out_status) { *out_status = kAudioConverterErr_FormatNotSupported; }
		return false;
	}

	if (asbd->mChannelsPerFrame == 0 || asbd->mChannelsPerFrame > 64) {
		if (out_status) { *out_status = kAudioFormatUnsupportedDataFormatError; }
		return false;
	}

	if (asbd->mSampleRate <= 0.0) {
		if (out_status) { *out_status = kAudioFormatUnsupportedDataFormatError; }
		return false;
	}

	if (asbd->mFormatFlags & kAudioFormatFlagIsFloat) {
		if (asbd->mBitsPerChannel != 32 && asbd->mBitsPerChannel != 64) {
			if (out_status) { *out_status = kAudioFormatUnsupportedDataFormatError; }
			return false;
		}
	} else {
		/* Integers: 8, 16, 24, 32 significant bits. An 8-bit integer is unsigned unless it says
		 * otherwise, which is the WAV convention Core Audio also follows. */
		if (asbd->mBitsPerChannel != 8 && asbd->mBitsPerChannel != 16
		    && asbd->mBitsPerChannel != 24 && asbd->mBitsPerChannel != 32) {
			if (out_status) { *out_status = kAudioFormatUnsupportedDataFormatError; }
			return false;
		}
	}

	bytes_per_sample = fn_pcm_bytes_per_sample(asbd);
	if (bytes_per_sample == 0 || bytes_per_sample > 8) {
		if (out_status) { *out_status = kAudioFormatUnsupportedDataFormatError; }
		return false;
	}

	if (out_status) { *out_status = 0; }
	return true;
}

/* ------------------------------------------------------------------------------------------------
 * Samples in and out.
 *
 * EVERY CONVERSION PASSES THROUGH FLOAT32. It is the one representation every format here round-trips
 * exactly — 24-bit integers are exact in a float32 only up to 24 bits of mantissa, which is exactly the
 * width of the integer, so the round trip is deliberate and checked by `tests/converter.c` — and it costs
 * nothing measurable against the filter's own work.
 * ---------------------------------------------------------------------------------------------- */

static float fn_read_sample(const uint8_t *p, UInt32 bits, bool is_float, bool is_signed, bool is_big_endian)
{
	if (is_float) {
		if (bits == 32) {
			float value;
			uint32_t raw;

			memcpy(&raw, p, 4);
			if (is_big_endian) { raw = __builtin_bswap32(raw); }
			memcpy(&value, &raw, 4);
			return value;
		} else {
			double value;
			uint64_t raw;

			memcpy(&raw, p, 8);
			if (is_big_endian) { raw = __builtin_bswap64(raw); }
			memcpy(&value, &raw, 8);
			return (float)value;
		}
	}

	switch (bits) {
	case 8: {
		if (is_signed) {
			return (float)(int8_t)(*p) / 128.0f;
		}
		return ((float)(*p) - 128.0f) / 128.0f;
	}
	case 16: {
		int16_t value;

		memcpy(&value, p, 2);
		if (is_big_endian) { value = (int16_t)__builtin_bswap16((uint16_t)value); }
		return (float)value / 32768.0f;
	}
	case 24: {
		uint32_t raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);

		if (is_big_endian) {
			raw = (uint32_t)p[2] | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16);
		}
		/* Sign-extend from 24 bits, whatever the container. */
		if (raw & 0x00800000U) {
			raw |= 0xFF000000U;
		}
		return (float)(int32_t)raw / 8388608.0f;
	}
	default: {
		int32_t value;

		memcpy(&value, p, 4);
		if (is_big_endian) { value = (int32_t)__builtin_bswap32((uint32_t)value); }
		return (float)((double)value / 2147483648.0);
	}
	}
}

/* THE SCALE IS 2^(bits-1), AND THE CLAMP IS WHAT MAKES THE ROUND TRIP EXACT: `v / 32768` then
 * `v * 32768` is the identity on every int16, while the "multiply by 32767" convention is off by one
 * least-significant bit for every sample that is not zero. The price is that +1.0 saturates to the largest
 * positive sample rather than wrapping, which is what a limiter would do anyway. */
static int64_t fn_round_clamp(double value, int64_t minimum, int64_t maximum)
{
	double rounded = (value >= 0.0) ? floor(value + 0.5) : ceil(value - 0.5);

	if (rounded < (double)minimum) {
		return minimum;
	}
	if (rounded > (double)maximum) {
		return maximum;
	}

	return (int64_t)rounded;
}

static void fn_write_sample(uint8_t *p, UInt32 bits, bool is_float, bool is_signed, bool is_big_endian, float value)
{
	double clamped = (double)value;

	if (is_float) {
		if (bits == 32) {
			uint32_t raw;

			memcpy(&raw, &value, 4);
			if (is_big_endian) { raw = __builtin_bswap32(raw); }
			memcpy(p, &raw, 4);
		} else {
			double wide = (double)value;
			uint64_t raw;

			memcpy(&raw, &wide, 8);
			if (is_big_endian) { raw = __builtin_bswap64(raw); }
			memcpy(p, &raw, 8);
		}
		return;
	}

	if (clamped < -1.0) { clamped = -1.0; }
	if (clamped > 1.0) { clamped = 1.0; }

	switch (bits) {
	case 8: {
		if (is_signed) {
			int64_t sample = fn_round_clamp(clamped * 128.0, -128, 127);

			p[0] = (uint8_t)(int8_t)sample;
		} else {
			int64_t sample = fn_round_clamp(clamped * 128.0 + 128.0, 0, 255);

			p[0] = (uint8_t)sample;
		}
		return;
	}
	case 16: {
		int64_t sample = fn_round_clamp(clamped * 32768.0, -32768, 32767);
		uint16_t raw = (uint16_t)(int16_t)sample;

		if (is_big_endian) { raw = __builtin_bswap16(raw); }
		memcpy(p, &raw, 2);
		return;
	}
	case 24: {
		int64_t sample = fn_round_clamp(clamped * 8388608.0, -8388608, 8388607);
		uint32_t raw = (uint32_t)sample & 0x00FFFFFFU;

		if (is_big_endian) {
			p[0] = (uint8_t)(raw >> 16);
			p[1] = (uint8_t)(raw >> 8);
			p[2] = (uint8_t)raw;
		} else {
			p[0] = (uint8_t)raw;
			p[1] = (uint8_t)(raw >> 8);
			p[2] = (uint8_t)(raw >> 16);
		}
		return;
	}
	default: {
		int64_t sample = fn_round_clamp(clamped * 2147483648.0, -2147483648LL, 2147483647LL);
		uint32_t raw = (uint32_t)(int32_t)sample;

		if (is_big_endian) { raw = __builtin_bswap32(raw); }
		memcpy(p, &raw, 4);
		return;
	}
	}
}

void fn_pcm_frame_to_float(const AudioStreamBasicDescription *asbd, const uint8_t *src, float *dst)
{
	bool is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
	bool is_signed = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
	/* An 8-bit integer with neither flag is unsigned, per Core Audio's own convention. */
	bool is_big_endian = (asbd->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;
	UInt32 bytes_per_sample = fn_pcm_bytes_per_sample(asbd);
	UInt32 channels = asbd->mChannelsPerFrame;
	UInt32 channel;

	if (!is_float && asbd->mBitsPerChannel == 8 && !is_signed) {
		/* keep unsigned */
	} else if (!is_float && asbd->mBitsPerChannel != 8) {
		is_signed = true;
	}

	if (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
		/* `src` is the frame in ONE channel's buffer; the caller names the channel. */
		dst[0] = fn_read_sample(src, asbd->mBitsPerChannel, is_float, is_signed, is_big_endian);
		return;
	}

	for (channel = 0; channel < channels; channel++) {
		dst[channel] = fn_read_sample(src + (size_t)channel * bytes_per_sample,
		                              asbd->mBitsPerChannel, is_float, is_signed, is_big_endian);
	}
}

void fn_pcm_frame_from_float(const AudioStreamBasicDescription *asbd, const float *src, uint8_t *dst)
{
	bool is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
	bool is_signed = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
	bool is_big_endian = (asbd->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;
	UInt32 bytes_per_sample = fn_pcm_bytes_per_sample(asbd);
	UInt32 channels = asbd->mChannelsPerFrame;
	UInt32 channel;

	if (!is_float && asbd->mBitsPerChannel != 8) {
		is_signed = true;
	}

	if (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
		fn_write_sample(dst, asbd->mBitsPerChannel, is_float, is_signed, is_big_endian, src[0]);
		return;
	}

	for (channel = 0; channel < channels; channel++) {
		fn_write_sample(dst + (size_t)channel * bytes_per_sample,
		                asbd->mBitsPerChannel, is_float, is_signed, is_big_endian, src[channel]);
	}
}

/* ------------------------------------------------------------------------------------------------
 * The property door.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_format_info(const void *specifier, UInt32 specifier_size,
                               UInt32 *data_size, void *data)
{
	const AudioFormatInfo *info = (const AudioFormatInfo *)specifier;
	AudioStreamBasicDescription asbd;
	OSStatus status;
	UInt32 bytes_per_sample;
	UInt32 channels;

	if (specifier == NULL || specifier_size < sizeof(AudioFormatInfo)) {
		return kAudioFormatBadSpecifierSizeError;
	}

	asbd = info->mASBD;

	if (asbd.mFormatID != kAudioFormatLinearPCM) {
		/* Apple fills in linear PCM and refuses the rest here; the codec's own component answers for
		 * its format. No codec, no answer. */
		return kAudioFormatUnsupportedDataFormatError;
	}

	if (!fn_pcm_is_supported(&asbd, &status)) {
		return status;
	}

	channels = asbd.mChannelsPerFrame;
	bytes_per_sample = fn_pcm_bytes_per_sample(&asbd);

	/* FILL IN WHAT IS MISSING, NEVER OVERWRITE WHAT WAS SAID. The one exception is the frame arithmetic,
	 * which has to agree with the sample size or the description is not a description. */
	if (asbd.mBitsPerChannel == 0) {
		asbd.mBitsPerChannel = bytes_per_sample * 8;
	}

	if (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
		asbd.mBytesPerFrame = bytes_per_sample;
	} else {
		asbd.mBytesPerFrame = bytes_per_sample * channels;
	}

	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	asbd.mFramesPerPacket = 1;

	if (data == NULL) {
		return kAudioFormatBadPropertySizeError;
	}
	if (data_size == NULL || *data_size < sizeof(AudioStreamBasicDescription)) {
		return kAudioFormatBadPropertySizeError;
	}

	memcpy(data, &asbd, sizeof(AudioStreamBasicDescription));
	*data_size = sizeof(AudioStreamBasicDescription);
	return 0;
}

static UInt32 fn_channels_in_bitmap(AudioChannelBitmap bitmap)
{
	UInt32 count = 0;

	while (bitmap != 0) {
		count += bitmap & 1U;
		bitmap >>= 1;
	}

	return count;
}

static OSStatus fn_channels_for_layout(const void *specifier, UInt32 specifier_size,
                                       UInt32 *data_size, void *data)
{
	const AudioChannelLayout *layout = (const AudioChannelLayout *)specifier;
	UInt32 channels;

	if (layout == NULL || specifier_size < offsetof(AudioChannelLayout, mChannelDescriptions)) {
		return kAudioFormatBadSpecifierSizeError;
	}

	if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
		if (layout->mNumberChannelDescriptions == 0) {
			return kAudioFormatUnsupportedDataFormatError;
		}
		channels = layout->mNumberChannelDescriptions;
	} else if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
		channels = fn_channels_in_bitmap(layout->mChannelBitmap);
		if (channels == 0) {
			return kAudioFormatUnsupportedDataFormatError;
		}
	} else {
		/* A named layout encodes its own channel count in the low half. */
		channels = layout->mChannelLayoutTag & 0xFFFFU;
		if (channels == 0) {
			return kAudioFormatUnsupportedDataFormatError;
		}
	}

	if (data == NULL) {
		return kAudioFormatBadPropertySizeError;
	}
	if (data_size == NULL || *data_size < sizeof(UInt32)) {
		return kAudioFormatBadPropertySizeError;
	}

	memcpy(data, &channels, sizeof(UInt32));
	*data_size = sizeof(UInt32);
	return 0;
}

static OSStatus fn_layout_for_tag(const void *specifier, UInt32 specifier_size,
                                  UInt32 *data_size, void *data)
{
	AudioChannelLayoutTag tag;

	if (specifier == NULL || specifier_size < sizeof(AudioChannelLayoutTag)) {
		return kAudioFormatBadSpecifierSizeError;
	}

	memcpy(&tag, specifier, sizeof(tag));

	if (tag == kAudioChannelLayoutTag_UseChannelDescriptions || tag == kAudioChannelLayoutTag_UseChannelBitmap) {
		/* Those two tags are not layouts; they are instructions to look elsewhere. */
		return kAudioFormatUnsupportedDataFormatError;
	}

	if (data == NULL) {
		return kAudioFormatBadPropertySizeError;
	}
	if (data_size == NULL || *data_size < sizeof(AudioChannelLayout)) {
		return kAudioFormatBadPropertySizeError;
	}

	/* A named layout needs no descriptions: the tag says which channels, in which order. The structure is
	 * still returned at full size, because a caller that allocates one and reads `mNumberChannelDescriptions`
	 * must not read past what it was given. */
	{
		AudioChannelLayout layout;

		memset(&layout, 0, sizeof(layout));
		layout.mChannelLayoutTag = tag;
		memcpy(data, &layout, sizeof(layout));
	}

	*data_size = sizeof(AudioChannelLayout);
	return 0;
}

static OSStatus fn_validate_layout(const void *specifier, UInt32 specifier_size,
                                   UInt32 *data_size, void *data)
{
	const AudioChannelLayout *layout = (const AudioChannelLayout *)specifier;

	if (layout == NULL || specifier_size < offsetof(AudioChannelLayout, mChannelDescriptions)) {
		return kAudioFormatBadSpecifierSizeError;
	}

	if (data == NULL) {
		return kAudioFormatBadPropertySizeError;
	}
	if (data_size == NULL || *data_size < sizeof(OSStatus)) {
		return kAudioFormatBadPropertySizeError;
	}

	{
		OSStatus verdict = 0;

		if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
			UInt32 needed = offsetof(AudioChannelLayout, mChannelDescriptions)
			              + layout->mNumberChannelDescriptions * sizeof(AudioChannelDescription);

			if (layout->mNumberChannelDescriptions == 0 || specifier_size < needed) {
				verdict = kAudioFormatUnsupportedDataFormatError;
			}
		} else if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
			if (fn_channels_in_bitmap(layout->mChannelBitmap) == 0) {
				verdict = kAudioFormatUnsupportedDataFormatError;
			}
		} else if ((layout->mChannelLayoutTag & 0xFFFFU) == 0) {
			verdict = kAudioFormatUnsupportedDataFormatError;
		}

		memcpy(data, &verdict, sizeof(verdict));
	}

	*data_size = sizeof(OSStatus);
	return 0;
}

OSStatus AudioFormatGetPropertyInfo(AudioFormatPropertyID  inPropertyID,
                                    UInt32                 inSpecifierSize,
                                    const void *           inSpecifier,
                                    UInt32 *               outPropertyDataSize)
{
	(void)inSpecifier;

	if (outPropertyDataSize == NULL) {
		return kAudio_ParamError;
	}

	switch (inPropertyID) {
	case kAudioFormatProperty_FormatInfo:
		if (inSpecifier == NULL || inSpecifierSize < sizeof(AudioFormatInfo)) {
			return kAudioFormatBadSpecifierSizeError;
		}
		*outPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioFormatProperty_NumberOfChannelsForLayout:
		*outPropertyDataSize = sizeof(UInt32);
		return 0;

	case kAudioFormatProperty_ChannelLayoutForTag:
		*outPropertyDataSize = sizeof(AudioChannelLayout);
		return 0;

	case kAudioFormatProperty_ValidateChannelLayout:
		*outPropertyDataSize = sizeof(OSStatus);
		return 0;

	default:
		return kAudioFormatUnsupportedPropertyError;
	}
}

OSStatus AudioFormatGetProperty(AudioFormatPropertyID  inPropertyID,
                                UInt32                 inSpecifierSize,
                                const void *           inSpecifier,
                                UInt32 *               ioPropertyDataSize,
                                void *                 outPropertyData)
{
	switch (inPropertyID) {
	case kAudioFormatProperty_FormatInfo:
		return fn_format_info(inSpecifier, inSpecifierSize, ioPropertyDataSize, outPropertyData);

	case kAudioFormatProperty_NumberOfChannelsForLayout:
		return fn_channels_for_layout(inSpecifier, inSpecifierSize, ioPropertyDataSize, outPropertyData);

	case kAudioFormatProperty_ChannelLayoutForTag:
		return fn_layout_for_tag(inSpecifier, inSpecifierSize, ioPropertyDataSize, outPropertyData);

	case kAudioFormatProperty_ValidateChannelLayout:
		return fn_validate_layout(inSpecifier, inSpecifierSize, ioPropertyDataSize, outPropertyData);

	default:
		return kAudioFormatUnsupportedPropertyError;
	}
}
