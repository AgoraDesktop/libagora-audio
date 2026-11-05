/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioUnit.c — three units, one object model, and the registry that makes them findable.
 *
 * THE REGISTRY IS A TABLE OF THREE, AND THE THREE ARE THE POINT: an application needs a way out to the device
 * (DefaultOutput), a way between formats (AUConverter), and a way to sum several sources (MultiChannelMixer).
 * Everything above them — a graph, a plug-in host, an offline render — is arrangement, and arrangement is the next
 * slice rather than a fourth thing to be wrong here.
 *
 * EACH UNIT ANSWERS THE SAME FOUR QUESTIONS: what is my stream format for this scope and element, how many
 * elements are there, what is my maximum slice, and what fills it. That is why the three kinds share one
 * property switch and differ only in what `AudioUnitRender` does — which is the same shape a real unit host has.
 *
 * THE OUTPUT UNIT OWNS NO THREAD: it attaches to the default device's io proc and lets the device call it, the
 * same arrangement `AudioQueue` uses. A unit that started its own thread would be a second clock, and the first
 * thing to drift out of tune.
 */

#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include "AudioToolbox_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define AGORA_UNIT_MAX_ELEMENTS 8

typedef enum {
	AGORA_UNIT_OUTPUT = 0,
	AGORA_UNIT_CONVERTER = 1,
	AGORA_UNIT_MIXER = 2
} agora_unit_kind;

struct OpaqueAudioComponent {
	AudioComponentDescription desc;
	const char *              name;
	agora_unit_kind           kind;
};

typedef struct {
	AudioStreamBasicDescription format;
	Float32                     volume;
	bool                        enabled;
	AURenderCallbackStruct      callback;	/* what feeds this bus; the mixer pulls it */
} mixer_bus;

struct ComponentInstanceRecord {
	AudioComponent  component;
	agora_unit_kind kind;

	bool            initialized;
	bool            running;
	UInt32          max_frames;
	AudioDeviceID   device;
	AudioDeviceIOProcID io_id;

	AURenderCallbackStruct callback;

	/* Converter: the two descriptions and the object between them. */
	AudioStreamBasicDescription in_format;
	AudioStreamBasicDescription out_format;
	AudioConverterRef           converter;

	/* Mixer: its buses. */
	UInt32    element_count;
	mixer_bus buses[AGORA_UNIT_MAX_ELEMENTS];

	/* Render scratch, grown as needed and reused: an io proc may not allocate. */
	float *  scratch;
	UInt32   scratch_frames;
	UInt32   scratch_channels;

	pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------------------------------
 * The registry.
 * ---------------------------------------------------------------------------------------------- */

static struct OpaqueAudioComponent g_components[] = {
	{ { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 },
	  "Agora Default Output", AGORA_UNIT_OUTPUT },
	{ { kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter, kAudioUnitManufacturer_Apple, 0, 0 },
	  "Agora Converter", AGORA_UNIT_CONVERTER },
	{ { kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer, kAudioUnitManufacturer_Apple, 0, 0 },
	  "Agora MultiChannel Mixer", AGORA_UNIT_MIXER }
};

static const UInt32 g_component_count = (UInt32)(sizeof(g_components) / sizeof(g_components[0]));

static bool fn_matches(const AudioComponentDescription *candidate, const AudioComponentDescription *wanted)
{
	if (wanted->componentType != 0 && wanted->componentType != candidate->componentType) {
		return false;
	}
	if (wanted->componentSubType != 0 && wanted->componentSubType != candidate->componentSubType) {
		return false;
	}
	if (wanted->componentManufacturer != 0 && wanted->componentManufacturer != candidate->componentManufacturer) {
		return false;
	}

	/* The flags and the mask are a bit protocol, and the mask says which bits matter. */
	if ((wanted->componentFlags & wanted->componentFlagsMask)
	    != (candidate->componentFlags & wanted->componentFlagsMask)) {
		return false;
	}

	return true;
}

AudioComponent AudioComponentFindNext(AudioComponent inComponent, const AudioComponentDescription *inDesc)
{
	UInt32 start = 0;

	if (inDesc == NULL) {
		return NULL;
	}

	if (inComponent != NULL) {
		if (inComponent < g_components || inComponent >= g_components + g_component_count) {
			return NULL;	/* not one of ours: a component from another registry cannot be continued here */
		}
		start = (UInt32)(inComponent - g_components) + 1;
	}

	{
		UInt32 i;

		for (i = start; i < g_component_count; i++) {
			if (fn_matches(&g_components[i].desc, inDesc)) {
				return &g_components[i];
			}
		}
	}

	return NULL;
}

UInt32 AudioComponentCount(const AudioComponentDescription *inDesc)
{
	UInt32 count = 0;
	AudioComponent current = NULL;

	if (inDesc == NULL) {
		return 0;
	}

	while ((current = AudioComponentFindNext(current, inDesc)) != NULL) {
		count++;
	}

	return count;
}

OSStatus AudioComponentGetDescription(AudioComponent inComponent, AudioComponentDescription *outDesc)
{
	if (inComponent == NULL || outDesc == NULL) {
		return kAudio_ParamError;
	}

	*outDesc = inComponent->desc;
	return 0;
}

OSStatus AudioComponentCopyName(AudioComponent inComponent, CFStringRef *outName)
{
	if (inComponent == NULL || outName == NULL) {
		return kAudio_ParamError;
	}

	*outName = CFStringCreateWithCString(kCFAllocatorDefault, inComponent->name, kCFStringEncodingUTF8);
	return (*outName != NULL) ? 0 : kAudio_MemFullError;
}

Boolean AudioComponentInstanceCanDo(AudioComponentInstance inInstance, SInt16 inSelectorID)
{
	(void)inInstance;
	(void)inSelectorID;

	/* There is no selector table: these units answer the property and render doors and nothing else, and a
	 * "can do" that returned true for a selector nobody implements would be the wrong half of a promise. */
	return false;
}

/* ------------------------------------------------------------------------------------------------
 * Creation.
 * ---------------------------------------------------------------------------------------------- */

static AudioStreamBasicDescription fn_default_format(Float64 rate, UInt32 channels)
{
	AudioStreamBasicDescription format;

	memset(&format, 0, sizeof(format));
	format.mSampleRate = rate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
	format.mChannelsPerFrame = channels;
	format.mBitsPerChannel = 32;
	format.mBytesPerFrame = channels * (UInt32)sizeof(float);
	format.mBytesPerPacket = format.mBytesPerFrame;
	format.mFramesPerPacket = 1;

	return format;
}

static UInt32 fn_device_channels(AudioDeviceID device, Float64 *out_rate)
{
	AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
	                                       kAudioObjectPropertyElementMaster };
	AudioStreamID list[8];
	UInt32 size = sizeof(list);
	UInt32 channels = 0;

	*out_rate = 48000.0;

	{
		AudioObjectPropertyAddress rate_address = { kAudioDevicePropertyNominalSampleRate,
		                                            kAudioObjectPropertyScopeGlobal,
		                                            kAudioObjectPropertyElementMaster };
		Float64 rate = 0.0;
		UInt32 rate_size = sizeof(rate);

		if (AudioObjectGetPropertyData(device, &rate_address, 0, NULL, &rate_size, &rate) == 0 && rate > 0.0) {
			*out_rate = rate;
		}
	}

	memset(list, 0, sizeof(list));
	if (AudioObjectGetPropertyData(device, &streams, 0, NULL, &size, list) == 0 && size >= sizeof(AudioStreamID)) {
		AudioObjectPropertyAddress format_address = { kAudioStreamPropertyVirtualFormat,
		                                              kAudioObjectPropertyScopeGlobal,
		                                              kAudioObjectPropertyElementMaster };
		AudioStreamBasicDescription format;
		UInt32 format_size = sizeof(format);

		memset(&format, 0, sizeof(format));
		if (AudioObjectGetPropertyData(list[0], &format_address, 0, NULL, &format_size, &format) == 0
		    && format.mChannelsPerFrame > 0) {
			channels = format.mChannelsPerFrame;
		}
	}

	return channels > 0 ? channels : 2;
}

