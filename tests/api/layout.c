/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * api/layout.c — every size, alignment and field offset, against the manifest.
 *
 * EACH NUMBER HERE COMES FROM `api/manifest-10.12.txt`, WHICH IS GENERATED FROM `MacOSX10.12.sdk` BY
 * `tools/api-manifest.py`. They are written out rather than parsed at build time because a test that
 * reads the answer it is checking can pass for the wrong reason; the manifest is the record and this
 * file is the transcription, and a change to either is visible in a diff.
 *
 * WHY IT MATTERS: a client allocates `AudioBufferList` with room for N buffers after the one Apple
 * declares, and indexes `mBuffers` past the end. If our `mBuffers` is at a different offset, or the
 * structure is a different size, that client writes into its own stack frame — no diagnostic, no
 * error code, just a crash somewhere else.
 */

#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>

#include <stddef.h>

#define AGORA_SIZE(type, expected) \
	_Static_assert(sizeof(type) == (expected), #type " is not " #expected " bytes")

#define AGORA_ALIGN(type, expected) \
	_Static_assert(_Alignof(type) == (expected), #type " is not " #expected "-aligned")

#define AGORA_FIELD(type, field, expected) \
	_Static_assert(offsetof(type, field) == (expected), #type "." #field " is not at " #expected)

/* --- the records, size and alignment --------------------------------------- */
AGORA_SIZE(AudioValueRange, 16);
AGORA_SIZE(AudioValueTranslation, 32);
AGORA_SIZE(AudioBuffer, 16);
AGORA_SIZE(AudioBufferList, 24);
AGORA_SIZE(AudioStreamBasicDescription, 40);
AGORA_SIZE(AudioStreamPacketDescription, 16);
AGORA_SIZE(SMPTETime, 24);
AGORA_SIZE(AudioTimeStamp, 64);
AGORA_SIZE(AudioClassDescription, 12);
AGORA_SIZE(AudioChannelDescription, 20);
AGORA_SIZE(AudioChannelLayout, 32);

AGORA_ALIGN(AudioValueRange, 8);
AGORA_ALIGN(AudioBufferList, 8);
AGORA_ALIGN(AudioStreamBasicDescription, 8);
AGORA_ALIGN(SMPTETime, 4);
AGORA_ALIGN(AudioTimeStamp, 8);
AGORA_ALIGN(AudioChannelLayout, 4);

/* --- AudioStreamBasicDescription, field for field -------------------------- */
AGORA_FIELD(AudioStreamBasicDescription, mSampleRate, 0);
AGORA_FIELD(AudioStreamBasicDescription, mFormatID, 8);
AGORA_FIELD(AudioStreamBasicDescription, mFormatFlags, 12);
AGORA_FIELD(AudioStreamBasicDescription, mBytesPerPacket, 16);
AGORA_FIELD(AudioStreamBasicDescription, mFramesPerPacket, 20);
AGORA_FIELD(AudioStreamBasicDescription, mBytesPerFrame, 24);
AGORA_FIELD(AudioStreamBasicDescription, mChannelsPerFrame, 28);
AGORA_FIELD(AudioStreamBasicDescription, mBitsPerChannel, 32);
AGORA_FIELD(AudioStreamBasicDescription, mReserved, 36);

/* --- AudioBufferList: the one whose `[1]` a client allocates past ------------ */
AGORA_FIELD(AudioBufferList, mNumberBuffers, 0);
AGORA_FIELD(AudioBufferList, mBuffers, 8);
AGORA_FIELD(AudioBuffer, mNumberChannels, 0);
AGORA_FIELD(AudioBuffer, mDataByteSize, 4);
AGORA_FIELD(AudioBuffer, mData, 8);

/* --- the time stamps: four clocks and the flag word that vouches for them --- */
AGORA_FIELD(AudioTimeStamp, mSampleTime, 0);
AGORA_FIELD(AudioTimeStamp, mHostTime, 8);
AGORA_FIELD(AudioTimeStamp, mRateScalar, 16);
AGORA_FIELD(AudioTimeStamp, mWordClockTime, 24);
AGORA_FIELD(AudioTimeStamp, mSMPTETime, 32);
AGORA_FIELD(AudioTimeStamp, mFlags, 56);
AGORA_FIELD(AudioTimeStamp, mReserved, 60);
AGORA_FIELD(SMPTETime, mSubframes, 0);
AGORA_FIELD(SMPTETime, mSubframeDivisor, 2);
AGORA_FIELD(SMPTETime, mCounter, 4);
AGORA_FIELD(SMPTETime, mType, 8);
AGORA_FIELD(SMPTETime, mFlags, 12);
AGORA_FIELD(SMPTETime, mHours, 16);
AGORA_FIELD(SMPTETime, mMinutes, 18);
AGORA_FIELD(SMPTETime, mSeconds, 20);
AGORA_FIELD(SMPTETime, mFrames, 22);

/* --- the two variable-length channel structures ---------------------------- */
AGORA_FIELD(AudioChannelLayout, mChannelLayoutTag, 0);
AGORA_FIELD(AudioChannelLayout, mChannelBitmap, 4);
AGORA_FIELD(AudioChannelLayout, mNumberChannelDescriptions, 8);
AGORA_FIELD(AudioChannelLayout, mChannelDescriptions, 12);
AGORA_FIELD(AudioChannelDescription, mChannelLabel, 0);
AGORA_FIELD(AudioChannelDescription, mChannelFlags, 4);
AGORA_FIELD(AudioChannelDescription, mCoordinates, 8);

/* --- the HAL's object address, which every property call passes ------------ */
AGORA_SIZE(AudioObjectPropertyAddress, 12);
AGORA_ALIGN(AudioObjectPropertyAddress, 4);
AGORA_FIELD(AudioObjectPropertyAddress, mSelector, 0);
AGORA_FIELD(AudioObjectPropertyAddress, mScope, 4);
AGORA_FIELD(AudioObjectPropertyAddress, mElement, 8);

/* --- the AudioToolbox doors' records --------------------------------------- */
AGORA_SIZE(AudioFormatInfo, 56);
AGORA_SIZE(AudioFormatListItem, 48);
AGORA_SIZE(AudioConverterPrimeInfo, 8);
AGORA_ALIGN(AudioFormatInfo, 8);
AGORA_ALIGN(AudioFormatListItem, 8);
AGORA_FIELD(AudioFormatInfo, mASBD, 0);
AGORA_FIELD(AudioFormatInfo, mMagicCookie, 40);
AGORA_FIELD(AudioFormatInfo, mMagicCookieSize, 48);
AGORA_FIELD(AudioFormatListItem, mASBD, 0);
AGORA_FIELD(AudioFormatListItem, mChannelLayoutTag, 40);
AGORA_FIELD(AudioConverterPrimeInfo, leadingFrames, 0);
AGORA_FIELD(AudioConverterPrimeInfo, trailingFrames, 4);

/* --- the gapless declaration a compressed file answers ---------------------- */
AGORA_SIZE(AudioFilePacketTableInfo, 16);
AGORA_ALIGN(AudioFilePacketTableInfo, 8);
AGORA_FIELD(AudioFilePacketTableInfo, mNumberValidFrames, 0);
AGORA_FIELD(AudioFilePacketTableInfo, mPrimingFrames, 8);
AGORA_FIELD(AudioFilePacketTableInfo, mRemainderFrames, 12);

/* --- packet descriptions and translations ---------------------------------- */
AGORA_FIELD(AudioStreamPacketDescription, mStartOffset, 0);
AGORA_FIELD(AudioStreamPacketDescription, mVariableFramesInPacket, 8);
AGORA_FIELD(AudioStreamPacketDescription, mDataByteSize, 12);
AGORA_FIELD(AudioValueTranslation, mInputData, 0);
AGORA_FIELD(AudioValueTranslation, mInputDataSize, 8);
AGORA_FIELD(AudioValueTranslation, mOutputData, 16);
AGORA_FIELD(AudioValueTranslation, mOutputDataSize, 24);
AGORA_FIELD(AudioValueRange, mMinimum, 0);
AGORA_FIELD(AudioValueRange, mMaximum, 8);
AGORA_FIELD(AudioClassDescription, mType, 0);
AGORA_FIELD(AudioClassDescription, mSubType, 4);
AGORA_FIELD(AudioClassDescription, mManufacturer, 8);

/* --- the parameter description an AUv3 tree is built from ------------------ */
AGORA_SIZE(AudioUnitParameterInfo, 104);
AGORA_ALIGN(AudioUnitParameterInfo, 8);
AGORA_FIELD(AudioUnitParameterInfo, name, 0);
AGORA_FIELD(AudioUnitParameterInfo, unitName, 56);
AGORA_FIELD(AudioUnitParameterInfo, clumpID, 64);
AGORA_FIELD(AudioUnitParameterInfo, cfNameString, 72);
AGORA_FIELD(AudioUnitParameterInfo, unit, 80);
AGORA_FIELD(AudioUnitParameterInfo, minValue, 84);
AGORA_FIELD(AudioUnitParameterInfo, maxValue, 88);
AGORA_FIELD(AudioUnitParameterInfo, defaultValue, 92);
AGORA_FIELD(AudioUnitParameterInfo, flags, 96);

int main(void);
int main(void)
{
	/* Everything is asserted at compile time; reaching here means the declarations agreed with the
	 * manifest, and the exit status is the only thing ctest needs. */
	return 0;
}
