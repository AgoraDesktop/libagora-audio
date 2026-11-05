/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioUnit.h — a unit: a thing with scopes, elements, properties and a render function.
 *
 * THE MODEL IS TWO AXES AND A PULL. A unit has scopes (global, input, output) and each scope has elements (buses);
 * a property is addressed by both, which is why `AudioUnitSetProperty` takes four coordinates before its data. And
 * nothing pushes audio: `AudioUnitRender` is called by whoever needs samples, and the unit returns them — so an
 * output unit is the only one that needs a clock, and that clock is the device's.
 *
 * SPECIFIED BY macOS 10.12: the prototypes and the property, scope and element vocabulary are the reference's.
 * `AudioUnitProcess` and the render-notify doors are declared and refuse: they are for units that process in place
 * without a pull, which the three built-ins here do not.
 *
 * THE THREE UNITS THIS PHASE BUILDS are the ones an application needs to be audible and to be correct:
 * `kAudioUnitSubType_DefaultOutput` (the device), `kAudioUnitSubType_AUConverter` (format and rate) and
 * `kAudioUnitSubType_MultiChannelMixer` (buses with volumes). A fourth would be a fourth way to be wrong.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOUNIT_H
#define AGORA_AUDIOTOOLBOX_AUDIOUNIT_H

#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AudioConverter.h>
#include <CoreAudio/AudioHardware.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

CF_ENUM(AudioUnitPropertyID)
{
	kAudioUnitProperty_ClassInfo            = 0,
	kAudioUnitProperty_MakeConnection       = 1,
	kAudioUnitProperty_SampleRate           = 2,
	kAudioUnitProperty_ParameterList        = 3,
	kAudioUnitProperty_StreamFormat         = 8,
	kAudioUnitProperty_ElementCount         = 11,
	kAudioUnitProperty_SupportedNumChannels = 13,
	kAudioUnitProperty_MaximumFramesPerSlice = 14,
	kAudioUnitProperty_SetRenderCallback    = 23,
	kAudioUnitProperty_InPlaceProcessing    = 29,
	kAudioUnitProperty_ElementName          = 30,
	kAudioUnitProperty_CurrentPlaybackDevice = 2000
};

CF_ENUM(UInt32)
{
	kAudioUnitSubType_DefaultOutput      = 'def ',
	kAudioUnitSubType_SystemOutput       = 'sys ',
	kAudioUnitSubType_AUConverter        = 'conv',
	kAudioUnitSubType_MultiChannelMixer  = 'mcmx',
	kAudioUnitSubType_StereoMixer        = 'smxr'
};

CF_ENUM(UInt32)
{
	kMultiChannelMixerParam_Volume       = 0,
	kMultiChannelMixerParam_Enable       = 1
};

extern OSStatus
AudioUnitInitialize(AudioUnit inUnit);

extern OSStatus
AudioUnitUninitialize(AudioUnit inUnit);

extern OSStatus
AudioUnitGetPropertyInfo(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                         AudioUnitElement inElement, UInt32 * _Nullable outDataSize,
                         Boolean * _Nullable outWritable);

extern OSStatus
AudioUnitGetProperty(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                     AudioUnitElement inElement, void * outData, UInt32 * ioDataSize);

extern OSStatus
AudioUnitSetProperty(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                     AudioUnitElement inElement, const void * _Nullable inData, UInt32 inDataSize);

extern OSStatus
AudioUnitGetParameter(AudioUnit inUnit, AudioUnitParameterID inID, AudioUnitScope inScope,
                      AudioUnitElement inElement, AudioUnitParameterValue * outValue);

extern OSStatus
AudioUnitSetParameter(AudioUnit inUnit, AudioUnitParameterID inID, AudioUnitScope inScope,
                      AudioUnitElement inElement, AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames);

/* The pull. `ioData` carries the buffers and their frame capacity; a unit that produces audio fills them. */
extern OSStatus
AudioUnitRender(AudioUnit inUnit, AudioUnitRenderActionFlags * _Nullable ioActionFlags,
                const AudioTimeStamp * inTimeStamp, UInt32 inOutputBusNumber, UInt32 inNumberFrames,
                AudioBufferList * ioData);

extern OSStatus
AudioUnitReset(AudioUnit inUnit, AudioUnitScope inScope, AudioUnitElement inElement);

/* Declared and refused by name: in-place processing without a pull, and the listener doors. */
extern OSStatus
AudioUnitProcess(AudioUnit inUnit, AudioUnitRenderActionFlags * _Nullable ioActionFlags,
                 const AudioTimeStamp * inTimeStamp, UInt32 inNumberFrames, AudioBufferList * ioData);

extern OSStatus
AudioUnitScheduleParameters(AudioUnit inUnit, const void * inParameterEvent, UInt32 inNumParamEvents);

extern OSStatus
AudioUnitAddRenderNotify(AudioUnit inUnit, AURenderCallback inProc, void * _Nullable inProcUserData);

extern OSStatus
AudioUnitRemoveRenderNotify(AudioUnit inUnit, AURenderCallback inProc, void * _Nullable inProcUserData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOUNIT_H */
