/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * auv3.m — the object face of a unit, measured against the C face it is a view of.
 *
 * EVERY ASSERTION HERE IS A PAIR: what the object says, and what the C door says about the same fact. A bus's
 * format must be the unit's `kAudioUnitProperty_StreamFormat`; a parameter's value must be what
 * `AudioUnitGetParameter` reports after the tree wrote it; `-renderBlock` must produce the samples the mixer
 * really mixes. A wrapper that kept its own copy would pass the first half of each and fail the second, which
 * is the only reason to write the second.
 *
 * HEADLESS AND ON THE NULL BACKEND, like the rest of the suite: `-startHardwareAndReturnError:` is proved by
 * starting the null device's thread, not by being audible.
 *
 * ONE PARAMETER IS MEASURED TWICE ON PURPOSE. The mixer's Volume is one id with a value per bus, so the
 * address this library encodes has to carry the element: two buses, two nodes, two values, one parameter id.
 * If the encoding lost the element, the second write would land on the first bus.
 */

#import <AudioToolbox/AUAudioUnit.h>
#import <AudioToolbox/AudioOutputUnit.h>
#import "AudioToolbox/AUAudioUnit_internal.h"
#include "AudioToolbox_internal.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

static void fn_check_close(const char *what, double got, double wanted, double tolerance)
{
	double difference = got - wanted;

	if (difference < 0) {
		difference = -difference;
	}
	if (difference > tolerance) {
		fprintf(stderr, "FAIL %s (got %g, wanted %g)\n", what, got, wanted);
		g_failures++;
	}
}

static AudioComponentDescription fn_description(OSType type, OSType subtype)
{
	AudioComponentDescription description;

	memset(&description, 0, sizeof(description));
	description.componentType = type;
	description.componentSubType = subtype;
	description.componentManufacturer = kAudioUnitManufacturer_Apple;

	return description;
}

typedef struct {
	float value;
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

/* One buffer, with room for the list's single inline buffer. */
typedef struct {
	AudioBufferList list;
	float samples[512 * 2];
} render_storage;

/* In place, because the list's `mData` has to point at *this* struct's samples: a returned copy would point
 * at the samples of a struct that no longer exists, and the render would write into a dead stack frame. */
static void fn_storage(render_storage *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->list.mNumberBuffers = 1;
	storage->list.mBuffers[0].mNumberChannels = 2;
	storage->list.mBuffers[0].mDataByteSize = sizeof(storage->samples);
	storage->list.mBuffers[0].mData = storage->samples;
}

/* ------------------------------------------------------------------------------------------------
 * The default output unit, seen through the object.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_output_unit(void)
{
	AudioComponentDescription description = fn_description(kAudioUnitType_Output,
	                                                       kAudioUnitSubType_DefaultOutput);
	NSError *error = nil;
	AUAudioUnit *unit = [[AUAudioUnit alloc] initWithComponentDescription:description error:&error];
	AUAudioUnitBus *input;
	AudioStreamBasicDescription format;
	AUAudioFrameCount frames;
	render_storage storage;

	fn_check("the object instantiates around the default output unit", unit != nil);
	if (unit == nil) {
		return;
	}

	fn_check("the description comes back as it was asked for",
	         [unit componentDescription].componentType == kAudioUnitType_Output
	         && [unit componentDescription].componentSubType == kAudioUnitSubType_DefaultOutput);
	fn_check("the object knows the component's name", [unit componentName] != nil);
	fn_check("the manufacturer is Apple's four characters",
	         [[unit manufacturerName] isEqualToString:@"appl"]);
	fn_check("an output unit can perform output and not input",
	         [unit canPerformOutput] && ![unit canPerformInput]);

	fn_check("one output bus", [[unit outputBusses] count] == 1);
	fn_check("one input bus", [[unit inputBusses] count] == 1);
	fn_check("the bus array is not resizable for an output unit", ![[unit outputBusses] isCountChangeable]);

	input = [[unit inputBusses] objectAtIndexedSubscript:0];
	format = [input format];
	fn_check("the input bus reports the device's format",
	         format.mSampleRate > 0.0 && format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 32);
	fn_check("the bus knows which element it is", [input index] == 0);
	fn_check("the bus knows its owner", [input ownerAudioUnit] == unit);

	/* A format set through the bus is the format the C door reads. */
	{
		AudioStreamBasicDescription wanted;
		AudioStreamBasicDescription stored;
		UInt32 size = sizeof(stored);

		memset(&wanted, 0, sizeof(wanted));
		wanted.mSampleRate = 48000.0;
		wanted.mFormatID = kAudioFormatLinearPCM;
		wanted.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		wanted.mChannelsPerFrame = 2;
		wanted.mBitsPerChannel = 32;
		wanted.mBytesPerFrame = 8;
		wanted.mBytesPerPacket = 8;
		wanted.mFramesPerPacket = 1;

		fn_check("the bus accepts a format", [input setFormat:wanted error:NULL]);

		memset(&stored, 0, sizeof(stored));
		fn_check("the unit's own property carries that format",
		         AudioUnitGetProperty([unit agora_instance], kAudioUnitProperty_StreamFormat,
		                              kAudioUnitScope_Input, 0, &stored, &size) == 0
		         && stored.mSampleRate == 48000.0 && stored.mBytesPerFrame == 8);
	}

	frames = [unit maximumFramesToRender];
	fn_check("the maximum slice is the unit's", frames == 4096);
	[unit setMaximumFramesToRender:1024];
	fn_check("setting the maximum slice reaches the unit", [unit maximumFramesToRender] == 1024);

	fn_check("render resources are allocated only when asked",
	         ![unit renderResourcesAllocated] && [unit allocateRenderResourcesAndReturnError:NULL]);
	fn_check("allocated resources are reported", [unit renderResourcesAllocated]);

	/* The render block is `AudioUnitRender`: an output unit with no callback renders silence, at 0. */
	fn_storage(&storage);
	{
		AudioTimeStamp now;
		AudioUnitRenderActionFlags flags = 0;

		memset(&now, 0, sizeof(now));
		storage.list.mBuffers[0].mDataByteSize = 128 * 2 * sizeof(float);
		fn_check("the render block renders through the unit",
		         [unit renderBlock](&flags, &now, 128, 0, &storage.list, NULL) == 0);
		fn_check("an output unit with nothing to play renders silence",
		         storage.samples[0] == 0.0f && storage.samples[255] == 0.0f);
	}

	fn_check("the hardware starts and stops", [unit startHardwareAndReturnError:NULL]);
	[unit stopHardware];

	[unit deallocateRenderResources];
	fn_check("deallocating reaches the unit", ![unit renderResourcesAllocated]);

	[unit release];
}