OSStatus AudioComponentInstanceNew(AudioComponent inComponent, AudioComponentInstance *outInstance)
{
	AudioComponentInstance unit;
	Float64 rate;
	UInt32 channels;

	if (inComponent == NULL || outInstance == NULL) {
		return kAudio_ParamError;
	}

	unit = calloc(1, sizeof(*unit));
	if (unit == NULL) {
		return kAudio_MemFullError;
	}

	unit->component = inComponent;
	unit->kind = inComponent->kind;
	unit->max_frames = 4096;
	unit->element_count = (unit->kind == AGORA_UNIT_MIXER) ? 1 : 1;
	pthread_mutex_init(&unit->lock, NULL);

	{
		AudioObjectPropertyAddress default_device = { kAudioHardwarePropertyDefaultOutputDevice,
		                                              kAudioObjectPropertyScopeGlobal,
		                                              kAudioObjectPropertyElementMaster };
		UInt32 size = sizeof(AudioDeviceID);

		unit->device = kAudioObjectUnknown;
		AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_device, 0, NULL, &size, &unit->device);
	}

	channels = fn_device_channels(unit->device, &rate);

	switch (unit->kind) {
	case AGORA_UNIT_OUTPUT:
		unit->out_format = fn_default_format(rate, channels);
		unit->in_format = unit->out_format;
		break;

	case AGORA_UNIT_CONVERTER:
		/* A converter with no descriptions cannot do anything, and Apple's starts them unset. The caller
		 * sets both formats before initialising. */
		memset(&unit->in_format, 0, sizeof(unit->in_format));
		memset(&unit->out_format, 0, sizeof(unit->out_format));
		break;

	case AGORA_UNIT_MIXER:
		unit->out_format = fn_default_format(rate, channels);
		unit->buses[0].format = unit->out_format;
		unit->buses[0].volume = 1.0f;
		unit->buses[0].enabled = true;
		break;
	}

	*outInstance = unit;
	return 0;
}

