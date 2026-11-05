/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * api/macros.c — the constants and the compile-time macros, checked by value.
 *
 * THE VALUES ARE FROM `api/manifest-10.12.txt` (generated from the 10.12 SDK). They are checked at
 * run time, not only at compile time, because the interesting failures are silent: a format ID that
 * lost its four-char spelling still compiles, and a flag combination that decodes as the wrong
 * endianness still plays — as noise, on one platform, for whoever is listening.
 *
 * THE MACRO IS THE POINT OF THIS FILE. `TestAudioFormatNativeEndian` is a compile-time expression
 * over a structure, and being right for a float and wrong for a big-endian integer is exactly the
 * bug that no amount of type checking catches.
 */

#include <CoreAudio/CoreAudio.h>

#include <stdio.h>

static int g_failures;

static void fn_check(const char *what, long got, long expected)
{
	if (got != expected) {
		fprintf(stderr, "FAIL %s: got %ld, expected %ld\n", what, got, expected);
		g_failures++;
	}
}

int main(void);
int main(void)
{
	/* --- OSStatus codes: the numbers clients switch on. --------------------- */
	fn_check("kAudio_UnimplementedError", kAudio_UnimplementedError, -4);
	fn_check("kAudio_FileNotFoundError", kAudio_FileNotFoundError, -43);
	fn_check("kAudio_FilePermissionError", kAudio_FilePermissionError, -54);
	fn_check("kAudio_TooManyFilesOpenError", kAudio_TooManyFilesOpenError, -42);
	fn_check("kAudio_BadFilePathError", kAudio_BadFilePathError, 0x21707468); /* '!pth' */
	fn_check("kAudio_ParamError", kAudio_ParamError, -50);
	fn_check("kAudio_MemFullError", kAudio_MemFullError, -108);

	/* --- format IDs: four-char codes, spelled out because a typo is invisible. */
	fn_check("kAudioFormatLinearPCM", kAudioFormatLinearPCM, 1819304813);   /* 'lpcm' */
	fn_check("kAudioFormatMPEG4AAC", kAudioFormatMPEG4AAC, 1633772320);     /* 'aac ' */
	fn_check("kAudioFormatMPEGLayer3", kAudioFormatMPEGLayer3, 778924083);  /* '.mp3' */
	fn_check("kAudioFormatAppleLossless", kAudioFormatAppleLossless, 1634492771); /* 'alac' */

	/* --- flags: the bit each one is, and the three combinations. ------------ */
	fn_check("kAudioFormatFlagIsFloat", kAudioFormatFlagIsFloat, 1);
	fn_check("kAudioFormatFlagIsBigEndian", kAudioFormatFlagIsBigEndian, 2);
	fn_check("kAudioFormatFlagIsSignedInteger", kAudioFormatFlagIsSignedInteger, 4);
	fn_check("kAudioFormatFlagIsPacked", kAudioFormatFlagIsPacked, 8);
	fn_check("kAudioFormatFlagIsAlignedHigh", kAudioFormatFlagIsAlignedHigh, 16);
	fn_check("kAudioFormatFlagIsNonInterleaved", kAudioFormatFlagIsNonInterleaved, 32);
	fn_check("kAudioFormatFlagIsNonMixable", kAudioFormatFlagIsNonMixable, 64);
	fn_check("kAudioFormatFlagsAreAllClear", (long)(UInt32)kAudioFormatFlagsAreAllClear, 0x80000000L);
	fn_check("kLinearPCMFormatFlagsSampleFractionShift",
	         kLinearPCMFormatFlagsSampleFractionShift, 7);
	fn_check("kLinearPCMFormatFlagsSampleFractionMask",
	         kLinearPCMFormatFlagsSampleFractionMask, 0x1F80);
	fn_check("kAudioFormatFlagsNativeEndian", kAudioFormatFlagsNativeEndian, 0);
	fn_check("kAudioFormatFlagsNativeFloatPacked", kAudioFormatFlagsNativeFloatPacked, 9);
	fn_check("kAudioFormatFlagsCanonical", kAudioFormatFlagsCanonical, 9);
	fn_check("kAudioFormatFlagsAudioUnitCanonical", kAudioFormatFlagsAudioUnitCanonical, 41);
	fn_check("kAppleLosslessFormatFlag_24BitSourceData",
	         kAppleLosslessFormatFlag_24BitSourceData, 3);

	/* --- time: the SMPTE table and the timestamp flags. --------------------- */
	fn_check("kSMPTETimeType24", kSMPTETimeType24, 0);
	fn_check("kSMPTETimeType2997Drop", kSMPTETimeType2997Drop, 5);
	fn_check("kSMPTETimeType2398", kSMPTETimeType2398, 11);
	fn_check("kSMPTETimeValid", kSMPTETimeValid, 1);
	fn_check("kAudioTimeStampSampleTimeValid", kAudioTimeStampSampleTimeValid, 1);
	fn_check("kAudioTimeStampHostTimeValid", kAudioTimeStampHostTimeValid, 2);
	fn_check("kAudioTimeStampRateScalarValid", kAudioTimeStampRateScalarValid, 4);
	fn_check("kAudioTimeStampWordClockTimeValid", kAudioTimeStampWordClockTimeValid, 8);
	fn_check("kAudioTimeStampSMPTETimeValid", kAudioTimeStampSMPTETimeValid, 16);
	fn_check("kAudioTimeStampSampleHostTimeValid", kAudioTimeStampSampleHostTimeValid, 3);

	/* --- channels: labels, bits, and the (label << 16) | count layout tags. -- */
	fn_check("kAudioChannelLabel_UseCoordinates", kAudioChannelLabel_UseCoordinates, 100);
	fn_check("kAudioChannelBit_Left", kAudioChannelBit_Left, 1);
	fn_check("kAudioChannelBit_LFEScreen", kAudioChannelBit_LFEScreen, 8);
	fn_check("kAudioChannelFlags_Meters", kAudioChannelFlags_Meters, 4);
	fn_check("kAudioChannelCoordinates_Azimuth", kAudioChannelCoordinates_Azimuth, 0);
	fn_check("kAudioChannelCoordinates_DownUp", kAudioChannelCoordinates_DownUp, 2);
	fn_check("kAudioChannelLayoutTag_UseChannelDescriptions",
	         kAudioChannelLayoutTag_UseChannelDescriptions, 0);
	fn_check("kAudioChannelLayoutTag_UseChannelBitmap", kAudioChannelLayoutTag_UseChannelBitmap, 65536);
	fn_check("kAudioChannelLayoutTag_Mono", kAudioChannelLayoutTag_Mono, 6553601);
	fn_check("kAudioChannelLayoutTag_Stereo", kAudioChannelLayoutTag_Stereo, 6619138);
	fn_check("kAudioChannelLayoutTag_StereoHeadphones",
	         kAudioChannelLayoutTag_StereoHeadphones, 6684674);
	fn_check("kAudioChannelLayoutTag_MPEG_5_1_A", kAudioChannelLayoutTag_MPEG_5_1_A, 7929862);
	fn_check("kAudioChannelLayoutTag_MPEG_7_1_A", kAudioChannelLayoutTag_MPEG_7_1_A, 8257544);

	/* --- the macro: same format, four answers. ----------------------------- */
	{
		AudioStreamBasicDescription native_le = {0}, native_be = {0}, native_other = {0};

		native_le.mFormatID = kAudioFormatLinearPCM;
		native_le.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;

		native_be.mFormatID = kAudioFormatLinearPCM;
		native_be.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsBigEndian
		                       | kAudioFormatFlagIsPacked;

		native_other.mFormatID = kAudioFormatAppleLossless; /* not linear PCM at all */
		native_other.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;

		/* On this platform the native endianness is little, so the packed-float ASBD is native
		 * and the big-endian one is not. Written as `!= 0`/`== 0` rather than true/false so the
		 * expectation reads as the platform fact it is. */
		fn_check("TestAudioFormatNativeEndian(native LE)", TestAudioFormatNativeEndian(native_le) != 0, 1);
		fn_check("TestAudioFormatNativeEndian(native BE)", TestAudioFormatNativeEndian(native_be) != 0, 0);
		fn_check("TestAudioFormatNativeEndian(non-PCM)", TestAudioFormatNativeEndian(native_other) != 0, 0);
	}

	if (g_failures != 0) {
		fprintf(stderr, "api-macros: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("api-macros: all constants and the endian macro match the 10.12 manifest\n");
	return 0;
}