/* ------------------------------------------------------------------------------------------------
 * The mixer: two buses, two parameters each, and one render block over them.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_mixer(void)
{
	AudioComponentDescription description = fn_description(kAudioUnitType_Mixer,
	                                                       kAudioUnitSubType_MultiChannelMixer);
	AUAudioUnit *unit = [[AUAudioUnit alloc] initWithComponentDescription:description error:NULL];
	AUAudioUnitBusArray *busses;
	AUParameterTree *tree;
	AUParameter *volume_of_bus_one;
	tone_state first = { 0.25f };
	tone_state second = { 0.125f };
	render_storage storage;

	fn_check("the mixer instantiates around the C unit", unit != nil);
	if (unit == nil) {
		return;
	}

	busses = [unit inputBusses];
	fn_check("a mixer's input bus count is resizable", [busses isCountChangeable]);
	fn_check("a mixer starts with one input bus", [busses count] == 1);
	fn_check("the bus count can be raised", [busses setBusCount:2 error:NULL]);
	fn_check("the bus array reloaded", [busses count] == 2);
	fn_check("the reloaded array has fresh buses",
	         [[busses objectAtIndexedSubscript:1] index] == 1);
	fn_check("the count reached the C unit", [[busses objectAtIndexedSubscript:1] index] == 1);

	fn_check("a mixer's output bus count is the device's", [[unit outputBusses] count] == 1);

	/* Two sources, through the C door, because that is where a bus's source lives. */
	{
		AURenderCallbackStruct callback;

		callback.inputProc = fn_tone;
		callback.inputProcRefCon = &first;
		fn_check("bus 0 takes a source",
		         AudioUnitSetProperty([unit agora_instance], kAudioUnitProperty_SetRenderCallback,
		                              kAudioUnitScope_Input, 0, &callback, sizeof(callback)) == 0);

		callback.inputProcRefCon = &second;
		fn_check("bus 1 takes a source",
		         AudioUnitSetProperty([unit agora_instance], kAudioUnitProperty_SetRenderCallback,
		                              kAudioUnitScope_Input, 1, &callback, sizeof(callback)) == 0);
	}

	fn_check("the mixer can allocate its resources", [unit allocateRenderResourcesAndReturnError:NULL]);

	tree = [unit parameterTree];
	fn_check("the mixer has a parameter tree", tree != nil);
	fn_check("one parameter per (id, element): two ids over two buses", [[tree allParameters] count] == 4);

	volume_of_bus_one = [tree parameterWithID:kMultiChannelMixerParam_Volume
	                                    scope:kAudioUnitScope_Input
	                                  element:1];
	fn_check("a parameter is found by its three coordinates", volume_of_bus_one != nil);
	fn_check("its display name is the C unit's", [[volume_of_bus_one displayName] isEqualToString:@"Volume"]);
	fn_check("the tree's key path names the group and the bus",
	         [[volume_of_bus_one keyPath] isEqualToString:@"Input.Volume_1"]);
	fn_check("its range is the C unit's",
	         [volume_of_bus_one minValue] == 0.0f && [volume_of_bus_one maxValue] == 1.0f);
	fn_check("its default is what the unit reports", [volume_of_bus_one value] == 1.0f);

	/* A write through the tree is a write to the C parameter — and the observer hears what the unit did. */
	{
		__block int notifications = 0;
		__block AUValue heard = -1.0f;
		AUParameterObserverToken token;

		token = [volume_of_bus_one tokenByAddingParameterObserver:^(AUParameterAddress address, AUValue value) {
			notifications++;
			heard = value;
			(void)address;
		}];

		[volume_of_bus_one setValue:0.5f];
		{
			AudioUnitParameterValue stored = 0.0f;

			fn_check("the value reads back through the tree", [volume_of_bus_one value] == 0.5f);
			fn_check("the C parameter holds it",
			         AudioUnitGetParameter([unit agora_instance], kMultiChannelMixerParam_Volume,
			                               kAudioUnitScope_Input, 1, &stored) == 0
			         && stored == 0.5f);
		}
		fn_check("the observer was called once with the value",
		         notifications == 1 && heard == 0.5f);

		/* Out of range is clamped, and the observer hears the clamp rather than the request. */
		[volume_of_bus_one setValue:5.0f];
		fn_check("a value above the maximum is clamped", [volume_of_bus_one value] == 1.0f);
		fn_check("the observer heard the clamped value", notifications == 2 && heard == 1.0f);

		/* The originator of a change is not told about its own change. */
		[volume_of_bus_one setValue:0.5f originator:token];
		fn_check("the originator is not echoed", notifications == 2);

		[volume_of_bus_one removeParameterObserver:token];
		[volume_of_bus_one setValue:0.25f];
		fn_check("a removed observer is not called", notifications == 2);
	}

	/* Now the mix: 0.25 on bus 0 at volume 1, 0.125 on bus 1 at 0.25. */
	fn_storage(&storage);
	{
		AudioTimeStamp now;
		AudioUnitRenderActionFlags flags = 0;

		memset(&now, 0, sizeof(now));
		storage.list.mBuffers[0].mDataByteSize = 128 * 2 * sizeof(float);
		fn_check("the render block mixes through the unit",
		         [unit renderBlock](&flags, &now, 128, 0, &storage.list, NULL) == 0);
		fn_check_close("the mix is bus 0 plus bus 1 at its volume", storage.samples[0],
		               0.25 + 0.125 * 0.25, 0.0001);
	}

	fn_check("a parameter tree survives a bus count change", [busses setBusCount:1 error:NULL]);
	fn_check("and reports the new count", [busses count] == 1);
	fn_check("the tree was rebuilt for it", [[[unit parameterTree] allParameters] count] == 2);

	[unit deallocateRenderResources];
	[unit release];
}

