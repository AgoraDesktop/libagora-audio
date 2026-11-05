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

#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioToolbox/ExtendedAudioFile.h>

/* --- CoreAudio/HostTime.h -------------------------------------------------- */
static UInt64 (*p_AudioGetCurrentHostTime)(void) = AudioGetCurrentHostTime;
static Float64 (*p_AudioGetHostClockFrequency)(void) = AudioGetHostClockFrequency;
static UInt32 (*p_AudioGetHostClockMinimumTimeDelta)(void) = AudioGetHostClockMinimumTimeDelta;
static UInt64 (*p_AudioConvertHostTimeToNanos)(UInt64) = AudioConvertHostTimeToNanos;
static UInt64 (*p_AudioConvertNanosToHostTime)(UInt64) = AudioConvertNanosToHostTime;

/* --- CoreAudio/AudioHardware.h --------------------------------------------- */
static OSStatus (*p_AudioObjectGetPropertyData)(AudioObjectID, const AudioObjectPropertyAddress * _Nonnull,
                                                UInt32, const void * _Nullable, UInt32 * _Nonnull,
                                                void * _Nonnull) = AudioObjectGetPropertyData;
static OSStatus (*p_AudioObjectSetPropertyData)(AudioObjectID, const AudioObjectPropertyAddress * _Nonnull,
                                                UInt32, const void * _Nullable, UInt32,
                                                const void * _Nonnull) = AudioObjectSetPropertyData;
static Boolean (*p_AudioObjectHasProperty)(AudioObjectID, const AudioObjectPropertyAddress * _Nonnull)
    = AudioObjectHasProperty;
static OSStatus (*p_AudioObjectIsPropertySettable)(AudioObjectID,
                                                   const AudioObjectPropertyAddress * _Nonnull,
                                                   Boolean * _Nonnull) = AudioObjectIsPropertySettable;
static OSStatus (*p_AudioObjectGetPropertyDataSize)(AudioObjectID,
                                                    const AudioObjectPropertyAddress * _Nonnull, UInt32,
                                                    const void * _Nullable, UInt32 * _Nonnull)
    = AudioObjectGetPropertyDataSize;
static OSStatus (*p_AudioObjectAddPropertyListener)(AudioObjectID,
                                                    const AudioObjectPropertyAddress * _Nonnull,
                                                    AudioObjectPropertyListenerProc _Nonnull,
                                                    void * _Nullable) = AudioObjectAddPropertyListener;
static OSStatus (*p_AudioObjectRemovePropertyListener)(AudioObjectID,
                                                       const AudioObjectPropertyAddress * _Nonnull,
                                                       AudioObjectPropertyListenerProc _Nonnull,
                                                       void * _Nullable) = AudioObjectRemovePropertyListener;
static OSStatus (*p_AudioObjectAddPropertyListenerBlock)(AudioObjectID,
                                                         const AudioObjectPropertyAddress * _Nonnull,
                                                         dispatch_queue_t _Nullable,
                                                         AudioObjectPropertyListenerBlock _Nonnull)
    = AudioObjectAddPropertyListenerBlock;
static OSStatus (*p_AudioObjectRemovePropertyListenerBlock)(AudioObjectID,
                                                            const AudioObjectPropertyAddress * _Nonnull,
                                                            dispatch_queue_t _Nullable,
                                                            AudioObjectPropertyListenerBlock _Nonnull)
    = AudioObjectRemovePropertyListenerBlock;
static OSStatus (*p_AudioDeviceCreateIOProcID)(AudioObjectID, AudioDeviceIOProc _Nonnull, void * _Nullable,
                                               AudioDeviceIOProcID _Nullable * _Nonnull)
    = AudioDeviceCreateIOProcID;
static OSStatus (*p_AudioDeviceCreateIOProcIDWithBlock)(AudioDeviceIOProcID _Nullable * _Nonnull,
                                                        AudioObjectID, dispatch_queue_t _Nullable,
                                                        AudioDeviceIOBlock _Nonnull)
    = AudioDeviceCreateIOProcIDWithBlock;
static OSStatus (*p_AudioDeviceDestroyIOProcID)(AudioObjectID, AudioDeviceIOProcID _Nonnull)
    = AudioDeviceDestroyIOProcID;
static OSStatus (*p_AudioDeviceStart)(AudioObjectID, AudioDeviceIOProcID _Nullable) = AudioDeviceStart;
static OSStatus (*p_AudioDeviceStop)(AudioObjectID, AudioDeviceIOProcID _Nullable) = AudioDeviceStop;
static OSStatus (*p_AudioDeviceGetCurrentTime)(AudioObjectID, AudioTimeStamp * _Nonnull)
    = AudioDeviceGetCurrentTime;
