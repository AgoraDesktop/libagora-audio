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
#include <AudioToolbox/ExtendedAudioFile.h>

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

/* --- AudioToolbox/AudioFile.h ---------------------------------------------- */
static OSStatus (*p_AudioFileOpenURL)(CFURLRef _Nonnull, AudioFilePermissions, AudioFileTypeID,
                                      AudioFileID _Nullable * _Nonnull) = AudioFileOpenURL;
static OSStatus (*p_AudioFileOpenWithCallbacks)(void * _Nonnull, AudioFile_ReadProc _Nonnull,
                                                AudioFile_WriteProc _Nullable,
                                                AudioFile_GetSizeProc _Nonnull,
                                                AudioFile_SetSizeProc _Nullable, AudioFileTypeID,
                                                AudioFileID _Nullable * _Nonnull) = AudioFileOpenWithCallbacks;
static OSStatus (*p_AudioFileCreateWithURL)(CFURLRef _Nonnull, AudioFileTypeID,
                                            const AudioStreamBasicDescription * _Nonnull, AudioFileFlags,
                                            AudioFileID _Nullable * _Nonnull) = AudioFileCreateWithURL;
static OSStatus (*p_AudioFileInitializeWithCallbacks)(void * _Nonnull, AudioFile_ReadProc _Nonnull,
                                                      AudioFile_WriteProc _Nonnull,
                                                      AudioFile_GetSizeProc _Nonnull,
                                                      AudioFile_SetSizeProc _Nonnull, AudioFileTypeID,
                                                      const AudioStreamBasicDescription * _Nonnull,
                                                      AudioFileFlags,
                                                      AudioFileID _Nullable * _Nonnull)
    = AudioFileInitializeWithCallbacks;
static OSStatus (*p_AudioFileClose)(AudioFileID _Nonnull) = AudioFileClose;
static OSStatus (*p_AudioFileOptimize)(AudioFileID _Nonnull) = AudioFileOptimize;
static OSStatus (*p_AudioFileGetPropertyInfo)(AudioFileID _Nonnull, AudioFilePropertyID,
                                              UInt32 * _Nullable, UInt32 * _Nullable)
    = AudioFileGetPropertyInfo;
static OSStatus (*p_AudioFileGetProperty)(AudioFileID _Nonnull, AudioFilePropertyID, UInt32 * _Nonnull,
                                          void * _Nonnull) = AudioFileGetProperty;
static OSStatus (*p_AudioFileSetProperty)(AudioFileID _Nonnull, AudioFilePropertyID, UInt32,
                                          const void * _Nonnull) = AudioFileSetProperty;
static OSStatus (*p_AudioFileReadBytes)(AudioFileID _Nonnull, Boolean, SInt64, UInt32 * _Nonnull,
                                        void * _Nonnull) = AudioFileReadBytes;
static OSStatus (*p_AudioFileWriteBytes)(AudioFileID _Nonnull, Boolean, SInt64, UInt32 * _Nonnull,
                                         const void * _Nonnull) = AudioFileWriteBytes;
static OSStatus (*p_AudioFileReadPacketData)(AudioFileID _Nonnull, Boolean, UInt32 * _Nonnull,
                                             AudioStreamPacketDescription * _Nullable, SInt64,
                                             UInt32 * _Nonnull, void * _Nullable)
    = AudioFileReadPacketData;
static OSStatus (*p_AudioFileReadPackets)(AudioFileID _Nonnull, Boolean, UInt32 * _Nonnull,
                                          AudioStreamPacketDescription * _Nullable, SInt64,
                                          UInt32 * _Nonnull, void * _Nullable) = AudioFileReadPackets;
static OSStatus (*p_AudioFileWritePackets)(AudioFileID _Nonnull, Boolean, UInt32,
                                           const AudioStreamPacketDescription * _Nullable, SInt64,
                                           UInt32 * _Nonnull, const void * _Nonnull)
    = AudioFileWritePackets;