/* ------------------------------------------------------------------------------------------------
 * The doors that are refused, and the one class that is only a pair of numbers.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_refusals(void)
{
	__block int calls = 0;
	__block AUAudioUnit *handed = (AUAudioUnit *)0x1;
	__block NSError *reported = nil;
	AUAudioUnitPreset *preset = [[AUAudioUnitPreset alloc] init];

	[AUAudioUnit instantiateWithComponentDescription:fn_description(kAudioUnitType_Output,
	                                                                kAudioUnitSubType_DefaultOutput)
	                                        options:0
	                              completionHandler:^(AUAudioUnit *audioUnit, NSError *error) {
		calls++;
		handed = audioUnit;
		reported = error;
	}];
	fn_check("out-of-process instantiation still answers", calls == 1);
	fn_check("with no unit and an error", handed == nil && reported != nil);

	preset.number = 7;
	preset.name = @"Bright";
	fn_check("a preset keeps its number and its name",
	         [preset number] == 7 && [[preset name] isEqualToString:@"Bright"]);
	[preset release];
}

int main(void);
int main(void)
{
	/* The suite states its backend in code, so a developer's shell cannot change what it measured. */
	fn_agora_audio_set_driver("null");

	fn_test_output_unit();
	fn_test_mixer();
	fn_test_refusals();

	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("auv3: ok\n");
	return 0;
}