OSStatus AudioComponentInstanceDispose(AudioComponentInstance inInstance)
{
	if (inInstance == NULL) {
		return kAudio_ParamError;
	}

	if (inInstance->running) {
		AudioOutputUnitStop(inInstance);
	}

	if (inInstance->converter != NULL) {
		AudioConverterDispose(inInstance->converter);
	}

	free(inInstance->scratch);
	pthread_mutex_destroy(&inInstance->lock);
	free(inInstance);

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Properties and parameters.
 * ---------------------------------------------------------------------------------------------- */

static AudioStreamBasicDescription *fn_format_for(AudioComponentInstance unit, AudioUnitScope scope,
                                                  AudioUnitElement element)
{
	switch (unit->kind) {
	case AGORA_UNIT_OUTPUT:
		/* The output unit's input scope is what the caller fills; its output scope is the device. */
		return (scope == kAudioUnitScope_Input) ? &unit->out_format : &unit->out_format;

	case AGORA_UNIT_CONVERTER:
		if (scope == kAudioUnitScope_Input) {
			return &unit->in_format;
		}
		return &unit->out_format;

	case AGORA_UNIT_MIXER:
		if (scope == kAudioUnitScope_Input) {
			if (element >= unit->element_count) {
				return NULL;
			}
			return &unit->buses[element].format;
		}
		return &unit->out_format;
	}

	return NULL;
}

/* A unit's parameters are per (scope, id), and the element is what a *value* is set on — the mixer's Volume
 * is one parameter and as many values as it has buses. That is why the list is two ids and the tree the AUv3
 * door builds has one node per bus: the address carries the element, the id does not. */
static UInt32 fn_parameter_count(AudioComponentInstance unit, AudioUnitScope scope)
{
	if (unit->kind != AGORA_UNIT_MIXER || scope != kAudioUnitScope_Input) {
		return 0;
	}
	return 2;
}

static bool fn_parameter_id_is_known(AudioComponentInstance unit, AudioUnitScope scope, AudioUnitElement id)
{
	if (unit->kind != AGORA_UNIT_MIXER || scope != kAudioUnitScope_Input) {
		return false;
	}
	return id == kMultiChannelMixerParam_Volume || id == kMultiChannelMixerParam_Enable;
}

OSStatus AudioUnitGetPropertyInfo(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                                  AudioUnitElement inElement, UInt32 *outDataSize, Boolean *outWritable)
{
	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	if (outWritable != NULL) {
		*outWritable = false;
	}

	switch (inID) {
	case kAudioUnitProperty_StreamFormat:
		if (outDataSize) { *outDataSize = sizeof(AudioStreamBasicDescription); }
		if (outWritable) { *outWritable = true; }
		return 0;
	case kAudioUnitProperty_ElementCount:
		if (outDataSize) { *outDataSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = (inUnit->kind == AGORA_UNIT_MIXER); }
		return 0;
	case kAudioUnitProperty_ParameterList:
		if (outDataSize) { *outDataSize = fn_parameter_count(inUnit, inScope) * (UInt32)sizeof(AudioUnitParameterID); }
		return 0;
	case kAudioUnitProperty_ParameterInfo:
		if (outDataSize) { *outDataSize = sizeof(AudioUnitParameterInfo); }
		return fn_parameter_id_is_known(inUnit, inScope, inElement) ? 0 : kAudioUnitErr_InvalidParameter;
	case kAudioUnitProperty_MaximumFramesPerSlice:
		if (outDataSize) { *outDataSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;
	case kAudioUnitProperty_SetRenderCallback:
		if (outDataSize) { *outDataSize = sizeof(AURenderCallbackStruct); }
		if (outWritable) { *outWritable = true; }
		return 0;
	case kAudioUnitProperty_CurrentPlaybackDevice:
		if (outDataSize) { *outDataSize = sizeof(AudioDeviceID); }
		if (outWritable) { *outWritable = true; }
		return 0;
	case kAudioUnitProperty_SampleRate:
		if (outDataSize) { *outDataSize = sizeof(Float64); }
		return 0;
	default:
		(void)inScope;
		return kAudioUnitErr_InvalidProperty;
	}
}

OSStatus AudioUnitGetProperty(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                              AudioUnitElement inElement, void *outData, UInt32 *ioDataSize)
{
	AudioStreamBasicDescription *format;

	if (inUnit == NULL || outData == NULL || ioDataSize == NULL) {
		return kAudio_ParamError;
	}

	switch (inID) {
	case kAudioUnitProperty_StreamFormat:
		format = fn_format_for(inUnit, inScope, inElement);
		if (format == NULL) {
			return kAudioUnitErr_InvalidElement;
		}
		if (*ioDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		*(AudioStreamBasicDescription *)outData = *format;
		*ioDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioUnitProperty_ElementCount:
		if (*ioDataSize < sizeof(UInt32)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		*(UInt32 *)outData = inUnit->element_count;
		*ioDataSize = sizeof(UInt32);
		return 0;

	case kAudioUnitProperty_ParameterList: {
		UInt32 count = fn_parameter_count(inUnit, inScope);
		UInt32 index = 0;

		if (*ioDataSize < count * (UInt32)sizeof(AudioUnitParameterID)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		if (count > 0) {
			AudioUnitParameterID *ids = outData;

			ids[index++] = kMultiChannelMixerParam_Volume;
			ids[index++] = kMultiChannelMixerParam_Enable;
		}
		*ioDataSize = index * (UInt32)sizeof(AudioUnitParameterID);
		return 0;
	}

	case kAudioUnitProperty_ParameterInfo: {
		AudioUnitParameterInfo *info = outData;

		if (*ioDataSize < sizeof(AudioUnitParameterInfo)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		if (!fn_parameter_id_is_known(inUnit, inScope, inElement)) {
			return kAudioUnitErr_InvalidParameter;
		}

		memset(info, 0, sizeof(*info));
		info->flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
		if (inElement == kMultiChannelMixerParam_Volume) {
			strncpy(info->name, "Volume", sizeof(info->name) - 1);
			info->unit = kAudioUnitParameterUnit_LinearGain;
			info->minValue = 0.0f;
			info->maxValue = 1.0f;
			info->defaultValue = 1.0f;
		} else {
			strncpy(info->name, "Enable", sizeof(info->name) - 1);
			info->unit = kAudioUnitParameterUnit_Boolean;
			info->minValue = 0.0f;
			info->maxValue = 1.0f;
			info->defaultValue = 1.0f;
		}
		*ioDataSize = sizeof(AudioUnitParameterInfo);
		return 0;
	}

	case kAudioUnitProperty_MaximumFramesPerSlice:
		if (*ioDataSize < sizeof(UInt32)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		*(UInt32 *)outData = inUnit->max_frames;
		*ioDataSize = sizeof(UInt32);
		return 0;

	case kAudioUnitProperty_CurrentPlaybackDevice:
		if (*ioDataSize < sizeof(AudioDeviceID)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		*(AudioDeviceID *)outData = inUnit->device;
		*ioDataSize = sizeof(AudioDeviceID);
		return 0;

	case kAudioUnitProperty_SampleRate:
		if (*ioDataSize < sizeof(Float64)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		*(Float64 *)outData = inUnit->out_format.mSampleRate;
		*ioDataSize = sizeof(Float64);
		return 0;

	default:
		return kAudioUnitErr_InvalidProperty;
	}
}

static OSStatus fn_rebuild_converter(AudioComponentInstance unit)
{
	if (unit->converter != NULL) {
		AudioConverterDispose(unit->converter);
		unit->converter = NULL;
	}

	if (unit->in_format.mFormatID == 0 || unit->out_format.mFormatID == 0) {
		return 0;	/* not configured yet; initialisation is where that is refused */
	}

	if (memcmp(&unit->in_format, &unit->out_format, sizeof(AudioStreamBasicDescription)) == 0) {
		return 0;	/* identical descriptions: no converter, a copy */
	}

	return AudioConverterNew(&unit->in_format, &unit->out_format, &unit->converter);
}

OSStatus AudioUnitSetProperty(AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope,
                              AudioUnitElement inElement, const void *inData, UInt32 inDataSize)
{
	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	switch (inID) {
	case kAudioUnitProperty_StreamFormat: {
		AudioStreamBasicDescription *format = fn_format_for(inUnit, inScope, inElement);
		OSStatus status;

		if (format == NULL) {
			return kAudioUnitErr_InvalidElement;
		}
		if (inData == NULL || inDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}

		*format = *(const AudioStreamBasicDescription *)inData;

		if (inUnit->kind == AGORA_UNIT_MIXER && inScope == kAudioUnitScope_Input) {
			/* A bus's format is the mixer's; a bus that disagrees would need a converter per bus, which is
			 * a graph's job rather than a mixer's. */
			if (format->mSampleRate != inUnit->out_format.mSampleRate
			    || format->mChannelsPerFrame != inUnit->out_format.mChannelsPerFrame) {
				return kAudioUnitErr_FormatNotSupported;
			}
			return 0;
		}

		status = fn_rebuild_converter(inUnit);
		return status;
	}

	case kAudioUnitProperty_ElementCount: {
		UInt32 count;

		if (inUnit->kind != AGORA_UNIT_MIXER) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		if (inData == NULL || inDataSize < sizeof(UInt32)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}

		count = *(const UInt32 *)inData;
		if (count == 0 || count > AGORA_UNIT_MAX_ELEMENTS) {
			return kAudioUnitErr_InvalidPropertyValue;
		}

		inUnit->element_count = count;
		{
			UInt32 i;

			for (i = 0; i < count; i++) {
				if (inUnit->buses[i].format.mFormatID == 0) {
					inUnit->buses[i].format = inUnit->out_format;
					inUnit->buses[i].volume = 1.0f;
					inUnit->buses[i].enabled = true;
				}
			}
		}
		return 0;
	}

	case kAudioUnitProperty_MaximumFramesPerSlice:
		if (inData == NULL || inDataSize < sizeof(UInt32)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		inUnit->max_frames = *(const UInt32 *)inData;
		return 0;

	case kAudioUnitProperty_SetRenderCallback:
		if (inData == NULL || inDataSize < sizeof(AURenderCallbackStruct)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		if (inUnit->kind == AGORA_UNIT_MIXER && inScope == kAudioUnitScope_Input) {
			/* A mixer's callback belongs to a BUS: each input is a source, and that is the whole point of a
			 * mixer. The global callback would be one source pretending to be several. */
			if (inElement >= inUnit->element_count) {
				return kAudioUnitErr_InvalidElement;
			}
			inUnit->buses[inElement].callback = *(const AURenderCallbackStruct *)inData;
			return 0;
		}
		inUnit->callback = *(const AURenderCallbackStruct *)inData;
		return 0;

	case kAudioUnitProperty_CurrentPlaybackDevice:
		if (inData == NULL || inDataSize < sizeof(AudioDeviceID)) {
			return kAudioUnitErr_InvalidPropertyValue;
		}
		inUnit->device = *(const AudioDeviceID *)inData;
		return 0;

	default:
		return kAudioUnitErr_InvalidProperty;
	}
}

OSStatus AudioUnitGetParameter(AudioUnit inUnit, AudioUnitParameterID inID, AudioUnitScope inScope,
                               AudioUnitElement inElement, AudioUnitParameterValue *outValue)
{
	if (inUnit == NULL || outValue == NULL) {
		return kAudio_ParamError;
	}

	if (inUnit->kind != AGORA_UNIT_MIXER || inScope != kAudioUnitScope_Input
	    || inElement >= inUnit->element_count) {
		return kAudioUnitErr_InvalidParameter;
	}

	switch (inID) {
	case kMultiChannelMixerParam_Volume: *outValue = inUnit->buses[inElement].volume; return 0;
	case kMultiChannelMixerParam_Enable: *outValue = inUnit->buses[inElement].enabled ? 1.0f : 0.0f; return 0;
	default: return kAudioUnitErr_InvalidParameter;
	}
}

OSStatus AudioUnitSetParameter(AudioUnit inUnit, AudioUnitParameterID inID, AudioUnitScope inScope,
                               AudioUnitElement inElement, AudioUnitParameterValue inValue,
                               UInt32 inBufferOffsetInFrames)
{
	(void)inBufferOffsetInFrames;

	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	if (inUnit->kind != AGORA_UNIT_MIXER || inScope != kAudioUnitScope_Input
	    || inElement >= inUnit->element_count) {
		return kAudioUnitErr_InvalidParameter;
	}

	switch (inID) {
	case kMultiChannelMixerParam_Volume:
		if (inValue < 0.0f) { inValue = 0.0f; }
		if (inValue > 1.0f) { inValue = 1.0f; }
		inUnit->buses[inElement].volume = inValue;
		return 0;
	case kMultiChannelMixerParam_Enable:
		inUnit->buses[inElement].enabled = inValue != 0.0f;
		return 0;
	default:
		return kAudioUnitErr_InvalidParameter;
	}
}

/* ------------------------------------------------------------------------------------------------
 * Initialisation.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioUnitInitialize(AudioUnit inUnit)
{
	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	if (inUnit->initialized) {
		return kAudioUnitErr_Initialized;
	}

	if (inUnit->kind == AGORA_UNIT_CONVERTER) {
		OSStatus status;

		if (inUnit->in_format.mFormatID == 0 || inUnit->out_format.mFormatID == 0) {
			return kAudioUnitErr_Uninitialized;	/* both formats must be set first */
		}

		status = fn_rebuild_converter(inUnit);
		if (status != 0) {
			return kAudioUnitErr_FailedInitialization;
		}
	}

	inUnit->initialized = true;
	return 0;
}

OSStatus AudioUnitUninitialize(AudioUnit inUnit)
{
	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	if (inUnit->running) {
		AudioOutputUnitStop(inUnit);
	}

	inUnit->initialized = false;
	return 0;
}

OSStatus AudioUnitReset(AudioUnit inUnit, AudioUnitScope inScope, AudioUnitElement inElement)
{
	(void)inScope;
	(void)inElement;

	if (inUnit == NULL) {
		return kAudio_ParamError;
	}

	if (inUnit->converter != NULL) {
		AudioConverterReset(inUnit->converter);
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Rendering.
 * ---------------------------------------------------------------------------------------------- */

static bool fn_ensure_scratch(AudioComponentInstance unit, UInt32 frames, UInt32 channels)
{
	size_t needed = (size_t)frames * channels;

	if (needed <= (size_t)unit->scratch_frames * unit->scratch_channels) {
		return true;
	}

	unit->scratch = realloc(unit->scratch, needed * sizeof(float));
	if (unit->scratch == NULL) {
		return false;
	}

	unit->scratch_frames = frames;
	unit->scratch_channels = channels;

	return true;
}

typedef struct {
	const uint8_t * bytes;
	UInt32          length;
	UInt32          delivered;
	UInt32          bytes_per_frame;
} unit_source;

static OSStatus fn_unit_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets, AudioBufferList *ioData,
                              AudioStreamPacketDescription **outDescription, void *user)
{
	unit_source *source = user;
	UInt32 wanted;
	(void)converter;
	(void)outDescription;

	if (source->delivered >= source->length) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	wanted = *ioNumberDataPackets;
	if (wanted * source->bytes_per_frame > source->length - source->delivered) {
		wanted = (source->length - source->delivered) / source->bytes_per_frame;
	}

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mDataByteSize = wanted * source->bytes_per_frame;
	ioData->mBuffers[0].mData = (void *)(source->bytes + source->delivered);

	source->delivered += wanted * source->bytes_per_frame;
	*ioNumberDataPackets = wanted;
	return 0;
}

static OSStatus fn_render_converter(AudioComponentInstance unit, AudioUnitRenderActionFlags *flags,
                                    const AudioTimeStamp *timestamp, UInt32 frames, AudioBufferList *ioData)
{
	UInt32 input_bytes_per_frame = fn_pcm_bytes_per_frame(&unit->in_format);
	UInt32 needed;
	uint8_t *input;
	OSStatus status;

	if (input_bytes_per_frame == 0) {
		return kAudioUnitErr_FormatNotSupported;
	}

	/* Enough input to produce `frames` at the output rate, plus a little for the resampler's reach. */
	needed = (UInt32)((double)frames * unit->in_format.mSampleRate
	                  / (unit->out_format.mSampleRate > 0.0 ? unit->out_format.mSampleRate : 1.0)) + 64;

	input = calloc((size_t)needed, input_bytes_per_frame);
	if (input == NULL) {
		return kAudio_MemFullError;
	}

	{
		AudioBufferList source_list;
		UInt32 got = needed;

		memset(&source_list, 0, sizeof(source_list));
		source_list.mNumberBuffers = 1;
		source_list.mBuffers[0].mNumberChannels = unit->in_format.mChannelsPerFrame;
		source_list.mBuffers[0].mDataByteSize = needed * input_bytes_per_frame;
		source_list.mBuffers[0].mData = input;

		if (unit->callback.inputProc != NULL) {
			status = unit->callback.inputProc(unit->callback.inputProcRefCon, flags, timestamp, 0, needed,
			                                  &source_list);
			if (status != 0) {
				free(input);
				return status;
			}
		}
		got = source_list.mBuffers[0].mDataByteSize / input_bytes_per_frame;

		if (unit->converter != NULL) {
			unit_source source;
			UInt32 converted = frames;

			source.bytes = input;
			source.length = got * input_bytes_per_frame;
			source.delivered = 0;
			source.bytes_per_frame = input_bytes_per_frame;

			status = AudioConverterFillComplexBuffer(unit->converter, fn_unit_input, &source, &converted,
			                                         ioData, NULL);
			free(input);
			return status;
		}

		/* Identical descriptions: the source buffer IS the destination's content. */
		memcpy(ioData->mBuffers[0].mData, input, (size_t)frames * input_bytes_per_frame);
		free(input);
		return 0;
	}
}

static OSStatus fn_render_mixer(AudioComponentInstance unit, AudioUnitRenderActionFlags *flags,
                                const AudioTimeStamp *timestamp, UInt32 frames, AudioBufferList *ioData)
{
	UInt32 channels = unit->out_format.mChannelsPerFrame;
	float *out = ioData->mBuffers[0].mData;
	UInt32 element;

	memset(out, 0, (size_t)frames * channels * sizeof(float));

	for (element = 0; element < unit->element_count; element++) {
		AudioBufferList bus;
		float *sink;
		UInt32 i;

		if (!unit->buses[element].enabled) {
			continue;
		}

		if (!fn_ensure_scratch(unit, frames, channels)) {
			return kAudio_MemFullError;
		}

		memset(unit->scratch, 0, (size_t)frames * channels * sizeof(float));

		memset(&bus, 0, sizeof(bus));
		bus.mNumberBuffers = 1;
		bus.mBuffers[0].mNumberChannels = channels;
		bus.mBuffers[0].mDataByteSize = frames * channels * (UInt32)sizeof(float);
		bus.mBuffers[0].mData = unit->scratch;

		if (unit->buses[element].callback.inputProc != NULL) {
			OSStatus status = unit->buses[element].callback.inputProc(
			    unit->buses[element].callback.inputProcRefCon, flags, timestamp, element, frames, &bus);

			if (status != 0) {
				continue;	/* a silent bus, not a failed render: one source is not the mix */
			}
		}

		sink = unit->scratch;
		for (i = 0; i < frames * channels; i++) {
			out[i] += sink[i] * unit->buses[element].volume;
		}
	}

	/* Saturate at the rails: a mixer sums, and the sum of two full-scale sources is a limit. */
	{
		UInt32 i;

		for (i = 0; i < frames * channels; i++) {
			if (out[i] > 1.0f) { out[i] = 1.0f; }
			if (out[i] < -1.0f) { out[i] = -1.0f; }
		}
	}

	return 0;
}

OSStatus AudioUnitRender(AudioUnit inUnit, AudioUnitRenderActionFlags *ioActionFlags,
                         const AudioTimeStamp *inTimeStamp, UInt32 inOutputBusNumber, UInt32 inNumberFrames,
                         AudioBufferList *ioData)
{
	if (inUnit == NULL || ioData == NULL) {
		return kAudio_ParamError;
	}

	if (inNumberFrames > inUnit->max_frames) {
		return kAudioUnitErr_TooManyFramesToProcess;
	}

	switch (inUnit->kind) {
	case AGORA_UNIT_OUTPUT:
		if (inUnit->callback.inputProc != NULL) {
			return inUnit->callback.inputProc(inUnit->callback.inputProcRefCon, ioActionFlags, inTimeStamp,
			                                 inOutputBusNumber, inNumberFrames, ioData);
		}
		/* No callback: an output unit with nothing to play plays silence, which is not an error. */
		memset(ioData->mBuffers[0].mData, 0,
		       (size_t)inNumberFrames * inUnit->out_format.mChannelsPerFrame * sizeof(float));
		return 0;

	case AGORA_UNIT_CONVERTER:
		return fn_render_converter(inUnit, ioActionFlags, inTimeStamp, inNumberFrames, ioData);

	case AGORA_UNIT_MIXER:
		return fn_render_mixer(inUnit, ioActionFlags, inTimeStamp, inNumberFrames, ioData);
	}

	return kAudioUnitErr_InvalidProperty;
}

/* ------------------------------------------------------------------------------------------------
 * Starting an output unit.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_output_render(AudioObjectID device, const AudioTimeStamp *now,
                                 const AudioBufferList *input, const AudioTimeStamp *input_time,
                                 AudioBufferList *output, const AudioTimeStamp *output_time, void *client)
{
	AudioComponentInstance unit = client;
	UInt32 frames;

	(void)device;
	(void)input;
	(void)input_time;
	(void)output_time;

	if (output == NULL || output->mNumberBuffers == 0 || output->mBuffers[0].mData == NULL) {
		return 0;
	}

	frames = output->mBuffers[0].mDataByteSize
	       / (output->mBuffers[0].mNumberChannels * (UInt32)sizeof(float));

	/* The device's buffer is the unit's output buffer: rendering into it directly is what makes an output
	 * unit cost nothing to add. */
	return AudioUnitRender(unit, NULL, now, 0, frames, output);
}

OSStatus AudioOutputUnitStart(AudioUnit ci)
{
	OSStatus status;
	AudioDeviceIOProcID id = NULL;

	if (ci == NULL) {
		return kAudio_ParamError;
	}

	if (ci->kind != AGORA_UNIT_OUTPUT) {
		return kAudioUnitErr_InvalidProperty;	/* only an output unit has a clock to start */
	}

	if (ci->running) {
		return 0;
	}

	if (!ci->initialized) {
		return kAudioUnitErr_Uninitialized;
	}

	status = AudioDeviceCreateIOProcID(ci->device, fn_output_render, ci, &id);
	if (status != 0) {
		return kAudioUnitErr_FailedInitialization;
	}

	status = AudioDeviceStart(ci->device, id);
	if (status != 0) {
		AudioDeviceDestroyIOProcID(ci->device, id);
		return kAudioUnitErr_FailedInitialization;
	}

	ci->io_id = id;
	ci->running = true;

	return 0;
}

OSStatus AudioOutputUnitStop(AudioUnit ci)
{
	if (ci == NULL) {
		return kAudio_ParamError;
	}

	if (ci->running) {
		ci->running = false;
		AudioDeviceStop(ci->device, fn_output_render);
		AudioDeviceDestroyIOProcID(ci->device, fn_output_render);
		ci->io_id = NULL;
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * The declared-and-refused doors.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioUnitProcess(AudioUnit inUnit, AudioUnitRenderActionFlags *ioActionFlags,
                          const AudioTimeStamp *inTimeStamp, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	(void)inUnit;
	(void)ioActionFlags;
	(void)inTimeStamp;
	(void)inNumberFrames;
	(void)ioData;

	return kAudioUnitErr_CannotDoInCurrentContext;
}

OSStatus AudioUnitScheduleParameters(AudioUnit inUnit, const void *inParameterEvent, UInt32 inNumParamEvents)
{
	(void)inUnit;
	(void)inParameterEvent;
	(void)inNumParamEvents;

	return kAudioUnitErr_CannotDoInCurrentContext;
}

OSStatus AudioUnitAddRenderNotify(AudioUnit inUnit, AURenderCallback inProc, void *inProcUserData)
{
	(void)inUnit;
	(void)inProc;
	(void)inProcUserData;

	return kAudioUnitErr_InvalidProperty;
}

OSStatus AudioUnitRemoveRenderNotify(AudioUnit inUnit, AURenderCallback inProc, void *inProcUserData)
{
	(void)inUnit;
	(void)inProc;
	(void)inProcUserData;

	return kAudioUnitErr_InvalidProperty;
}
