/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * unit.c — the units and the registry, measured through the mixer and the device.
 *
 * THREE KINDS OF EVIDENCE FOR THREE KINDS OF UNIT. The output unit is proved audible by the device's own mixed
 * peak; the converter is proved by a sample value that had to change format and come out the other side; the
 * mixer by two buses that must add. And the registry is proved in both directions: a description that names a
 * built-in finds it, and one that names nothing finds nothing — instead of finding a stub.
 */

#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include <AudioToolbox/AUGraph.h>
#include "AudioToolbox_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

static void fn_sleep_ms(long milliseconds)
{
	struct timespec ts;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0) {
	}
}

static AudioDeviceID fn_default_output(void)
{
	AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice,
	                                       kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMaster };
	AudioDeviceID device = kAudioObjectUnknown;
	UInt32 size = sizeof(device);

	AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device);
	return device;
}

static AudioStreamBasicDescription fn_pcm(double rate, UInt32 channels, UInt32 bits, AudioFormatFlags flags)
{
	AudioStreamBasicDescription asbd;

	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate = rate;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = flags;
	asbd.mChannelsPerFrame = channels;
	asbd.mBitsPerChannel = bits;
	asbd.mBytesPerFrame = (bits / 8) * channels;
	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	asbd.mFramesPerPacket = 1;

	return asbd;
}

static AudioComponent fn_find(OSType type, OSType subtype)
{
	AudioComponentDescription description;

	memset(&description, 0, sizeof(description));
	description.componentType = type;
	description.componentSubType = subtype;
	description.componentManufacturer = kAudioUnitManufacturer_Apple;

	return AudioComponentFindNext(NULL, &description);
}

/* ------------------------------------------------------------------------------------------------
 * The registry.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_registry(void)
{
	AudioComponent output = fn_find(kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput);
	AudioComponentDescription description;
	CFStringRef name = NULL;

	fn_check("the default output unit is in the registry", output != NULL);

	fn_check("a unit's description comes back",
	         AudioComponentGetDescription(output, &description) == 0
	         && description.componentType == kAudioUnitType_Output
	         && description.componentSubType == kAudioUnitSubType_DefaultOutput
	         && description.componentManufacturer == kAudioUnitManufacturer_Apple);

	fn_check("a unit has a name",
	         AudioComponentCopyName(output, &name) == 0 && name != NULL);

	fn_check("a description with no manufacturer matches by type and subtype",
	         AudioComponentCount(&(AudioComponentDescription){ kAudioUnitType_Mixer,
	                                                           kAudioUnitSubType_MultiChannelMixer, 0, 0, 0 }) == 1);

	fn_check("a description that names nothing finds nothing",
	         fn_find(kAudioUnitType_Effect, 'zzzz') == NULL);

	fn_check("the mixer and the converter are in the registry",
	         fn_find(kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer) != NULL
	         && fn_find(kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter) != NULL);
}

/* ------------------------------------------------------------------------------------------------
 * The output unit.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	float value;
	AudioStreamBasicDescription format;
} tone_state;

static OSStatus fn_tone(void *refCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                        UInt32 bus, UInt32 frames, AudioBufferList *ioData)
{
	tone_state *state = refCon;
	float *samples;
	UInt32 count;
	UInt32 i;

	(void)flags;
	(void)timestamp;
	(void)bus;

	if (ioData == NULL || ioData->mNumberBuffers == 0) {
		return 0;
	}

	samples = ioData->mBuffers[0].mData;
	count = frames * ioData->mBuffers[0].mNumberChannels;
	for (i = 0; i < count; i++) {
		samples[i] = state->value;
	}

	return 0;
}

static void fn_test_output_unit(void)
{
	AudioDeviceID device = fn_default_output();
	AudioComponent component = fn_find(kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput);
	AudioUnit unit = NULL;
	AudioStreamBasicDescription format;
	tone_state tone;
	AURenderCallbackStruct callback;
	float peak = 0.0f;
	OSStatus status;

	memset(&tone, 0, sizeof(tone));
	tone.value = 0.5f;

	status = AudioComponentInstanceNew(component, &unit);
	fn_check("the output unit instantiates", status == 0 && unit != NULL);
	if (status != 0) {
		return;
	}

	/* The unit's input scope is what the caller fills; ask for the device's own description. */
	{
		UInt32 size = sizeof(format);

		memset(&format, 0, sizeof(format));
		fn_check("the output unit reports its format",
		         AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
		                              &format, &size) == 0
		         && format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 32);
	}

	{
		UInt32 size = sizeof(format);
		AudioFormatFlags flags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		AudioStreamBasicDescription wanted = fn_pcm(48000.0, 2, 32, flags);
		OSStatus set_status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
		                                           kAudioUnitScope_Input, 0, &wanted, sizeof(wanted));

		fn_check("the unit accepts the device's format", set_status == 0);
		(void)size;
	}

	callback.inputProc = fn_tone;
	callback.inputProcRefCon = &tone;
	fn_check("a render callback is set",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0,
	                              &callback, sizeof(callback)) == 0);

	fn_check("the unit initialises", AudioUnitInitialize(unit) == 0);
	fn_check("the unit starts", AudioOutputUnitStart(unit) == 0);

	fn_sleep_ms(200);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the output unit is audible at the device", peak > 0.45f && peak < 0.55f);

	fn_check("the unit stops", AudioOutputUnitStop(unit) == 0);

	/* Refusals. */
	{
		UInt32 value = 0;
		UInt32 size = sizeof(value);

		fn_check("an unknown unit property is refused by name",
		         AudioUnitGetProperty(unit, 0x7A7A7A7A, kAudioUnitScope_Global, 0, &value, &size)
		         == kAudioUnitErr_InvalidProperty);
	}

	fn_check("the unit is disposed", AudioComponentInstanceDispose(unit) == 0);
}

