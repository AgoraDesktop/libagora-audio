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
	kAudioUnitProperty_ParameterInfo        = 4,
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

/* What a parameter's value means. `AudioUnitParameterInfo` is the description a unit gives of each id, and it
 * is what `AUParameterTree` is built from — 104 bytes with `unitName` at 56, because a client casts a
 * caller-provided buffer to it and a wrong offset corrupts a CFStringRef rather than returning an error. */
typedef struct AudioUnitParameterInfo
{
	char                    name[52];
	CFStringRef _Nullable   unitName;
	UInt32                  clumpID;
	CFStringRef _Nullable   cfNameString;
	AudioUnitParameterUnit  unit;
	AudioUnitParameterValue minValue;
	AudioUnitParameterValue maxValue;
	AudioUnitParameterValue defaultValue;
	AudioUnitParameterOptions flags;
} AudioUnitParameterInfo;

CF_ENUM(AudioUnitParameterUnit)
{
	kAudioUnitParameterUnit_Generic             = 0,
	kAudioUnitParameterUnit_Indexed             = 1,
	kAudioUnitParameterUnit_Boolean             = 2,
	kAudioUnitParameterUnit_Percent             = 3,
	kAudioUnitParameterUnit_Seconds             = 4,
	kAudioUnitParameterUnit_SampleFrames        = 5,
	kAudioUnitParameterUnit_Phase               = 6,
	kAudioUnitParameterUnit_Rate                = 7,
	kAudioUnitParameterUnit_Hertz               = 8,
	kAudioUnitParameterUnit_Cents               = 9,
	kAudioUnitParameterUnit_RelativeSemiTones   = 10,
	kAudioUnitParameterUnit_MIDINoteNumber      = 11,
	kAudioUnitParameterUnit_MIDIController      = 12,
	kAudioUnitParameterUnit_Decibels            = 13,
	kAudioUnitParameterUnit_LinearGain          = 14,
	kAudioUnitParameterUnit_Degrees             = 15,
	kAudioUnitParameterUnit_EqualPowerCrossfade = 16,
	kAudioUnitParameterUnit_MixerFaderCurve1    = 17,
	kAudioUnitParameterUnit_Pan                 = 18,
	kAudioUnitParameterUnit_Meters              = 19,
	kAudioUnitParameterUnit_AbsoluteCents       = 20,
	kAudioUnitParameterUnit_Octaves             = 21,
	kAudioUnitParameterUnit_BPM                 = 22,
	kAudioUnitParameterUnit_Beats               = 23,
	kAudioUnitParameterUnit_Milliseconds        = 24,
	kAudioUnitParameterUnit_Ratio               = 25,
	kAudioUnitParameterUnit_CustomUnit          = 26
};

/* The flags say who may read and write a parameter and what it means. The display hints Apple also has here
 * are a UI's business; every one of them is in the manifest, waiting for the phase that draws one. */
CF_ENUM(AudioUnitParameterOptions)
{
	kAudioUnitParameterFlag_CFNameRelease     = (1UL << 4),
	kAudioUnitParameterFlag_OmitFromPresets   = (1UL << 13),
	kAudioUnitParameterFlag_PlotHistory       = (1UL << 14),
	kAudioUnitParameterFlag_MeterReadOnly     = (1UL << 15),
	kAudioUnitParameterFlag_HasClump          = (1UL << 20),
	kAudioUnitParameterFlag_ValuesHaveStrings = (1UL << 21),
	kAudioUnitParameterFlag_IsHighResolution  = (1UL << 23),
	kAudioUnitParameterFlag_NonRealTime       = (1UL << 24),
	kAudioUnitParameterFlag_CanRamp           = (1UL << 25),
	kAudioUnitParameterFlag_ExpertMode        = (1UL << 26),
	kAudioUnitParameterFlag_HasCFNameString   = (1UL << 27),
	kAudioUnitParameterFlag_IsGlobalMeta      = (1UL << 28),
	kAudioUnitParameterFlag_IsElementMeta     = (1UL << 29),
	kAudioUnitParameterFlag_IsReadable        = (1UL << 30),
	kAudioUnitParameterFlag_IsWritable        = (1UL << 31)
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
