/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardware.c — the object model, the properties, the listeners, and the mixer.
 *
 * ONE TABLE OF OBJECTS AND ONE SWITCH PER KIND. That is deliberate: the HAL's API is "ask an object for a
 * property", so the implementation is a dispatch from `(object, selector, scope, element)` to an answer, and
 * everything else — the device list, the default device, a stream's format — is a case in that dispatch
 * rather than a function of its own. A backend supplies device *descriptions* and a thread; it never answers
 * a property, which is why the same property code will serve PipeWire and ALSA unchanged.
 *
 * THE MIXER IS HERE, NOT IN THE BACKEND. Two io procs attached to one device must sum, whatever the backend
 * does with the result, because that is what the HAL promises on macOS and a client cannot tell which
 * backend it has. The sum saturates at the rails and is scaled by the device's volume once, after mixing —
 * the same order a hardware mixer uses.
 *
 * LISTENERS ARE COMPARED ON THEIR ADDRESS, INCLUDING WILDCARDS: a listener registered for the wildcard
 * element hears every element, which is how a client watches "the volume" without naming a channel.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <CoreFoundation/CFString.h>
#include <dispatch/dispatch.h>
#include <math.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AGORA_MAX_DEVICES   8
#define AGORA_MAX_STREAMS   16
#define AGORA_MAX_IOPROCS   8
#define AGORA_MAX_LISTENERS 32

typedef struct {
	bool                       active;
	AudioDeviceIOProc          proc;
	AudioDeviceIOBlock         block;
	dispatch_queue_t           queue;
	void *                     client;
	UInt64                     calls;
} agora_ioproc;

typedef struct {
	bool                             active;
	AudioObjectPropertySelector      selector;
	AudioObjectPropertyScope         scope;
	AudioObjectPropertyElement       element;
	AudioObjectPropertyListenerProc  proc;
	AudioObjectPropertyListenerBlock block;
	dispatch_queue_t                 queue;
	void *                           client;
} agora_listener;

typedef struct {
	AudioStreamID               id;
	AudioDeviceID               device;
	bool                        output;
	UInt32                      channels;
	AudioStreamBasicDescription format;
	UInt32                      terminal_type;
	UInt32                      starting_channel;
} agora_stream;

typedef struct {
	AudioDeviceID     id;
	char              name[128];
	char              uid[128];
	char              manufacturer[128];
	UInt32            transport;
	Float64           nominal_rate;
	UInt32            buffer_frames;
	UInt32            channels;
	Float64           volume;
	bool              mute;
	bool              running;
	bool              has_input;
	bool              has_output;

	agora_stream      streams[AGORA_MAX_STREAMS];
	UInt32            stream_count;

	agora_ioproc      procs[AGORA_MAX_IOPROCS];

	/* What the mixer produced, for the suite. */
	float             last_peak;
	UInt64            frames_rendered;

	/* The device's clock, as of the last render: enough for `AudioDeviceGetCurrentTime` to answer with
	 * something that is not a lie. */
	UInt64            last_host_time;
	Float64           last_sample_time;
} agora_device;

static struct {
	bool                initialised;
	const agora_driver *driver;
	agora_device        devices[AGORA_MAX_DEVICES];
	UInt32              device_count;
	agora_listener      listeners[AGORA_MAX_LISTENERS];
} g_hal;

static void fn_init(void);

/* What a caller gets when it does not choose: the session's own sound server if one answers, then ALSA, and the null
 * device otherwise. A property read cannot ask "would a stream work?" without opening one, so the probe is the device
 * count — PipeWire describes no sinks without its daemon, sndio opens no handle without its server, and ALSA *opens the
 * PCM* (`fn_alsa_device_count` is a real `snd_pcm_open`/`close`, added in phase 7 for exactly this reason) rather than
 * describing a card that may not be there. **That open is what makes ALSA safe in this chain**: before it, "no sound
 * card" and "a sound card" were the same answer from ALSA, so choosing it automatically would have turned a container
 * into playback errors. The order is the platform's own: PipeWire on Linux, sndio on the BSDs, ALSA on a bare machine
 * with a card and no session server, and every probe is a no-op where the backend is not compiled in. */
static const agora_driver *fn_driver_named(const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	if (strcmp(name, "null") == 0) {
		return fn_agora_driver_null();
	}
	if (strcmp(name, "alsa") == 0) {
		return fn_agora_driver_alsa();
	}
	if (strcmp(name, "pipewire") == 0) {
		return fn_agora_driver_pipewire();
	}
	if (strcmp(name, "sndio") == 0) {
		return fn_agora_driver_sndio();
	}
	return NULL;
}

static const agora_driver *fn_driver_default(void)
{
	const agora_driver *pipewire = fn_agora_driver_pipewire();
	const agora_driver *sndio = fn_agora_driver_sndio();
	const agora_driver *alsa = fn_agora_driver_alsa();

	if (pipewire != NULL && pipewire->device_count() > 0) {
		return pipewire;
	}
	if (sndio != NULL && sndio->device_count() > 0) {
		return sndio;
	}
	if (alsa != NULL && alsa->device_count() > 0) {
		return alsa;
	}
	return fn_agora_driver_null();
}

/* ------------------------------------------------------------------------------------------------
 * Finding things.
 * ---------------------------------------------------------------------------------------------- */