/* ------------------------------------------------------------------------------------------------
 * The converter unit.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	float value;	/* the value to put in each sample, in the source format */
} source_state;

/* Writes int16 samples at 0.25 full scale into whatever buffer it is handed. */
static OSStatus fn_int16_source(void *refCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                                UInt32 bus, UInt32 frames, AudioBufferList *ioData)
{
	source_state *state = refCon;
	int16_t *samples;
	UInt32 count;
	UInt32 i;

	(void)flags;
	(void)timestamp;
	(void)bus;

	samples = ioData->mBuffers[0].mData;
	count = frames * ioData->mBuffers[0].mNumberChannels;
	for (i = 0; i < count; i++) {
		samples[i] = (int16_t)(state->value * 32767.0f);
	}

	ioData->mBuffers[0].mDataByteSize = count * (UInt32)sizeof(int16_t);
	return 0;
}

static void fn_test_converter_unit(void)
{
	AudioComponent component = fn_find(kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter);
	AudioUnit unit = NULL;
	AudioStreamBasicDescription source_format = fn_pcm(48000.0, 1, 16,
	                                                   kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription sink_format = fn_pcm(48000.0, 1, 32,
	                                                 kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	source_state state = { 0.25f };
	AURenderCallbackStruct callback;
	float rendered[256];
	AudioBufferList list;
	OSStatus status;

	status = AudioComponentInstanceNew(component, &unit);
	fn_check("the converter unit instantiates", status == 0 && unit != NULL);
	if (status != 0) {
		return;
	}

	fn_check("the converter takes a source format",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
	                              &source_format, sizeof(source_format)) == 0);
	fn_check("the converter takes a destination format",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
	                              &sink_format, sizeof(sink_format)) == 0);

	callback.inputProc = fn_int16_source;
	callback.inputProcRefCon = &state;
	fn_check("the converter's source is set",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0,
	                              &callback, sizeof(callback)) == 0);

	fn_check("the converter initialises", AudioUnitInitialize(unit) == 0);

	memset(rendered, 0, sizeof(rendered));
	memset(&list, 0, sizeof(list));
	list.mNumberBuffers = 1;
	list.mBuffers[0].mNumberChannels = 1;
	list.mBuffers[0].mDataByteSize = (UInt32)sizeof(rendered);
	list.mBuffers[0].mData = rendered;

	{
		AudioTimeStamp now;

		memset(&now, 0, sizeof(now));
		fn_check("the converter renders", AudioUnitRender(unit, NULL, &now, 0, 256, &list) == 0);
	}
	fn_check("int16 became float32 with its value",
	         rendered[0] > 0.24f && rendered[0] < 0.26f && rendered[255] > 0.24f && rendered[255] < 0.26f);

	AudioComponentInstanceDispose(unit);
}

