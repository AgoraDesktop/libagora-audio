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

#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioToolbox/ExtendedAudioFile.h>

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

	/* --- AudioToolbox: the selectors and the errors clients switch on. ----- */
	fn_check("kAudioFormatProperty_FormatInfo", kAudioFormatProperty_FormatInfo, 1718449257);
	fn_check("kAudioFormatProperty_NumberOfChannelsForLayout",
	         kAudioFormatProperty_NumberOfChannelsForLayout, 1852008557);
	fn_check("kAudioFormatProperty_ChannelLayoutForTag",
	         kAudioFormatProperty_ChannelLayoutForTag, 1668116588);
	fn_check("kAudioFormatUnsupportedPropertyError", kAudioFormatUnsupportedPropertyError, 1886547824);
	fn_check("kAudioFormatBadPropertySizeError", kAudioFormatBadPropertySizeError, 561211770);
	fn_check("kAudioFormatBadSpecifierSizeError", kAudioFormatBadSpecifierSizeError, 561213539);
	fn_check("kAudioFormatUnsupportedDataFormatError", kAudioFormatUnsupportedDataFormatError, 1718449215);
	fn_check("kAudioFormatUnknownFormatError", kAudioFormatUnknownFormatError, 560360820);

	fn_check("kAudioConverterErr_FormatNotSupported", kAudioConverterErr_FormatNotSupported, 1718449215);
	fn_check("kAudioConverterErr_OperationNotSupported",
	         kAudioConverterErr_OperationNotSupported, 1869627199);
	fn_check("kAudioConverterErr_PropertyNotSupported",
	         kAudioConverterErr_PropertyNotSupported, 1886547824);
	fn_check("kAudioConverterErr_UnspecifiedError", kAudioConverterErr_UnspecifiedError, 2003329396);
	fn_check("kAudioConverterErr_InputSampleRateOutOfRange",
	         kAudioConverterErr_InputSampleRateOutOfRange, 560558962);
	fn_check("kAudioConverterQuality_Max", kAudioConverterQuality_Max, 0x7F);
	fn_check("kAudioConverterQuality_High", kAudioConverterQuality_High, 0x60);
	fn_check("kAudioConverterQuality_Min", kAudioConverterQuality_Min, 0);
	fn_check("kAudioConverterSampleRateConverterComplexity_Linear",
	         kAudioConverterSampleRateConverterComplexity_Linear, 1818848869);
	fn_check("kAudioConverterSampleRateConverterComplexity_Normal",
	         kAudioConverterSampleRateConverterComplexity_Normal, 1852797549);
	fn_check("kConverterPrimeMethod_Pre", kConverterPrimeMethod_Pre, 0);
	fn_check("kConverterPrimeMethod_None", kConverterPrimeMethod_None, 2);
	fn_check("kAudioConverterCurrentInputStreamDescription",
	         kAudioConverterCurrentInputStreamDescription, 1633904996);   /* 'acid' */
	fn_check("kAudioConverterCurrentOutputStreamDescription",
	         kAudioConverterCurrentOutputStreamDescription, 1633906532);  /* 'acod' */
	fn_check("kAudioConverterChannelMap", kAudioConverterChannelMap, 1667788144);

	/* --- AudioToolbox/AudioFile: containers, permissions, properties. ------ */
	fn_check("kAudioFileWAVEType", kAudioFileWAVEType, 1463899717);     /* 'WAVE' */
	fn_check("kAudioFileAIFFType", kAudioFileAIFFType, 1095321158);     /* 'AIFF' */
	fn_check("kAudioFileCAFType", kAudioFileCAFType, 1667327590);       /* 'caff' */
	fn_check("kAudioFileMP3Type", kAudioFileMP3Type, 1297106739);       /* 'MPG3', not 'mp3 ' */
	fn_check("kAudioFileReadPermission", kAudioFileReadPermission, 1);
	fn_check("kAudioFileWritePermission", kAudioFileWritePermission, 2);
	fn_check("kAudioFileReadWritePermission", kAudioFileReadWritePermission, 3);
	fn_check("kAudioFileFlags_EraseFile", kAudioFileFlags_EraseFile, 1);
	fn_check("kAudioFileUnsupportedFileTypeError", kAudioFileUnsupportedFileTypeError, 1954115647);
	fn_check("kAudioFileUnsupportedDataFormatError", kAudioFileUnsupportedDataFormatError, 1718449215);
	fn_check("kAudioFileUnsupportedPropertyError", kAudioFileUnsupportedPropertyError, 1886681407);
	fn_check("kAudioFileBadPropertySizeError", kAudioFileBadPropertySizeError, 561211770);
	fn_check("kAudioFileNotOpenError", kAudioFileNotOpenError, -38);
	fn_check("kAudioFilePropertyFileFormat", kAudioFilePropertyFileFormat, 1717988724);
	fn_check("kAudioFilePropertyDataOffset", kAudioFilePropertyDataOffset, 1685022310);
	fn_check("kAudioFilePropertyAudioDataByteCount", kAudioFilePropertyAudioDataByteCount, 1650683508);
	fn_check("kAudioFilePropertyAudioDataPacketCount", kAudioFilePropertyAudioDataPacketCount, 1885564532);
	fn_check("kExtAudioFileProperty_ClientDataFormat", kExtAudioFileProperty_ClientDataFormat, 1667657076);
	fn_check("kAudioFileStreamProperty_DataFormat", kAudioFileStreamProperty_DataFormat, 1684434292);
	fn_check("kAudioFileStreamProperty_DataOffset", kAudioFileStreamProperty_DataOffset, 1685022310);
	fn_check("kAudioFileStreamProperty_ReadyToProducePackets",
	         kAudioFileStreamProperty_ReadyToProducePackets, 1919247481);
	fn_check("kAudioFileStreamError_IllegalOperation", kAudioFileStreamError_IllegalOperation, 1852797029);
	fn_check("kAudioFileStreamError_InvalidFile", kAudioFileStreamError_InvalidFile, 1685348671);
	fn_check("kAudioFileStreamParseFlag_Discontinuity", kAudioFileStreamParseFlag_Discontinuity, 1);

	/* --- the HAL: object ids, scopes, selectors, errors. ------------------- */
	fn_check("kAudioObjectSystemObject", kAudioObjectSystemObject, 1);
	fn_check("kAudioObjectUnknown", kAudioObjectUnknown, 0);
	fn_check("kAudioObjectPropertyScopeGlobal", kAudioObjectPropertyScopeGlobal, 1735159650);
	fn_check("kAudioObjectPropertyScopeInput", kAudioObjectPropertyScopeInput, 1768845428);
	fn_check("kAudioObjectPropertyScopeOutput", kAudioObjectPropertyScopeOutput, 1869968496);
	fn_check("kAudioObjectPropertyElementMaster", kAudioObjectPropertyElementMaster, 0);
	fn_check("kAudioHardwarePropertyDevices", kAudioHardwarePropertyDevices, 1684370979);
	fn_check("kAudioHardwarePropertyDefaultOutputDevice",
	         kAudioHardwarePropertyDefaultOutputDevice, 1682929012);
	fn_check("kAudioDevicePropertyDeviceNameCFString", kAudioDevicePropertyDeviceNameCFString, 1819173229);
	fn_check("kAudioDevicePropertyVolumeScalar", kAudioDevicePropertyVolumeScalar, 1987013741);
	fn_check("kAudioDevicePropertyBufferFrameSize", kAudioDevicePropertyBufferFrameSize, 1718839674);
	fn_check("kAudioStreamPropertyVirtualFormat", kAudioStreamPropertyVirtualFormat, 1936092532);
	fn_check("kAudioStreamPropertyDirection", kAudioStreamPropertyDirection, 1935960434);
	fn_check("kAudioHardwareUnknownPropertyError", kAudioHardwareUnknownPropertyError, 2003332927);
	fn_check("kAudioHardwareBadObjectError", kAudioHardwareBadObjectError, 560947818);
	fn_check("kAudioHardwareUnsupportedOperationError",
	         kAudioHardwareUnsupportedOperationError, 1970171760);
	fn_check("kAudioDeviceTransportTypeVirtual", kAudioDeviceTransportTypeVirtual, 1986622068);

	/* --- the parameter doors the AUv3 tree reads ---------------------------- */
	fn_check("kAudioUnitProperty_ParameterList", kAudioUnitProperty_ParameterList, 3);
	fn_check("kAudioUnitProperty_ParameterInfo", kAudioUnitProperty_ParameterInfo, 4);
	fn_check("kAudioUnitParameterUnit_Generic", kAudioUnitParameterUnit_Generic, 0);
	fn_check("kAudioUnitParameterUnit_Boolean", kAudioUnitParameterUnit_Boolean, 2);
	fn_check("kAudioUnitParameterUnit_LinearGain", kAudioUnitParameterUnit_LinearGain, 14);
	fn_check("kAudioUnitParameterUnit_CustomUnit", kAudioUnitParameterUnit_CustomUnit, 26);
	fn_check("kAudioUnitParameterFlag_ValuesHaveStrings", kAudioUnitParameterFlag_ValuesHaveStrings, 2097152);
	fn_check("kAudioUnitParameterFlag_IsReadable", kAudioUnitParameterFlag_IsReadable, 1073741824);
	fn_check("kAudioUnitParameterFlag_IsWritable", kAudioUnitParameterFlag_IsWritable, 2147483648);

	if (g_failures != 0) {
		fprintf(stderr, "api-macros: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("api-macros: all constants and the endian macro match the 10.12 manifest\n");
	return 0;
}
