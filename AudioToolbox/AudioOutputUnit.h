/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioOutputUnit.h — starting and stopping a unit that emits audio.
 *
 * AN OUTPUT UNIT IS THE ONE KIND THAT NEEDS A CLOCK, and these two functions are how it gets one: `Start` attaches
 * the unit to the default output device's io proc, `Stop` detaches it. That is the whole difference from
 * `AudioUnitInitialize`, which only validates that the unit could start.
 *
 * SPECIFIED BY macOS 10.12.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOOUTPUTUNIT_H
#define AGORA_AUDIOTOOLBOX_AUDIOOUTPUTUNIT_H

#include <AudioToolbox/AudioUnit.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

CF_ENUM(UInt32)
{
	kAudioOutputUnitProperty_CurrentDevice    = 2000,
	kAudioOutputUnitProperty_IsRunning        = 2001,
	kAudioOutputUnitProperty_ChannelMap       = 2002,
	kAudioOutputUnitProperty_EnableIO         = 2003,
	kAudioOutputUnitProperty_StartTime        = 2004,
	kAudioOutputUnitProperty_SetInputCallback = 2005,
	kAudioOutputUnitProperty_HasIO            = 2006,
	kAudioOutputUnitProperty_StartTimestampsAtZero = 2007
};

extern OSStatus
AudioOutputUnitStart(AudioUnit ci);

extern OSStatus
AudioOutputUnitStop(AudioUnit ci);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOOUTPUTUNIT_H */