static agora_device *fn_device(AudioObjectID id)
{
	UInt32 i;

	fn_init();
	for (i = 0; i < g_hal.device_count; i++) {
		if (g_hal.devices[i].id == id) {
			return &g_hal.devices[i];
		}
	}

	return NULL;
}

static agora_stream *fn_stream(AudioObjectID id, agora_device **out_device)
{
	UInt32 i, s;

	fn_init();
	for (i = 0; i < g_hal.device_count; i++) {
		for (s = 0; s < g_hal.devices[i].stream_count; s++) {
			if (g_hal.devices[i].streams[s].id == id) {
				if (out_device != NULL) {
					*out_device = &g_hal.devices[i];
				}
				return &g_hal.devices[i].streams[s];
			}
		}
	}

	return NULL;
}

static agora_device *fn_default_device(bool for_input)
{
	UInt32 i;

	fn_init();
	for (i = 0; i < g_hal.device_count; i++) {
		if (for_input ? g_hal.devices[i].has_input : g_hal.devices[i].has_output) {
			return &g_hal.devices[i];
		}
	}

	return NULL;
}

/* ------------------------------------------------------------------------------------------------
 * The library's own state.
 * ---------------------------------------------------------------------------------------------- */

static void fn_init(void)
{
	UInt32 count;
	UInt32 i;

	if (g_hal.initialised) {
		return;
	}
	g_hal.initialised = true;

	if (g_hal.driver == NULL) {
		const char *requested = getenv("AGORA_AUDIO_DRIVER");
		const agora_driver *named = fn_driver_named(requested);

		/* A request this build does not have falls back to the null backend rather than failing to make a
		 * sound at all; no request at all means the server, if there is one. */
		g_hal.driver = (requested != NULL && named == NULL) ? fn_agora_driver_null()
		              : (named != NULL ? named : fn_driver_default());
	}

	count = g_hal.driver->device_count();
	if (count > AGORA_MAX_DEVICES) {
		count = AGORA_MAX_DEVICES;
	}

	for (i = 0; i < count; i++) {
		agora_device_desc description;
		agora_device *device = &g_hal.devices[g_hal.device_count];
		UInt32 stream_count = 0;

		memset(&description, 0, sizeof(description));
		if (!g_hal.driver->device_describe(i, &description)) {
			continue;
		}

		memset(device, 0, sizeof(*device));
		device->id = description.id;
		snprintf(device->name, sizeof(device->name), "%s", description.name != NULL ? description.name : "Device");
		snprintf(device->uid, sizeof(device->uid), "%s", description.uid != NULL ? description.uid : "agora:null");
		snprintf(device->manufacturer, sizeof(device->manufacturer), "%s",
		         description.manufacturer != NULL ? description.manufacturer : "Agora");
		device->transport = description.transport;
		device->nominal_rate = description.nominal_rate;
		device->buffer_frames = description.buffer_frames;
		device->channels = description.channels;
		device->volume = 1.0;
		device->has_input = description.has_input;
		device->has_output = description.has_output;

		if (description.has_output) {
			agora_stream *stream = &device->streams[stream_count++];

			stream->id = (AudioStreamID)(1000 + g_hal.device_count * 16 + 0);
			stream->device = device->id;
			stream->output = true;
			stream->channels = description.channels;
			stream->terminal_type = description.terminal_type;
			stream->starting_channel = 1;
			memset(&stream->format, 0, sizeof(stream->format));
			stream->format.mSampleRate = description.nominal_rate;
			stream->format.mFormatID = kAudioFormatLinearPCM;
			stream->format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
			                            | kAudioFormatFlagIsPacked;
			stream->format.mBytesPerPacket = (UInt32)sizeof(float) * description.channels;
			stream->format.mFramesPerPacket = 1;
			stream->format.mBytesPerFrame = (UInt32)sizeof(float) * description.channels;
			stream->format.mChannelsPerFrame = description.channels;
			stream->format.mBitsPerChannel = 32;
		}

		if (description.has_input) {
			agora_stream *stream = &device->streams[stream_count++];

			stream->id = (AudioStreamID)(1000 + g_hal.device_count * 16 + 1);
			stream->device = device->id;
			stream->output = false;
			stream->channels = description.channels;
			stream->terminal_type = kAudioStreamTerminalTypeMicrophone;
			stream->starting_channel = 1;
			memset(&stream->format, 0, sizeof(stream->format));
			stream->format.mSampleRate = description.nominal_rate;
			stream->format.mFormatID = kAudioFormatLinearPCM;
			stream->format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
			                            | kAudioFormatFlagIsPacked;
			stream->format.mBytesPerPacket = (UInt32)sizeof(float) * description.channels;
			stream->format.mFramesPerPacket = 1;
			stream->format.mBytesPerFrame = (UInt32)sizeof(float) * description.channels;
			stream->format.mChannelsPerFrame = description.channels;
			stream->format.mBitsPerChannel = 32;
		}

		device->stream_count = stream_count;
		g_hal.device_count++;
	}
}

