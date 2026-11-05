/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * format.c — the format property door: what it answers, and what it refuses.
 *
 * THE FRAME ARITHMETIC IS THE POINT. "How many bytes is a frame" is the question every layer above asks,
 * and every layer above gets it wrong once: a non-interleaved description counts ONE channel in
 * `mBytesPerFrame`, and a 24-bit sample in a 32-bit container is four bytes, not three. Both are asserted
 * here with numbers, because both are silent when wrong — the buffer is just short by half.
 */

#include <AudioToolbox/AudioFormat.h>

#include <stdio.h>
#include <string.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

static AudioStreamBasicDescription fn_asbd(UInt32 channels, UInt32 bits, AudioFormatFlags flags)
{
	AudioStreamBasicDescription asbd;

	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate = 44100.0;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = flags;
	asbd.mChannelsPerFrame = channels;
	asbd.mBitsPerChannel = bits;

	return asbd;
}

static void fn_test_format_info(void)
{
	AudioFormatInfo info;
	AudioStreamBasicDescription filled;
	UInt32 size = sizeof(filled);
	OSStatus status;

	/* A description missing its frame arithmetic: stereo, 16-bit, packed, signed, interleaved. */
	memset(&info, 0, sizeof(info));
	info.mASBD = fn_asbd(2, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);

	size = sizeof(filled);
	status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, sizeof(info), &info, &size, &filled);
	fn_check("FormatInfo answers for a half-written PCM description", status == 0);
	fn_check("FormatInfo reports the size it wrote",
	         size == sizeof(AudioStreamBasicDescription));
	fn_check("stereo 16-bit frame is 4 bytes", filled.mBytesPerFrame == 4);
	fn_check("packet is the frame", filled.mBytesPerPacket == 4);
	fn_check("one frame per packet", filled.mFramesPerPacket == 1);

	/* The same, non-interleaved: the frame is ONE channel. */
	memset(&info, 0, sizeof(info));
	info.mASBD = fn_asbd(2, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
	                            | kAudioFormatFlagIsNonInterleaved);
	size = sizeof(filled);
	status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, sizeof(info), &info, &size, &filled);
	fn_check("FormatInfo answers for a non-interleaved description", status == 0);
	fn_check("a non-interleaved frame is one channel's sample", filled.mBytesPerFrame == 2);

	/* 24 significant bits in a 32-bit container: the stride comes from the container, not the bits. */
	memset(&info, 0, sizeof(info));
	info.mASBD = fn_asbd(1, 24, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
	                          | kAudioFormatFlagIsAlignedHigh);
	info.mASBD.mBytesPerFrame = 4;
	info.mASBD.mBytesPerPacket = 4;
	info.mASBD.mFramesPerPacket = 1;
	size = sizeof(filled);
	status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, sizeof(info), &info, &size, &filled);
	fn_check("24-bit in a 4-byte container keeps its 4-byte frame",
	         status == 0 && filled.mBytesPerFrame == 4);

	/* A format with no codec here is refused, by name. */
	memset(&info, 0, sizeof(info));
	info.mASBD = fn_asbd(2, 16, 0);
	info.mASBD.mFormatID = kAudioFormatMPEG4AAC;
	size = sizeof(filled);
	status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, sizeof(info), &info, &size, &filled);
	fn_check("AAC has no format-info answer here",
	         status == kAudioFormatUnsupportedDataFormatError);

	/* And a specifier that is too small is told so rather than read past. */
	size = sizeof(filled);
	status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, sizeof(AudioFormatInfo) - 1, &info,
	                                &size, &filled);
	fn_check("a short specifier is refused",
	         status == kAudioFormatBadSpecifierSizeError);
}

static void fn_test_layouts(void)
{
	AudioChannelLayoutTag tag = kAudioChannelLayoutTag_Stereo;
	AudioChannelLayout layout;
	UInt32 size;
	OSStatus status;

	size = 0;
	status = AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &size);
	fn_check("ChannelLayoutForTag has a size", status == 0 && size >= sizeof(AudioChannelLayout));

	size = sizeof(layout);
	status = AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &size, &layout);
	fn_check("ChannelLayoutForTag answers", status == 0);
	fn_check("the tag is echoed", layout.mChannelLayoutTag == kAudioChannelLayoutTag_Stereo);
	fn_check("a named layout needs no descriptions", layout.mNumberChannelDescriptions == 0);

	/* Stereo names two channels; the count is in the tag's low half. */
	memset(&layout, 0, sizeof(layout));
	layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
	size = sizeof(UInt32);
	{
		UInt32 channels = 0;

		status = AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout,
		                                sizeof(layout), &layout, &size, &channels);
		fn_check("a stereo tag names two channels", status == 0 && channels == 2);
	}

	/* A bitmap counts its bits. */
	memset(&layout, 0, sizeof(layout));
	layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
	layout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | kAudioChannelBit_Center;
	{
		UInt32 channels = 0;

		size = sizeof(UInt32);
		status = AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout,
		                                sizeof(layout), &layout, &size, &channels);
		fn_check("a bitmap counts the channels it names", status == 0 && channels == 3);
	}

	/* Validate: a named layout is fine, an empty description list is not. */
	memset(&layout, 0, sizeof(layout));
	layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A;
	{
		OSStatus verdict = -1;

		size = sizeof(verdict);
		status = AudioFormatGetProperty(kAudioFormatProperty_ValidateChannelLayout,
		                                sizeof(layout), &layout, &size, &verdict);
		fn_check("a named layout validates", status == 0 && verdict == 0);
	}

	memset(&layout, 0, sizeof(layout));
	layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
	layout.mNumberChannelDescriptions = 0;
	{
		OSStatus verdict = 0;

		size = sizeof(verdict);
		status = AudioFormatGetProperty(kAudioFormatProperty_ValidateChannelLayout,
		                                sizeof(layout), &layout, &size, &verdict);
		fn_check("a description list with no descriptions does not validate",
		         status == 0 && verdict == kAudioFormatUnsupportedDataFormatError);
	}

	/* Two tags that are instructions rather than layouts are refused as layouts. */
	tag = kAudioChannelLayoutTag_UseChannelDescriptions;
	size = sizeof(layout);
	status = AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &size, &layout);
	fn_check("UseChannelDescriptions is not a layout tag answer",
	         status == kAudioFormatUnsupportedDataFormatError);
}

static void fn_test_refusals(void)
{
	UInt32 size = 0;

	fn_check("an unknown selector is refused by name",
	         AudioFormatGetPropertyInfo(0x7A7A7A7A, 0, NULL, &size) == kAudioFormatUnsupportedPropertyError);
	fn_check("an unknown selector is refused by name, by the other door",
	         AudioFormatGetProperty(0x7A7A7A7A, 0, NULL, &size, NULL) == kAudioFormatUnsupportedPropertyError);
}

int main(void);
int main(void)
{
	fn_test_format_info();
	fn_test_layouts();
	fn_test_refusals();

	if (g_failures != 0) {
		fprintf(stderr, "format: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("format: frame arithmetic, layouts, and four refusals answered\n");
	return 0;
}