static OSStatus (*p_AudioDeviceTranslateTime)(AudioObjectID, const AudioTimeStamp * _Nonnull,
                                              AudioTimeStamp * _Nonnull) = AudioDeviceTranslateTime;
static OSStatus (*p_AudioDeviceGetNearestStartTime)(AudioObjectID, AudioTimeStamp * _Nonnull, UInt32)
    = AudioDeviceGetNearestStartTime;
static OSStatus (*p_AudioDeviceAddIOProc)(AudioDeviceID, AudioDeviceIOProc _Nonnull, void * _Nullable)
    = AudioDeviceAddIOProc;
static OSStatus (*p_AudioDeviceRemoveIOProc)(AudioDeviceID, AudioDeviceIOProc _Nonnull)
    = AudioDeviceRemoveIOProc;
static OSStatus (*p_AudioDeviceGetProperty)(AudioDeviceID, UInt32, Boolean, AudioDevicePropertyID,
                                            UInt32 * _Nonnull, void * _Nonnull) = AudioDeviceGetProperty;
static OSStatus (*p_AudioDeviceSetProperty)(AudioDeviceID, const AudioTimeStamp * _Nullable, UInt32, Boolean,
                                            AudioDevicePropertyID, UInt32, const void * _Nonnull)
    = AudioDeviceSetProperty;
static OSStatus (*p_AudioDeviceGetPropertyInfo)(AudioDeviceID, UInt32, Boolean, AudioDevicePropertyID,
                                                UInt32 * _Nullable, Boolean * _Nullable)
    = AudioDeviceGetPropertyInfo;
static OSStatus (*p_AudioStreamGetProperty)(AudioStreamID, UInt32, AudioDevicePropertyID,
                                            UInt32 * _Nonnull, void * _Nonnull) = AudioStreamGetProperty;

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

/* --- AudioToolbox/AudioFileStream.h ---------------------------------------- */
static OSStatus (*p_AudioFileStreamOpen)(void * _Nullable,
                                         AudioFileStream_PropertyListenerProc _Nonnull,
                                         AudioFileStream_PacketsProc _Nonnull, AudioFileTypeID,
                                         AudioFileStreamID _Nullable * _Nonnull) = AudioFileStreamOpen;
static OSStatus (*p_AudioFileStreamParseBytes)(AudioFileStreamID _Nonnull, UInt32, const void * _Nonnull,
                                               AudioFileStreamParseFlags) = AudioFileStreamParseBytes;
static OSStatus (*p_AudioFileStreamGetPropertyInfo)(AudioFileStreamID _Nonnull,
                                                    AudioFileStreamPropertyID, UInt32 * _Nullable,
                                                    Boolean * _Nullable) = AudioFileStreamGetPropertyInfo;
static OSStatus (*p_AudioFileStreamGetProperty)(AudioFileStreamID _Nonnull, AudioFileStreamPropertyID,
                                                UInt32 * _Nonnull, void * _Nonnull)
    = AudioFileStreamGetProperty;
static OSStatus (*p_AudioFileStreamSetProperty)(AudioFileStreamID _Nonnull, AudioFileStreamPropertyID,
                                                UInt32, const void * _Nonnull) = AudioFileStreamSetProperty;
static OSStatus (*p_AudioFileStreamSeek)(AudioFileStreamID _Nonnull, SInt64, SInt64 * _Nonnull,
                                         AudioFileStreamSeekFlags * _Nonnull) = AudioFileStreamSeek;
static OSStatus (*p_AudioFileStreamClose)(AudioFileStreamID _Nonnull) = AudioFileStreamClose;

/* --- AudioToolbox/AudioQueue.h --------------------------------------------- */
static OSStatus (*p_AudioQueueNewOutput)(const AudioStreamBasicDescription * _Nonnull,
                                         AudioQueueOutputCallback _Nonnull, void * _Nullable,
                                         CFRunLoopRef _Nullable, CFStringRef _Nullable, UInt32,
                                         AudioQueueRef _Nullable * _Nonnull) = AudioQueueNewOutput;
static OSStatus (*p_AudioQueueNewOutputWithDispatchQueue)(AudioQueueRef _Nullable * _Nonnull,
                                                          const AudioStreamBasicDescription * _Nonnull,
                                                          UInt32, dispatch_queue_t _Nonnull,
                                                          AudioQueueOutputCallbackBlock _Nonnull)
    = AudioQueueNewOutputWithDispatchQueue;
static OSStatus (*p_AudioQueueAllocateBuffer)(AudioQueueRef _Nonnull, UInt32,
                                              AudioQueueBufferRef _Nullable * _Nonnull)
    = AudioQueueAllocateBuffer;
static OSStatus (*p_AudioQueueEnqueueBuffer)(AudioQueueRef _Nonnull, AudioQueueBufferRef _Nonnull, UInt32,
                                             const AudioStreamPacketDescription * _Nullable)
    = AudioQueueEnqueueBuffer;