void fn_agora_audio_set_driver(const char *name)
{
	/* A suite states its backend in code rather than inheriting one from the shell, which is the difference
	 * between a measurement and a coincidence. An unavailable backend falls back to null. */
	g_hal.driver = NULL;
	g_hal.initialised = false;
	g_hal.device_count = 0;
	memset(g_hal.devices, 0, sizeof(g_hal.devices));
	memset(g_hal.listeners, 0, sizeof(g_hal.listeners));

	/* A suite names its backend; naming none asks for whatever this machine would use by itself. */
	g_hal.driver = (name != NULL) ? fn_driver_named(name) : fn_driver_default();
	if (g_hal.driver == NULL) {
		g_hal.driver = fn_agora_driver_null();
	}
}

void fn_agora_audio_test_peaks(AudioDeviceID id, float *out_peak, UInt64 *out_frames)
{
	agora_device *device = fn_device(id);

	if (out_peak != NULL) {
		*out_peak = device != NULL ? device->last_peak : 0.0f;
	}
	if (out_frames != NULL) {
		*out_frames = device != NULL ? device->frames_rendered : 0;
	}
}

/* ------------------------------------------------------------------------------------------------
 * Listeners.
 * ---------------------------------------------------------------------------------------------- */

static bool fn_address_matches(const AudioObjectPropertyAddress *left,
                               const AudioObjectPropertyAddress *right)
{
	bool selector = left->mSelector == right->mSelector;
	bool scope = left->mScope == right->mScope
	          || left->mScope == kAudioObjectPropertyScopeWildcard
	          || right->mScope == kAudioObjectPropertyScopeWildcard;
	bool element = left->mElement == right->mElement
	            || left->mElement == kAudioObjectPropertyElementWildcard
	            || right->mElement == kAudioObjectPropertyElementWildcard;

	return selector && scope && element;
}

static void fn_notify(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 i;

	for (i = 0; i < AGORA_MAX_LISTENERS; i++) {
		agora_listener *listener = &g_hal.listeners[i];

		if (!listener->active) {
			continue;
		}

		if (!fn_address_matches(&(AudioObjectPropertyAddress){ listener->selector, listener->scope,
		                                                       listener->element }, address)) {
			continue;
		}

		if (listener->proc != NULL) {
			listener->proc(object, 1, address, listener->client);
		}
		if (listener->block != NULL) {
			if (listener->queue != NULL) {
				dispatch_async(listener->queue, ^{
					listener->block(1, address);
				});
			} else {
				listener->block(1, address);
			}
		}
	}
}

