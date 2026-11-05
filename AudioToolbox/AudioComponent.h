/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioComponent.h — what an audio unit *is*, before it is an instance.
 *
 * THE REGISTRY IS A STATIC TABLE HERE, AND THAT IS THE HONEST FORM OF THIS PHASE. On macOS the component manager
 * searches every bundle on disk, which is why `AudioComponentFindNext` takes a "find the next one" cursor: a
 * description can match several installed units. This tree has three built-in units and no plug-in ABI, so the
 * cursor walks a table and a description that names a fourth thing finds nothing — which is a refusal a caller
 * can see, rather than a dynamic loader it cannot.
 *
 * SPECIFIED BY macOS 10.12: `AudioComponentDescription` (five fields, twenty bytes), the component and instance
 * types, and every prototype are the reference's, checked by the manifest.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOCOMPONENT_H
#define AGORA_AUDIOTOOLBOX_AUDIOCOMPONENT_H

#include <CoreAudio/CoreAudioTypes.h>
#include <CoreFoundation/CFString.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

struct AudioComponentDescription
{
	OSType  componentType;
	OSType  componentSubType;
	OSType  componentManufacturer;
	UInt32  componentFlags;
	UInt32  componentFlagsMask;
};
typedef struct AudioComponentDescription AudioComponentDescription;

typedef struct OpaqueAudioComponent *   AudioComponent;
typedef struct ComponentInstanceRecord * AudioComponentInstance;

typedef UInt32 AudioComponentFlags;
typedef UInt32 AudioComponentInstantiationOptions;

/* A unit is an instance; the names are historical and both are real. */
typedef AudioComponentInstance AudioUnit;

typedef UInt32 AudioUnitPropertyID;
typedef UInt32 AudioUnitScope;
typedef UInt32 AudioUnitElement;
typedef UInt32 AudioUnitParameterID;
typedef Float32 AudioUnitParameterValue;
typedef UInt32 AudioUnitRenderActionFlags;

CF_ENUM(UInt32)
{
	kAudioUnitType_Output            = 'auou',
	kAudioUnitType_MusicDevice       = 'aumu',
	kAudioUnitType_MusicEffect       = 'aumf',
	kAudioUnitType_FormatConverter   = 'aufc',
	kAudioUnitType_Effect            = 'aufx',
	kAudioUnitType_Mixer             = 'aumx',
	kAudioUnitType_Panner            = 'aupn',
	kAudioUnitType_Generator         = 'augn',
	kAudioUnitType_MIDIProcessor     = 'aumi'
};

CF_ENUM(UInt32)
{
	kAudioUnitManufacturer_Apple     = 'appl'
};

CF_ENUM(AudioUnitScope)
{
	kAudioUnitScope_Global           = 0,
	kAudioUnitScope_Input            = 1,
	kAudioUnitScope_Output           = 2,
	kAudioUnitScope_Group            = 3,
	kAudioUnitScope_Part             = 4,
	kAudioUnitScope_Note             = 5
};

CF_ENUM(OSStatus)
{
	kAudioUnitErr_InvalidProperty       = -10879,
	kAudioUnitErr_InvalidParameter      = -10878,
	kAudioUnitErr_InvalidElement        = -10877,
	kAudioUnitErr_NoConnection          = -10876,
	kAudioUnitErr_FailedInitialization  = -10875,
	kAudioUnitErr_TooManyFramesToProcess = -10874,
	kAudioUnitErr_InvalidFile           = -10871,
	kAudioUnitErr_FormatNotSupported    = -10868,
	kAudioUnitErr_Uninitialized         = -10867,
	kAudioUnitErr_InvalidScope          = -10866,
	kAudioUnitErr_PropertyNotWritable   = -10865,
	kAudioUnitErr_CannotDoInCurrentContext = -10863,
	kAudioUnitErr_InvalidPropertyValue  = -10851,
	kAudioUnitErr_PropertyNotInUse      = -10850,
	kAudioUnitErr_Initialized           = -10849,
	kAudioUnitErr_InvalidOfflineRender  = -10848,
	kAudioUnitErr_Unauthorized          = -10847
};

/* The render callback: fill `ioData` with `inNumberFrames`, or read it, depending on the direction. */
typedef OSStatus
(*AURenderCallback)(void * _Nonnull                       inRefCon,
                    AudioUnitRenderActionFlags * _Nonnull  ioActionFlags,
                    const AudioTimeStamp * _Nonnull        inTimeStamp,
                    UInt32                                 inBusNumber,
                    UInt32                                 inNumberFrames,
                    AudioBufferList * _Nullable            ioData);

struct AURenderCallbackStruct
{
	AURenderCallback    inputProc;
	void *              inputProcRefCon;
};
typedef struct AURenderCallbackStruct AURenderCallbackStruct;

/* Find a component: `NULL` starts the search, and a description with zero flags matches anything with the same
 * type, subtype and manufacturer. */
extern AudioComponent _Nullable
AudioComponentFindNext(AudioComponent _Nullable inComponent, const AudioComponentDescription * inDesc);

extern UInt32
AudioComponentCount(const AudioComponentDescription * inDesc);

extern OSStatus
AudioComponentGetDescription(AudioComponent inComponent, AudioComponentDescription * outDesc);

extern OSStatus
AudioComponentCopyName(AudioComponent inComponent, CFStringRef _Nullable * _Nonnull outName);

extern OSStatus
AudioComponentInstanceNew(AudioComponent inComponent, AudioComponentInstance _Nullable * _Nonnull outInstance);

extern OSStatus
AudioComponentInstanceDispose(AudioComponentInstance inInstance);

extern Boolean
AudioComponentInstanceCanDo(AudioComponentInstance inInstance, SInt16 inSelectorID);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOCOMPONENT_H */