static OSStatus (*p_AudioQueueStart)(AudioQueueRef _Nonnull, const AudioTimeStamp * _Nullable)
    = AudioQueueStart;
static OSStatus (*p_AudioQueueStop)(AudioQueueRef _Nonnull, Boolean) = AudioQueueStop;
static OSStatus (*p_AudioQueueDispose)(AudioQueueRef _Nonnull, Boolean) = AudioQueueDispose;
static OSStatus (*p_AudioQueueFreeBuffer)(AudioQueueRef _Nonnull, AudioQueueBufferRef _Nonnull)
    = AudioQueueFreeBuffer;
static OSStatus (*p_AudioQueueGetProperty)(AudioQueueRef _Nonnull, AudioQueuePropertyID, void * _Nonnull,
                                           UInt32 * _Nonnull) = AudioQueueGetProperty;
static OSStatus (*p_AudioQueueSetProperty)(AudioQueueRef _Nonnull, AudioQueuePropertyID,
                                           const void * _Nonnull, UInt32) = AudioQueueSetProperty;
static OSStatus (*p_AudioQueueSetParameter)(AudioQueueRef _Nonnull, AudioQueueParameterID,
                                            AudioQueueParameterValue) = AudioQueueSetParameter;
static OSStatus (*p_AudioQueueGetParameter)(AudioQueueRef _Nonnull, AudioQueueParameterID,
                                            AudioQueueParameterValue * _Nonnull) = AudioQueueGetParameter;

/* --- AudioToolbox/AudioServices.h ------------------------------------------ */
static OSStatus (*p_AudioServicesCreateSystemSoundID)(CFURLRef _Nonnull, SystemSoundID * _Nonnull)
    = AudioServicesCreateSystemSoundID;
static OSStatus (*p_AudioServicesDisposeSystemSoundID)(SystemSoundID) = AudioServicesDisposeSystemSoundID;
static void (*p_AudioServicesPlaySystemSound)(SystemSoundID) = AudioServicesPlaySystemSound;
static void (*p_AudioServicesPlayAlertSound)(SystemSoundID) = AudioServicesPlayAlertSound;

/* --- AudioToolbox/AudioComponent.h and AudioUnit.h -------------------------- */
static AudioComponent _Nullable (*p_AudioComponentFindNext)(AudioComponent _Nullable,
                                                             const AudioComponentDescription * _Nonnull)
    = AudioComponentFindNext;
static UInt32 (*p_AudioComponentCount)(const AudioComponentDescription * _Nonnull) = AudioComponentCount;
static OSStatus (*p_AudioComponentGetDescription)(AudioComponent _Nonnull,
                                                  AudioComponentDescription * _Nonnull)
    = AudioComponentGetDescription;
static OSStatus (*p_AudioComponentCopyName)(AudioComponent _Nonnull, CFStringRef _Nullable * _Nonnull)
    = AudioComponentCopyName;
static OSStatus (*p_AudioComponentInstanceNew)(AudioComponent _Nonnull,
                                               AudioComponentInstance _Nullable * _Nonnull)
    = AudioComponentInstanceNew;
static OSStatus (*p_AudioComponentInstanceDispose)(AudioComponentInstance _Nonnull)
    = AudioComponentInstanceDispose;
static Boolean (*p_AudioComponentInstanceCanDo)(AudioComponentInstance _Nonnull, SInt16)
    = AudioComponentInstanceCanDo;
static OSStatus (*p_AudioUnitInitialize)(AudioUnit _Nonnull) = AudioUnitInitialize;
static OSStatus (*p_AudioUnitUninitialize)(AudioUnit _Nonnull) = AudioUnitUninitialize;
static OSStatus (*p_AudioUnitGetPropertyInfo)(AudioUnit _Nonnull, AudioUnitPropertyID, AudioUnitScope,
                                              AudioUnitElement, UInt32 * _Nullable, Boolean * _Nullable)
    = AudioUnitGetPropertyInfo;
static OSStatus (*p_AudioUnitGetProperty)(AudioUnit _Nonnull, AudioUnitPropertyID, AudioUnitScope,
                                          AudioUnitElement, void * _Nonnull, UInt32 * _Nonnull)
    = AudioUnitGetProperty;
static OSStatus (*p_AudioUnitSetProperty)(AudioUnit _Nonnull, AudioUnitPropertyID, AudioUnitScope,
                                          AudioUnitElement, const void * _Nullable, UInt32)
    = AudioUnitSetProperty;
static OSStatus (*p_AudioUnitGetParameter)(AudioUnit _Nonnull, AudioUnitParameterID, AudioUnitScope,
                                           AudioUnitElement, AudioUnitParameterValue * _Nonnull)
    = AudioUnitGetParameter;