/* ------------------------------------------------------------------------------------------------
 * Properties: sizes, reads and writes.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_size_for(AudioObjectID object, const AudioObjectPropertyAddress *address,
                            UInt32 *out_size);

static OSStatus fn_get_string_property(const char *text, UInt32 *io_size, void *out)
{
	CFStringRef string = CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
	UInt32 needed = (UInt32)sizeof(CFStringRef);

	if (string == NULL) {
		return kAudioHardwareUnspecifiedError;
	}

	if (io_size == NULL || out == NULL || *io_size < needed) {
		CFRelease(string);
		return kAudioHardwareBadPropertySizeError;
	}

	memcpy(out, &string, sizeof(string));
	*io_size = needed;
	return 0;
}

static OSStatus fn_system_property(const AudioObjectPropertyAddress *address, UInt32 *io_size, void *out)
{
	switch (address->mSelector) {
	case kAudioHardwarePropertyDevices: {
		UInt32 needed = g_hal.device_count * (UInt32)sizeof(AudioDeviceID);
		UInt32 i;

		if (io_size == NULL || out == NULL || *io_size < needed) {
			return kAudioHardwareBadPropertySizeError;
		}
		for (i = 0; i < g_hal.device_count; i++) {
			((AudioDeviceID *)out)[i] = g_hal.devices[i].id;
		}
		*io_size = needed;
		return 0;
	}

	case kAudioHardwarePropertyDefaultOutputDevice:
	case kAudioHardwarePropertyDefaultSystemOutputDevice:
	case kAudioHardwarePropertyDefaultInputDevice: {
		bool for_input = address->mSelector == kAudioHardwarePropertyDefaultInputDevice;
		agora_device *device = fn_default_device(for_input);
		AudioDeviceID id = device != NULL ? device->id : kAudioObjectUnknown;

		if (io_size == NULL || out == NULL || *io_size < sizeof(id)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(AudioDeviceID *)out = id;
		*io_size = sizeof(id);
		return 0;
	}

	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus fn_stream_property(const agora_stream *stream, const AudioObjectPropertyAddress *address,
                                   UInt32 *io_size, void *out)
{
	switch (address->mSelector) {
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		if (io_size == NULL || out == NULL || *io_size < sizeof(AudioStreamBasicDescription)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(AudioStreamBasicDescription *)out = stream->format;
		*io_size = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioStreamPropertyTerminalType:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = stream->terminal_type;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioStreamPropertyDirection:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		/* Apple's convention: 0 is output, 1 is input. */
		*(UInt32 *)out = stream->output ? 0U : 1U;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioStreamPropertyStartingChannel:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = stream->starting_channel;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioStreamPropertyLatency:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = 0;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioStreamPropertyIsActive:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = 1;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioStreamPropertyOwningDevice:
		if (io_size == NULL || out == NULL || *io_size < sizeof(AudioDeviceID)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(AudioDeviceID *)out = stream->device;
		*io_size = sizeof(AudioDeviceID);
		return 0;

	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus fn_device_property(agora_device *device, const AudioObjectPropertyAddress *address,
                                   UInt32 *io_size, void *out)
{
	switch (address->mSelector) {
	case kAudioDevicePropertyDeviceName:
	case kAudioDevicePropertyDeviceNameCFString:
		/* Apple has both: a `CFString`-carrying selector and one that returns the old C string. Both here
		 * would be two spellings of one answer, so the CFString one is answered and the other names it. */
		if (address->mSelector == kAudioDevicePropertyDeviceName) {
			return kAudioHardwareUnknownPropertyError;
		}
		return fn_get_string_property(device->name, io_size, out);

	case kAudioDevicePropertyDeviceManufacturer:
	case kAudioDevicePropertyDeviceManufacturerCFString:
		if (address->mSelector == kAudioDevicePropertyDeviceManufacturer) {
			return kAudioHardwareUnknownPropertyError;
		}
		return fn_get_string_property(device->manufacturer, io_size, out);

	case kAudioDevicePropertyDeviceUID:
		return fn_get_string_property(device->uid, io_size, out);

	case kAudioDevicePropertyTransportType:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = device->transport;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: {
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = (address->mSelector == kAudioDevicePropertyDeviceIsAlive)
		               ? 1U : (device->has_output ? 1U : 0U);
		*io_size = sizeof(UInt32);
		return 0;
	}

	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioDevicePropertyDeviceIsRunningSomewhere: {
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = device->running ? 1U : 0U;
		*io_size = sizeof(UInt32);
		return 0;
	}

	case kAudioDevicePropertyStreams: {
		UInt32 i;
		UInt32 count = 0;

		/* THE SCOPE FILTERS THE ANSWER, and it is the question a caller asks most: "the output streams" is
		 * how a player finds out how many channels it has. */
		for (i = 0; i < device->stream_count; i++) {
			if (address->mScope == kAudioObjectPropertyScopeGlobal
			    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
			    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
				count++;
			}
		}

		if (io_size == NULL || out == NULL || *io_size < count * sizeof(AudioStreamID)) {
			return kAudioHardwareBadPropertySizeError;
		}

		count = 0;
		for (i = 0; i < device->stream_count; i++) {
			if (address->mScope == kAudioObjectPropertyScopeGlobal
			    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
			    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
				((AudioStreamID *)out)[count++] = device->streams[i].id;
			}
		}

		*io_size = count * (UInt32)sizeof(AudioStreamID);
		return 0;
	}

	case kAudioDevicePropertyStreamConfiguration: {
		/* An `AudioBufferList` naming the channel count of each stream in the scope — the answer to "how
		 * many buffers do I allocate", and the reason a caller needs no format query first. */
		UInt32 i;
		UInt32 count = 0;
		UInt32 needed;
		AudioBufferList *list = out;

		for (i = 0; i < device->stream_count; i++) {
			if (address->mScope == kAudioObjectPropertyScopeGlobal
			    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
			    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
				count++;
			}
		}

		needed = (UInt32)(offsetof(AudioBufferList, mBuffers) + count * sizeof(AudioBuffer));
		if (io_size == NULL || out == NULL || *io_size < needed) {
			return kAudioHardwareBadPropertySizeError;
		}

		memset(list, 0, needed);
		list->mNumberBuffers = count;
		count = 0;
		for (i = 0; i < device->stream_count; i++) {
			if (address->mScope == kAudioObjectPropertyScopeGlobal
			    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
			    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
				list->mBuffers[count].mNumberChannels = device->streams[i].channels;
				list->mBuffers[count].mDataByteSize = 0;
				list->mBuffers[count].mData = NULL;
				count++;
			}
		}
		*io_size = needed;
		return 0;
	}

	case kAudioDevicePropertyNominalSampleRate:
	case kAudioDevicePropertyActualSampleRate:
		if (io_size == NULL || out == NULL || *io_size < sizeof(Float64)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float64 *)out = device->nominal_rate;
		*io_size = sizeof(Float64);
		return 0;

	case kAudioDevicePropertyAvailableNominalSampleRates: {
		AudioValueRange *ranges = out;

		if (io_size == NULL || out == NULL || *io_size < 2 * sizeof(AudioValueRange)) {
			return kAudioHardwareBadPropertySizeError;
		}
		ranges[0].mMinimum = 44100.0;
		ranges[0].mMaximum = 44100.0;
		ranges[1].mMinimum = 48000.0;
		ranges[1].mMaximum = 48000.0;
		*io_size = 2 * (UInt32)sizeof(AudioValueRange);
		return 0;
	}

	case kAudioDevicePropertyBufferFrameSize:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = device->buffer_frames;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioDevicePropertyBufferFrameSizeRange: {
		AudioValueRange *range = out;

		if (io_size == NULL || out == NULL || *io_size < sizeof(AudioValueRange)) {
			return kAudioHardwareBadPropertySizeError;
		}
		range->mMinimum = 32.0;
		range->mMaximum = 4096.0;
		*io_size = sizeof(AudioValueRange);
		return 0;
	}

	case kAudioDevicePropertyVolumeScalar: {
		/* The element is the channel; master means "all of them", which for one fader is the same value. */
		if (io_size == NULL || out == NULL || *io_size < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)out = (Float32)device->volume;
		*io_size = sizeof(Float32);
		return 0;
	}

	case kAudioDevicePropertyVolumeDecibels: {
		if (io_size == NULL || out == NULL || *io_size < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)out = (device->volume > 0.0)
		                ? (Float32)(20.0 * log10(device->volume)) : (Float32)-96.0;
		*io_size = sizeof(Float32);
		return 0;
	}

	case kAudioDevicePropertyMute:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = device->mute ? 1U : 0U;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyLatency:
		if (io_size == NULL || out == NULL || *io_size < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(UInt32 *)out = 0;
		*io_size = sizeof(UInt32);
		return 0;

	case kAudioDevicePropertyHogMode: {
		pid_t pid = -1;

		if (io_size == NULL || out == NULL || *io_size < sizeof(pid_t)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(pid_t *)out = pid;
		*io_size = sizeof(pid_t);
		return 0;
	}

	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus fn_size_for(AudioObjectID object, const AudioObjectPropertyAddress *address,
                            UInt32 *out_size)
{
	agora_device *device = NULL;
	agora_stream *stream;

	if (out_size == NULL || address == NULL) {
		return kAudio_ParamError;
	}

	if (object == kAudioObjectSystemObject) {
		switch (address->mSelector) {
		case kAudioHardwarePropertyDevices: *out_size = g_hal.device_count * sizeof(AudioDeviceID); return 0;
		case kAudioHardwarePropertyDefaultOutputDevice:
		case kAudioHardwarePropertyDefaultSystemOutputDevice:
		case kAudioHardwarePropertyDefaultInputDevice: *out_size = sizeof(AudioDeviceID); return 0;
		default: return kAudioHardwareUnknownPropertyError;
		}
	}

	stream = fn_stream(object, &device);
	if (stream != NULL) {
		switch (address->mSelector) {
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			*out_size = sizeof(AudioStreamBasicDescription); return 0;
		case kAudioStreamPropertyOwningDevice: *out_size = sizeof(AudioDeviceID); return 0;
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyIsActive: *out_size = sizeof(UInt32); return 0;
		default: return kAudioHardwareUnknownPropertyError;
		}
	}

	device = fn_device(object);
	if (device != NULL) {
		switch (address->mSelector) {
		case kAudioDevicePropertyDeviceNameCFString:
		case kAudioDevicePropertyDeviceManufacturerCFString:
		case kAudioDevicePropertyDeviceUID: *out_size = sizeof(CFStringRef); return 0;
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyActualSampleRate: *out_size = sizeof(Float64); return 0;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			*out_size = 2 * (UInt32)sizeof(AudioValueRange); return 0;
		case kAudioDevicePropertyBufferFrameSizeRange: *out_size = sizeof(AudioValueRange); return 0;
		case kAudioDevicePropertyStreamConfiguration: {
			UInt32 i, count = 0;

			for (i = 0; i < device->stream_count; i++) {
				if (address->mScope == kAudioObjectPropertyScopeGlobal
				    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
				    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
					count++;
				}
			}
			*out_size = (UInt32)(offsetof(AudioBufferList, mBuffers) + count * sizeof(AudioBuffer));
			return 0;
		}
		case kAudioDevicePropertyStreams: {
			UInt32 i, count = 0;

			for (i = 0; i < device->stream_count; i++) {
				if (address->mScope == kAudioObjectPropertyScopeGlobal
				    || (address->mScope == kAudioObjectPropertyScopeOutput && device->streams[i].output)
				    || (address->mScope == kAudioObjectPropertyScopeInput && !device->streams[i].output)) {
					count++;
				}
			}
			*out_size = count * (UInt32)sizeof(AudioStreamID);
			return 0;
		}
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceIsRunningSomewhere:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyBufferFrameSize:
		case kAudioDevicePropertyMute:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyLatency: *out_size = sizeof(UInt32); return 0;
		case kAudioDevicePropertyVolumeScalar:
		case kAudioDevicePropertyVolumeDecibels: *out_size = sizeof(Float32); return 0;
		case kAudioDevicePropertyHogMode: *out_size = sizeof(pid_t); return 0;
		default: return kAudioHardwareUnknownPropertyError;
		}
	}

	return kAudioHardwareBadObjectError;
}

OSStatus AudioObjectGetPropertyDataSize(AudioObjectID inObjectID,
                                        const AudioObjectPropertyAddress *inAddress,
                                        UInt32 inQualifierDataSize, const void *inQualifierData,
                                        UInt32 *outDataSize)
{
	(void)inQualifierDataSize;
	(void)inQualifierData;

	fn_init();
	return fn_size_for(inObjectID, inAddress, outDataSize);
}

OSStatus AudioObjectGetPropertyData(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize, const void *inQualifierData,
                                    UInt32 *ioDataSize, void *outData)
{
	agora_stream *stream;
	agora_device *device;

	(void)inQualifierDataSize;
	(void)inQualifierData;

	fn_init();

	if (inAddress == NULL || ioDataSize == NULL) {
		return kAudio_ParamError;
	}

	if (inObjectID == kAudioObjectSystemObject) {
		return fn_system_property(inAddress, ioDataSize, outData);
	}

	stream = fn_stream(inObjectID, &device);
	if (stream != NULL) {
		return fn_stream_property(stream, inAddress, ioDataSize, outData);
	}

	device = fn_device(inObjectID);
	if (device != NULL) {
		return fn_device_property(device, inAddress, ioDataSize, outData);
	}

	return kAudioHardwareBadObjectError;
}

OSStatus AudioObjectSetPropertyData(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize, const void *inQualifierData,
                                    UInt32 inDataSize, const void *inData)
{
	agora_device *device;
	OSStatus status = 0;

	(void)inQualifierDataSize;
	(void)inQualifierData;

	fn_init();

	if (inAddress == NULL || inData == NULL) {
		return kAudio_ParamError;
	}

	device = fn_device(inObjectID);
	if (device == NULL) {
		return kAudioHardwareBadObjectError;
	}

	switch (inAddress->mSelector) {
	case kAudioDevicePropertyVolumeScalar: {
		Float32 volume;

		if (inDataSize < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		volume = *(const Float32 *)inData;
		if (volume < 0.0f) { volume = 0.0f; }
		if (volume > 1.0f) { volume = 1.0f; }
		device->volume = (Float64)volume;
		break;
	}

	case kAudioDevicePropertyMute:
		if (inDataSize < sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		device->mute = (*(const UInt32 *)inData) != 0;
		break;

	case kAudioDevicePropertyBufferFrameSize:
		/* NOT A RECONFIGURATION: this backend's quantum is the driver's, and a caller that changes it would
		 * be asking us to take the device away from whoever else has it. Apple allows it; a client that
		 * needs it gets the honest refusal. */
		return kAudioHardwareUnsupportedOperationError;

	default:
		status = kAudioHardwareUnknownPropertyError;
		break;
	}

	if (status == 0) {
		fn_notify(inObjectID, inAddress);
	}

	return status;
}

Boolean AudioObjectHasProperty(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress)
{
	UInt32 size = 0;

	if (inAddress == NULL) {
		return false;
	}

	return fn_size_for(inObjectID, inAddress, &size) == 0 ? true : false;
}

OSStatus AudioObjectIsPropertySettable(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress,
                                       Boolean *outIsSettable)
{
	if (inAddress == NULL || outIsSettable == NULL) {
		return kAudio_ParamError;
	}

	*outIsSettable = false;

	if (fn_size_for(inObjectID, inAddress, &(UInt32){ 0 }) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}

	switch (inAddress->mSelector) {
	case kAudioDevicePropertyVolumeScalar:
	case kAudioDevicePropertyMute:
		*outIsSettable = true;
		break;
	default:
		break;
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Listener registration.
 * ---------------------------------------------------------------------------------------------- */

static agora_listener *fn_free_listener(void)
{
	UInt32 i;

	for (i = 0; i < AGORA_MAX_LISTENERS; i++) {
		if (!g_hal.listeners[i].active) {
			return &g_hal.listeners[i];
		}
	}

	return NULL;
}

OSStatus AudioObjectAddPropertyListener(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress,
                                        AudioObjectPropertyListenerProc inListener, void *inClientData)
{
	agora_listener *listener;

	fn_init();

	if (inAddress == NULL || inListener == NULL) {
		return kAudio_ParamError;
	}

	listener = fn_free_listener();
	if (listener == NULL) {
		return kAudioHardwareUnspecifiedError;
	}

	memset(listener, 0, sizeof(*listener));
	listener->active = true;
	listener->selector = inAddress->mSelector;
	listener->scope = inAddress->mScope;
	listener->element = inAddress->mElement;
	listener->proc = inListener;
	listener->client = inClientData;
	(void)inObjectID;

	return 0;
}

OSStatus AudioObjectRemovePropertyListener(AudioObjectID inObjectID,
                                           const AudioObjectPropertyAddress *inAddress,
                                           AudioObjectPropertyListenerProc inListener,
                                           void *inClientData)
{
	UInt32 i;

	(void)inObjectID;

	for (i = 0; i < AGORA_MAX_LISTENERS; i++) {
		agora_listener *listener = &g_hal.listeners[i];

		AudioObjectPropertyAddress registered = { listener->selector, listener->scope, listener->element };

		if (listener->active && listener->proc == inListener && listener->client == inClientData
		    && inAddress != NULL && fn_address_matches(&registered, inAddress)) {
			listener->active = false;
			return 0;
		}
	}

	return kAudioHardwareBadObjectError;
}

OSStatus AudioObjectAddPropertyListenerBlock(AudioObjectID inObjectID,
                                             const AudioObjectPropertyAddress *inAddress,
                                             dispatch_queue_t inDispatchQueue,
                                             AudioObjectPropertyListenerBlock inListener)
{
	agora_listener *listener;

	fn_init();

	if (inAddress == NULL || inListener == NULL) {
		return kAudio_ParamError;
	}

	listener = fn_free_listener();
	if (listener == NULL) {
		return kAudioHardwareUnspecifiedError;
	}

	memset(listener, 0, sizeof(*listener));
	listener->active = true;
	listener->selector = inAddress->mSelector;
	listener->scope = inAddress->mScope;
	listener->element = inAddress->mElement;
	listener->block = inListener;
	listener->queue = inDispatchQueue;
	(void)inObjectID;

	return 0;
}

OSStatus AudioObjectRemovePropertyListenerBlock(AudioObjectID inObjectID,
                                                const AudioObjectPropertyAddress *inAddress,
                                                dispatch_queue_t inDispatchQueue,
                                                AudioObjectPropertyListenerBlock inListener)
{
	UInt32 i;

	(void)inObjectID;

	for (i = 0; i < AGORA_MAX_LISTENERS; i++) {
		agora_listener *listener = &g_hal.listeners[i];

		AudioObjectPropertyAddress registered = { listener->selector, listener->scope, listener->element };

		if (listener->active && listener->block == inListener && listener->queue == inDispatchQueue
		    && inAddress != NULL && fn_address_matches(&registered, inAddress)) {
			listener->active = false;
			return 0;
		}
	}

	return kAudioHardwareBadObjectError;
}

/* ------------------------------------------------------------------------------------------------
 * Io procs, starting and stopping, the clock.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioDeviceCreateIOProcID(AudioObjectID inDevice, AudioDeviceIOProc inProc, void *inClientData,
                                   AudioDeviceIOProcID *outIOProcID)
{
	agora_device *device = fn_device(inDevice);
	UInt32 i;

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}
	if (inProc == NULL || outIOProcID == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < AGORA_MAX_IOPROCS; i++) {
		if (!device->procs[i].active) {
			memset(&device->procs[i], 0, sizeof(device->procs[i]));
			device->procs[i].active = true;
			device->procs[i].proc = inProc;
			device->procs[i].client = inClientData;
			*outIOProcID = inProc;
			return 0;
		}
	}

	return kAudioHardwareUnspecifiedError;
}

OSStatus AudioDeviceCreateIOProcIDWithBlock(AudioDeviceIOProcID *outIOProcID, AudioObjectID inDevice,
                                            dispatch_queue_t inDispatchQueue, AudioDeviceIOBlock inIOBlock)
{
	agora_device *device = fn_device(inDevice);
	UInt32 i;

	(void)inDispatchQueue;

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}
	if (outIOProcID == NULL || inIOBlock == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < AGORA_MAX_IOPROCS; i++) {
		if (!device->procs[i].active) {
			memset(&device->procs[i], 0, sizeof(device->procs[i]));
			device->procs[i].active = true;
			device->procs[i].block = inIOBlock;
			/* The id is the block's address: unique per registration and comparable, which is all an id
			 * has to be. */
			*outIOProcID = (AudioDeviceIOProcID)(void *)inIOBlock;
			return 0;
		}
	}

	return kAudioHardwareUnspecifiedError;
}

OSStatus AudioDeviceDestroyIOProcID(AudioObjectID inDevice, AudioDeviceIOProcID inIOProcID)
{
	agora_device *device = fn_device(inDevice);
	UInt32 i;

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}

	for (i = 0; i < AGORA_MAX_IOPROCS; i++) {
		if (device->procs[i].active
		    && (device->procs[i].proc == inIOProcID
		        || (void *)device->procs[i].block == (void *)inIOProcID)) {
			memset(&device->procs[i], 0, sizeof(device->procs[i]));
			return 0;
		}
	}

	return kAudioHardwareBadDeviceError;
}

OSStatus AudioDeviceStart(AudioObjectID inDevice, AudioDeviceIOProcID inProcID)
{
	agora_device *device = fn_device(inDevice);
	AudioObjectPropertyAddress running = { kAudioDevicePropertyDeviceIsRunning,
	                                       kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMaster };
	(void)inProcID;

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}

	if (!device->running) {
		if (g_hal.driver == NULL || !g_hal.driver->start(device->id)) {
			return kAudioHardwareUnspecifiedError;
		}
		device->running = true;
		fn_notify(device->id, &running);
	}

	return 0;
}

OSStatus AudioDeviceStop(AudioObjectID inDevice, AudioDeviceIOProcID inProcID)
{
	agora_device *device = fn_device(inDevice);
	AudioObjectPropertyAddress running = { kAudioDevicePropertyDeviceIsRunning,
	                                       kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMaster };
	(void)inProcID;

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}

	if (device->running) {
		if (g_hal.driver != NULL) {
			g_hal.driver->stop(device->id);
		}
		device->running = false;
		/* A STOPPED DEVICE PRODUCES SILENCE, and says so: the mixer's last output is stale the moment the
		 * data thread stops, and a caller (or a suite) that asks "what came out" must not be handed the
		 * value from before the stop. */
		device->last_peak = 0.0f;
		fn_notify(device->id, &running);
	}

	return 0;
}

OSStatus AudioDeviceGetCurrentTime(AudioObjectID inDevice, AudioTimeStamp *outTime)
{
	agora_device *device = fn_device(inDevice);

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}
	if (outTime == NULL) {
		return kAudio_ParamError;
	}

	memset(outTime, 0, sizeof(*outTime));
	outTime->mSampleTime = device->last_sample_time;
	outTime->mHostTime = device->last_host_time;
	outTime->mRateScalar = 1.0;
	outTime->mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;

	return 0;
}

OSStatus AudioDeviceTranslateTime(AudioObjectID inDevice, const AudioTimeStamp *inTime,
                                  AudioTimeStamp *outTime)
{
	agora_device *device = fn_device(inDevice);

	if (device == NULL) {
		return kAudioHardwareBadDeviceError;
	}
	if (inTime == NULL || outTime == NULL) {
		return kAudio_ParamError;
	}

	*outTime = *inTime;

	if ((inTime->mFlags & kAudioTimeStampSampleTimeValid) && !(inTime->mFlags & kAudioTimeStampHostTimeValid)) {
		/* Sample time to host time, through the rate the device is actually running at. */
		outTime->mHostTime = device->last_host_time
		                   + (UInt64)((inTime->mSampleTime - device->last_sample_time)
		                              / device->nominal_rate * AudioGetHostClockFrequency());
		outTime->mFlags |= kAudioTimeStampHostTimeValid;
	} else if ((inTime->mFlags & kAudioTimeStampHostTimeValid)
	           && !(inTime->mFlags & kAudioTimeStampSampleTimeValid)) {
		outTime->mSampleTime = device->last_sample_time
		                     + (Float64)(inTime->mHostTime - device->last_host_time)
		                       / AudioGetHostClockFrequency() * device->nominal_rate;
		outTime->mFlags |= kAudioTimeStampSampleTimeValid;
	}

	return 0;
}

OSStatus AudioDeviceGetNearestStartTime(AudioObjectID inDevice, AudioTimeStamp *ioRequestedStartTime,
                                        UInt32 inFlags)
{
	(void)inFlags;

	if (fn_device(inDevice) == NULL) {
		return kAudioHardwareBadDeviceError;
	}
	if (ioRequestedStartTime == NULL) {
		return kAudio_ParamError;
	}

	/* No scheduling: this backend starts when it is told to, and the nearest start time is "now". */
	ioRequestedStartTime->mFlags |= kAudioTimeStampHostTimeValid;
	ioRequestedStartTime->mHostTime = AudioGetCurrentHostTime();
	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * The mixer: what a driver's data thread calls.
 * ---------------------------------------------------------------------------------------------- */

void fn_agora_device_render(AudioDeviceID id, float *mix, UInt32 frames)
{
	agora_device *device = fn_device(id);
	UInt32 channels;
	UInt32 total;
	float peak = 0.0f;
	UInt32 i;
	UInt32 frame;

	if (device == NULL || mix == NULL || frames == 0 || device->channels == 0) {
		return;
	}

	channels = device->channels;
	total = frames * channels;
	memset(mix, 0, (size_t)total * sizeof(float));

	for (i = 0; i < AGORA_MAX_IOPROCS; i++) {
		agora_ioproc *proc = &device->procs[i];
		AudioBufferList list;
		float *scratch;
		AudioTimeStamp now = { 0 }, out_time = { 0 };
		OSStatus status;

		if (!proc->active) {
			continue;
		}

		/* EACH PROC GETS ITS OWN ZEROED BUFFER, which is the HAL's contract on macOS: a callback that does
		 * not write every frame must not inherit another client's audio. The sum happens afterwards. */
		scratch = calloc((size_t)total, sizeof(float));
		if (scratch == NULL) {
			continue;
		}

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = channels;
		list.mBuffers[0].mDataByteSize = (UInt32)((size_t)total * sizeof(float));
		list.mBuffers[0].mData = scratch;

		now.mSampleTime = device->last_sample_time;
		now.mHostTime = AudioGetCurrentHostTime();
		now.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
		out_time = now;

		if (proc->proc != NULL) {
			/* An output-only device has no input data, and Apple's own io proc signature marks the two
			 * buffer-list parameters nullable for exactly that reason; a variable keeps the annotation from
			 * complaining about the literal. */
			const AudioBufferList *no_input = NULL;
			/* `inInputTime` is NOT nullable in Apple's signature even when there is no input data, so the
			 * "when the input would have been" is the same instant as the output's. */
			AudioTimeStamp input_time = now;

			status = proc->proc(id, &now, no_input, &input_time, &list, &out_time, proc->client);
		} else if (proc->block != NULL) {
			proc->block(&now, NULL, NULL, &list, &out_time);
			status = 0;
		} else {
			status = kAudioHardwareUnspecifiedError;
		}

		if (status == 0) {
			for (frame = 0; frame < total; frame++) {
				float value = mix[frame] + scratch[frame];

				/* Saturate at the rails, as a mixer does: the sum of two full-scale signals is a limit,
				 * not a wrap. */
				if (value > 1.0f) { value = 1.0f; }
				if (value < -1.0f) { value = -1.0f; }
				mix[frame] = value;
			}
		}

		free(scratch);
		proc->calls++;
	}

	if (device->mute) {
		memset(mix, 0, (size_t)total * sizeof(float));
	} else {
		for (frame = 0; frame < total; frame++) {
			mix[frame] = (float)(mix[frame] * device->volume);
		}
	}

	for (frame = 0; frame < total; frame++) {
		float magnitude = mix[frame] < 0.0f ? -mix[frame] : mix[frame];

		if (magnitude > peak) {
			peak = magnitude;
		}
	}

	device->last_peak = peak;
	device->frames_rendered += frames;
	device->last_sample_time += frames;
	device->last_host_time = AudioGetCurrentHostTime();
}
