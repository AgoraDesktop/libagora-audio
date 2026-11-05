/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * api/prototypes.c — every function this library exports, taken through a pointer typed from the manifest.
 *
 * WHY THIS AND NOT A RUNTIME TEST: a changed parameter, a dropped `const`, a `UInt32 *` that became `UInt32`
 * — none of those is a link error, because C does not put the prototype in the symbol. What *is* an error is
 * assigning the function to a pointer whose type is written out from `api/manifest-10.12.txt`, and that
 * costs one compile per change and nothing at run time.
 *
 * NOTHING HERE IS EXECUTED. `main` only touches the pointers so the compiler cannot drop them; reaching it
 * means every prototype in the door matched the one Apple shipped.
 */

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

/* --- CoreAudio/HostTime.h -------------------------------------------------- */
static UInt64 (*p_AudioGetCurrentHostTime)(void) = AudioGetCurrentHostTime;
static Float64 (*p_AudioGetHostClockFrequency)(void) = AudioGetHostClockFrequency;
static UInt32 (*p_AudioGetHostClockMinimumTimeDelta)(void) = AudioGetHostClockMinimumTimeDelta;
static UInt64 (*p_AudioConvertHostTimeToNanos)(UInt64) = AudioConvertHostTimeToNanos;
static UInt64 (*p_AudioConvertNanosToHostTime)(UInt64) = AudioConvertNanosToHostTime;

/* --- AudioToolbox/AudioFormat.h -------------------------------------------- */
static OSStatus (*p_AudioFormatGetPropertyInfo)(AudioFormatPropertyID, UInt32, const void * _Nullable,
                                                UInt32 * _Nonnull) = AudioFormatGetPropertyInfo;
static OSStatus (*p_AudioFormatGetProperty)(AudioFormatPropertyID, UInt32, const void * _Nullable,
                                            UInt32 * _Nullable, void * _Nullable) = AudioFormatGetProperty;

/* --- AudioToolbox/AudioConverter.h ----------------------------------------- */
static OSStatus (*p_AudioConverterNew)(const AudioStreamBasicDescription * _Nonnull,
                                       const AudioStreamBasicDescription * _Nonnull,
                                       AudioConverterRef _Nullable * _Nonnull) = AudioConverterNew;
static OSStatus (*p_AudioConverterNewSpecific)(const AudioStreamBasicDescription * _Nonnull,
                                               const AudioStreamBasicDescription * _Nonnull,
                                               UInt32, const AudioClassDescription * _Nonnull,
                                               AudioConverterRef _Nullable * _Nonnull) = AudioConverterNewSpecific;
static OSStatus (*p_AudioConverterDispose)(AudioConverterRef _Nonnull) = AudioConverterDispose;
static OSStatus (*p_AudioConverterReset)(AudioConverterRef _Nonnull) = AudioConverterReset;
static OSStatus (*p_AudioConverterGetPropertyInfo)(AudioConverterRef _Nonnull, AudioConverterPropertyID,
                                                   UInt32 * _Nullable, Boolean * _Nullable)
    = AudioConverterGetPropertyInfo;
static OSStatus (*p_AudioConverterGetProperty)(AudioConverterRef _Nonnull, AudioConverterPropertyID,
                                               UInt32 * _Nonnull, void * _Nonnull) = AudioConverterGetProperty;
static OSStatus (*p_AudioConverterSetProperty)(AudioConverterRef _Nonnull, AudioConverterPropertyID, UInt32,
                                               const void * _Nonnull) = AudioConverterSetProperty;
static OSStatus (*p_AudioConverterConvertBuffer)(AudioConverterRef _Nonnull, UInt32, const void * _Nonnull,
                                                 UInt32 * _Nonnull, void * _Nonnull)
    = AudioConverterConvertBuffer;
static OSStatus (*p_AudioConverterFillComplexBuffer)(AudioConverterRef _Nonnull,
                                                     AudioConverterComplexInputDataProc _Nonnull,
                                                     void * _Nullable, UInt32 * _Nonnull,
                                                     AudioBufferList * _Nonnull,
                                                     AudioStreamPacketDescription * _Nullable)
    = AudioConverterFillComplexBuffer;
static OSStatus (*p_AudioConverterFillBuffer)(AudioConverterRef _Nonnull,
                                              AudioConverterInputDataProc _Nonnull, void * _Nullable,
                                              UInt32 * _Nonnull, void * _Nonnull) = AudioConverterFillBuffer;

int main(void);
int main(void)
{
	/* Touch every pointer so nothing above is optimised away. The values are never called: the types are
	 * the assertion, and this file is a compile-time gate with a linker's edge. */
	void *unused[] = {
		(void *)p_AudioGetCurrentHostTime, (void *)p_AudioGetHostClockFrequency,
		(void *)p_AudioGetHostClockMinimumTimeDelta, (void *)p_AudioConvertHostTimeToNanos,
		(void *)p_AudioConvertNanosToHostTime,
		(void *)p_AudioFormatGetPropertyInfo, (void *)p_AudioFormatGetProperty,
		(void *)p_AudioConverterNew, (void *)p_AudioConverterNewSpecific,
		(void *)p_AudioConverterDispose, (void *)p_AudioConverterReset,
		(void *)p_AudioConverterGetPropertyInfo, (void *)p_AudioConverterGetProperty,
		(void *)p_AudioConverterSetProperty, (void *)p_AudioConverterConvertBuffer,
		(void *)p_AudioConverterFillComplexBuffer, (void *)p_AudioConverterFillBuffer,
	};

	return unused[0] == NULL ? 0 : 0;
}