static OSStatus (*p_AudioUnitSetParameter)(AudioUnit _Nonnull, AudioUnitParameterID, AudioUnitScope,
                                           AudioUnitElement, AudioUnitParameterValue, UInt32)
    = AudioUnitSetParameter;
static OSStatus (*p_AudioUnitRender)(AudioUnit _Nonnull, AudioUnitRenderActionFlags * _Nullable,
                                     const AudioTimeStamp * _Nonnull, UInt32, UInt32,
                                     AudioBufferList * _Nonnull) = AudioUnitRender;
static OSStatus (*p_AudioUnitReset)(AudioUnit _Nonnull, AudioUnitScope, AudioUnitElement) = AudioUnitReset;
static OSStatus (*p_AudioOutputUnitStart)(AudioUnit _Nonnull) = AudioOutputUnitStart;
static OSStatus (*p_AudioOutputUnitStop)(AudioUnit _Nonnull) = AudioOutputUnitStop;

int main(void);
int main(void)
{
	/* Touch every pointer so nothing above is optimised away. The values are never called: the types are
	 * the assertion, and this file is a compile-time gate with a linker's edge. */
	void *unused[] = {
		(void *)p_AudioGetCurrentHostTime, (void *)p_AudioGetHostClockFrequency,
		(void *)p_AudioGetHostClockMinimumTimeDelta, (void *)p_AudioConvertHostTimeToNanos,
		(void *)p_AudioConvertNanosToHostTime,
		(void *)p_AudioObjectGetPropertyData, (void *)p_AudioObjectSetPropertyData,
		(void *)p_AudioObjectHasProperty, (void *)p_AudioObjectIsPropertySettable,
		(void *)p_AudioObjectGetPropertyDataSize, (void *)p_AudioObjectAddPropertyListener,
		(void *)p_AudioObjectRemovePropertyListener, (void *)p_AudioObjectAddPropertyListenerBlock,
		(void *)p_AudioObjectRemovePropertyListenerBlock, (void *)p_AudioDeviceCreateIOProcID,
		(void *)p_AudioDeviceCreateIOProcIDWithBlock, (void *)p_AudioDeviceDestroyIOProcID,
		(void *)p_AudioDeviceStart, (void *)p_AudioDeviceStop,
		(void *)p_AudioDeviceGetCurrentTime, (void *)p_AudioDeviceTranslateTime,
		(void *)p_AudioDeviceGetNearestStartTime, (void *)p_AudioDeviceAddIOProc,
		(void *)p_AudioDeviceRemoveIOProc, (void *)p_AudioDeviceGetProperty,
		(void *)p_AudioDeviceSetProperty, (void *)p_AudioDeviceGetPropertyInfo,
		(void *)p_AudioStreamGetProperty,
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
		(void *)p_AudioFileStreamOpen, (void *)p_AudioFileStreamParseBytes,
		(void *)p_AudioFileStreamGetPropertyInfo, (void *)p_AudioFileStreamGetProperty,
		(void *)p_AudioFileStreamSetProperty, (void *)p_AudioFileStreamSeek,
		(void *)p_AudioFileStreamClose,
		(void *)p_AudioQueueNewOutput, (void *)p_AudioQueueNewOutputWithDispatchQueue,
		(void *)p_AudioQueueAllocateBuffer, (void *)p_AudioQueueEnqueueBuffer,
		(void *)p_AudioQueueStart, (void *)p_AudioQueueStop, (void *)p_AudioQueueDispose,
		(void *)p_AudioQueueFreeBuffer, (void *)p_AudioQueueGetProperty,
		(void *)p_AudioQueueSetProperty, (void *)p_AudioQueueSetParameter,
		(void *)p_AudioQueueGetParameter,
		(void *)p_AudioServicesCreateSystemSoundID, (void *)p_AudioServicesDisposeSystemSoundID,
		(void *)p_AudioServicesPlaySystemSound, (void *)p_AudioServicesPlayAlertSound,
		(void *)p_AudioComponentFindNext, (void *)p_AudioComponentCount,
		(void *)p_AudioComponentGetDescription, (void *)p_AudioComponentCopyName,
		(void *)p_AudioComponentInstanceNew, (void *)p_AudioComponentInstanceDispose,
		(void *)p_AudioComponentInstanceCanDo, (void *)p_AudioUnitInitialize,
		(void *)p_AudioUnitUninitialize, (void *)p_AudioUnitGetPropertyInfo,
		(void *)p_AudioUnitGetProperty, (void *)p_AudioUnitSetProperty,
		(void *)p_AudioUnitGetParameter, (void *)p_AudioUnitSetParameter,
		(void *)p_AudioUnitRender, (void *)p_AudioUnitReset,
		(void *)p_AudioOutputUnitStart, (void *)p_AudioOutputUnitStop,
	};

	return unused[0] == NULL ? 0 : 0;
}