/* --- AudioToolbox/ExtendedAudioFile.h -------------------------------------- */
static OSStatus (*p_ExtAudioFileOpenURL)(CFURLRef _Nonnull, ExtAudioFileRef _Nullable * _Nonnull)
    = ExtAudioFileOpenURL;
static OSStatus (*p_ExtAudioFileCreateWithURL)(CFURLRef _Nonnull, AudioFileTypeID,
                                               const AudioStreamBasicDescription * _Nonnull,
                                               const AudioChannelLayout * _Nullable, UInt32,
                                               ExtAudioFileRef _Nullable * _Nonnull)
    = ExtAudioFileCreateWithURL;
static OSStatus (*p_ExtAudioFileWrapAudioFileID)(AudioFileID _Nonnull, Boolean,
                                                 ExtAudioFileRef _Nullable * _Nonnull)
    = ExtAudioFileWrapAudioFileID;
static OSStatus (*p_ExtAudioFileDispose)(ExtAudioFileRef _Nonnull) = ExtAudioFileDispose;
static OSStatus (*p_ExtAudioFileRead)(ExtAudioFileRef _Nonnull, UInt32 * _Nonnull, AudioBufferList * _Nonnull)
    = ExtAudioFileRead;
static OSStatus (*p_ExtAudioFileWrite)(ExtAudioFileRef _Nonnull, UInt32,
                                       const AudioBufferList * _Nonnull) = ExtAudioFileWrite;
static OSStatus (*p_ExtAudioFileWriteAsync)(ExtAudioFileRef _Nonnull, UInt32,
                                            const AudioBufferList * _Nullable) = ExtAudioFileWriteAsync;
static OSStatus (*p_ExtAudioFileSeek)(ExtAudioFileRef _Nonnull, SInt64) = ExtAudioFileSeek;
static OSStatus (*p_ExtAudioFileTell)(ExtAudioFileRef _Nonnull, SInt64 * _Nonnull) = ExtAudioFileTell;
static OSStatus (*p_ExtAudioFileGetPropertyInfo)(ExtAudioFileRef _Nonnull, ExtAudioFilePropertyID,
                                                 UInt32 * _Nullable, Boolean * _Nullable)
    = ExtAudioFileGetPropertyInfo;
static OSStatus (*p_ExtAudioFileGetProperty)(ExtAudioFileRef _Nonnull, ExtAudioFilePropertyID,
                                             UInt32 * _Nonnull, void * _Nonnull)
    = ExtAudioFileGetProperty;
static OSStatus (*p_ExtAudioFileSetProperty)(ExtAudioFileRef _Nonnull, ExtAudioFilePropertyID, UInt32,
                                             const void * _Nonnull) = ExtAudioFileSetProperty;

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
		(void *)p_AudioFileOpenURL, (void *)p_AudioFileOpenWithCallbacks,
		(void *)p_AudioFileCreateWithURL, (void *)p_AudioFileInitializeWithCallbacks,
		(void *)p_AudioFileClose, (void *)p_AudioFileOptimize,
		(void *)p_AudioFileGetPropertyInfo, (void *)p_AudioFileGetProperty,
		(void *)p_AudioFileSetProperty, (void *)p_AudioFileReadBytes,
		(void *)p_AudioFileWriteBytes, (void *)p_AudioFileReadPacketData,
		(void *)p_AudioFileReadPackets, (void *)p_AudioFileWritePackets,
		(void *)p_ExtAudioFileOpenURL, (void *)p_ExtAudioFileCreateWithURL,
		(void *)p_ExtAudioFileWrapAudioFileID, (void *)p_ExtAudioFileDispose,
		(void *)p_ExtAudioFileRead, (void *)p_ExtAudioFileWrite,
		(void *)p_ExtAudioFileWriteAsync, (void *)p_ExtAudioFileSeek,
		(void *)p_ExtAudioFileTell, (void *)p_ExtAudioFileGetPropertyInfo,
		(void *)p_ExtAudioFileGetProperty, (void *)p_ExtAudioFileSetProperty,
	};

	return unused[0] == NULL ? 0 : 0;
}