/* ------------------------------------------------------------------------------------------------
 * The mixer unit.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_mixer_unit(void)
{
	AudioComponent component = fn_find(kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer);
	AudioUnit unit = NULL;
	AudioStreamBasicDescription format = fn_pcm(48000.0, 1, 32,
	                                            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	source_state loud = { 0.25f };
	source_state quiet = { 0.125f };
	AURenderCallbackStruct callback;
	float rendered[256];
	AudioBufferList list;
	UInt32 buses = 2;
	OSStatus status;

	status = AudioComponentInstanceNew(component, &unit);
	fn_check("the mixer instantiates", status == 0 && unit != NULL);
	if (status != 0) {
		return;
	}

	fn_check("the mixer takes two buses",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0,
	                              &buses, sizeof(buses)) == 0);

	fn_check("the mixer's output format is set",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
	                              &format, sizeof(format)) == 0);
	fn_check("bus 0's format is set",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
	                              &format, sizeof(format)) == 0);
	fn_check("bus 1's format is set",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
	                              &format, sizeof(format)) == 0);

	callback.inputProc = fn_tone;
	callback.inputProcRefCon = &loud;
	fn_check("bus 0 has a source",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
	                              &callback, sizeof(callback)) == 0);

	callback.inputProcRefCon = &quiet;
	fn_check("bus 1 has a source",
	         AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 1,
	                              &callback, sizeof(callback)) == 0);

	fn_check("the mixer initialises", AudioUnitInitialize(unit) == 0);

	memset(rendered, 0, sizeof(rendered));
	memset(&list, 0, sizeof(list));
	list.mNumberBuffers = 1;
	list.mBuffers[0].mNumberChannels = 1;
	list.mBuffers[0].mDataByteSize = (UInt32)sizeof(rendered);
	list.mBuffers[0].mData = rendered;

	{
		AudioTimeStamp now;

		memset(&now, 0, sizeof(now));
		fn_check("the mixer renders", AudioUnitRender(unit, NULL, &now, 0, 256, &list) == 0);
	}
	fn_check("the two buses summed", rendered[0] > 0.37f && rendered[0] < 0.38f);

	/* A bus's volume scales only that bus. */
	fn_check("bus 0's volume is set",
	         AudioUnitSetParameter(unit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, 0.5f, 0) == 0);
	{
		AudioTimeStamp now;

		memset(&now, 0, sizeof(now));
		fn_check("the mixer renders again", AudioUnitRender(unit, NULL, &now, 0, 256, &list) == 0);
	}
	fn_check("halving one bus halved its contribution", rendered[0] > 0.24f && rendered[0] < 0.26f);

	{
		AudioUnitParameterValue value = 0.0f;

		fn_check("the bus volume reads back",
		         AudioUnitGetParameter(unit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, &value) == 0
		         && value == 0.5f);
	}

	AudioComponentInstanceDispose(unit);
}

