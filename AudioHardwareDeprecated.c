/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardwareDeprecated.c — the pre-10.7 doors, as translations.
 *
 * THEY ARE NOT REFUSED, BECAUSE THEY CAN BE ANSWERED: every one of them is a call to the object API with the
 * channel and the input flag folded into a scope and an element. A refusal here would be a refusal of a
 * question this component can answer, which is not what the tier rubric means — `kAudioHardwareUnknownPropertyError`
 * is for a property, not for an old spelling of a property that exists.
 *
 * WHAT IS NOT TRANSLATED: `AudioDeviceAddIOProc` takes a bare function pointer with no id, which is exactly
 * what the modern door takes after `AudioDeviceCreateIOProcID` — so it creates one and throws the id away,
 * and `AudioDeviceRemoveIOProc` finds it by pointer.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

OSStatus AudioDeviceAddIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc, void *inClientData)
{
	AudioDeviceIOProcID id = NULL;

	return AudioDeviceCreateIOProcID(inDevice, inProc, inClientData, &id);
}

OSStatus AudioDeviceRemoveIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc)
{
	return AudioDeviceDestroyIOProcID(inDevice, inProc);
}

OSStatus AudioDeviceGetProperty(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput,
                                AudioDevicePropertyID inPropertyID, UInt32 *ioPropertyDataSize,
                                void *outPropertyData)
{
	AudioObjectPropertyAddress address;

	address.mSelector = inPropertyID;
	address.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
	address.mElement = inChannel;

	return AudioObjectGetPropertyData(inDevice, &address, 0, NULL, ioPropertyDataSize, outPropertyData);
}

OSStatus AudioDeviceSetProperty(AudioDeviceID inDevice, const AudioTimeStamp *inWhen, UInt32 inChannel,
                                Boolean isInput, AudioDevicePropertyID inPropertyID,
                                UInt32 inPropertyDataSize, const void *inPropertyData)
{
	AudioObjectPropertyAddress address;

	(void)inWhen;

	address.mSelector = inPropertyID;
	address.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
	address.mElement = inChannel;

	return AudioObjectSetPropertyData(inDevice, &address, 0, NULL, inPropertyDataSize, inPropertyData);
}

OSStatus AudioDeviceGetPropertyInfo(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput,
                                    AudioDevicePropertyID inPropertyID, UInt32 *outSize,
                                    Boolean *outWritable)
{
	AudioObjectPropertyAddress address;
	OSStatus status;

	address.mSelector = inPropertyID;
	address.mScope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
	address.mElement = inChannel;

	status = AudioObjectGetPropertyDataSize(inDevice, &address, 0, NULL, outSize);
	if (status == 0 && outWritable != NULL) {
		AudioObjectIsPropertySettable(inDevice, &address, outWritable);
	}

	return status;
}

OSStatus AudioStreamGetProperty(AudioStreamID inStream, UInt32 inChannel,
                                AudioDevicePropertyID inPropertyID, UInt32 *ioPropertyDataSize,
                                void *outPropertyData)
{
	AudioObjectPropertyAddress address;

	address.mSelector = inPropertyID;
	address.mScope = kAudioObjectPropertyScopeGlobal;
	address.mElement = inChannel;

	return AudioObjectGetPropertyData(inStream, &address, 0, NULL, ioPropertyDataSize, outPropertyData);
}