/* ------------------------------------------------------------------------------------------------
 * The graph.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_graph(void)
{
	AudioDeviceID device = fn_default_output();
	AUGraph graph = NULL;
	AUNode mixer = -1;
	AUNode output = -1;
	AudioComponentDescription mixer_desc = { kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer,
	                                         kAudioUnitManufacturer_Apple, 0, 0 };
	AudioComponentDescription output_desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
	                                          kAudioUnitManufacturer_Apple, 0, 0 };
	AudioUnit mixer_unit = NULL;
	source_state loud = { 0.25f };
	source_state quiet = { 0.125f };
	AURenderCallbackStruct callback;
	UInt32 buses = 2;
	UInt32 count = 0;
	float peak = 0.0f;
	Boolean flag = false;

	fn_check("a graph is created", NewAUGraph(&graph) == 0 && graph != NULL);
	if (graph == NULL) {
		return;
	}

	fn_check("the mixer node is added",
	         AUGraphAddNode(graph, &mixer_desc, &mixer) == 0 && mixer >= 0);
	fn_check("the output node is added",
	         AUGraphAddNode(graph, &output_desc, &output) == 0 && output >= 0);

	fn_check("the graph reports two nodes",
	         AUGraphGetNodeCount(graph, &count) == 0 && count == 2);

	{
		AUNode first = -1;
		AudioComponentDescription described;

		fn_check("a node can be found by index",
		         AUGraphGetIndNode(graph, 0, &first) == 0 && first == mixer);
		fn_check("a node's description comes back",
		         AUGraphNodeInfo(graph, mixer, &described, &mixer_unit) == 0
		         && described.componentSubType == kAudioUnitSubType_MultiChannelMixer
		         && mixer_unit != NULL);
	}

	/* Two buses, each with its own source, both feeding the mixer directly. */
	fn_check("the mixer takes two buses",
	         AudioUnitSetProperty(mixer_unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0,
	                              &buses, sizeof(buses)) == 0);

	callback.inputProc = fn_tone;
	callback.inputProcRefCon = &loud;
	fn_check("bus 0's source is set on the graph",
	         AUGraphSetNodeInputCallback(graph, mixer, 0, &callback) == 0);

	callback.inputProcRefCon = &quiet;
	fn_check("bus 1's source is set on the graph",
	         AUGraphSetNodeInputCallback(graph, mixer, 1, &callback) == 0);

	fn_check("the mixer feeds the output",
	         AUGraphConnectNodeInput(graph, mixer, 0, output, 0) == 0);
	fn_check("the graph counts one connection",
	         AUGraphGetNumberOfConnections(graph, &count) == 0 && count == 1);

	/* A connection back the other way would close a ring, and a ring is not a graph. */
	fn_check("a cycle is refused",
	         AUGraphConnectNodeInput(graph, output, 0, mixer, 0) == kAUGraphErr_InvalidConnection);
	fn_check("an unknown node is refused",
	         AUGraphConnectNodeInput(graph, 99, 0, output, 0) == kAUGraphErr_NodeNotFound);

	fn_check("the graph opens", AUGraphOpen(graph) == 0);
	fn_check("the graph initialises", AUGraphInitialize(graph) == 0);

	fn_check("the graph starts", AUGraphStart(graph) == 0);
	fn_check("the graph says it is running", AUGraphIsRunning(graph, &flag) == 0 && flag);

	fn_sleep_ms(200);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the graph's mix reaches the device", peak > 0.37f && peak < 0.38f);

	fn_check("the graph stops", AUGraphStop(graph) == 0);
	fn_check("the graph says it is stopped", AUGraphIsRunning(graph, &flag) == 0 && !flag);

	fn_check("the graph is disposed", DisposeAUGraph(graph) == 0);
}

int main(void);
int main(void)
{
	/* The suite states its backend in code, so a developer's shell cannot change what it measured. */
	fn_agora_audio_set_driver("null");

	fn_test_registry();
	fn_test_output_unit();
	fn_test_converter_unit();
	fn_test_mixer_unit();
	fn_test_graph();

	if (g_failures != 0) {
		fprintf(stderr, "unit: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("unit: the registry, an output unit audible at the device, a conversion and a mix\n");
	return 0;
}
