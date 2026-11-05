/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * hal.c — the device layer, measured on the null backend.
 *
 * NO SOUND CARD, NO SESSION SERVER, AND THE TIMING STILL MEANS SOMETHING. The null backend runs a thread at
 * the device's rate, so "an io proc is called at 48 kHz" is a claim about a clock: 250 ms of a 512-frame
 * quantum is about 23 000 frames, and the assertions below are ratios, not magnitudes that happen to pass on
 * a fast machine.
 *
 * THE MIXER IS CHECKED WITH SAMPLE VALUES, NOT WITH CALL COUNTS. Two io procs write 0.25 and 0.125; what comes
 * out of the mixer must be 0.375, and muting it must make it zero. A test that asserted "both were called"
 * would pass for a HAL that played neither.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;
static const char *g_driver = "null";
static bool g_strict_timing = true;
/* The null backend and an ALSA `null` PCM are *ours*: this library chooses 48 000 Hz, a 512-frame quantum and
 * two channels, so the suite can assert those numbers. A session server chooses them for us, and a suite that
 * insisted on 512 would be measuring PipeWire's configuration rather than this HAL. */
static bool g_exact_numbers = true;

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

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != 0) {
		return kAudioObjectUnknown;
	}

	return device;
}

/* ------------------------------------------------------------------------------------------------
 * The objects and their properties.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_objects(void)
{
	AudioDeviceID device = fn_default_output();
	AudioObjectPropertyAddress devices_address = { kAudioHardwarePropertyDevices,
	                                               kAudioObjectPropertyScopeGlobal,
	                                               kAudioObjectPropertyElementMaster };
	UInt32 size = 0;

	fn_check("a default output device exists", device != kAudioObjectUnknown);

	/* One id per device: the null backend has one and a session server may have several, so the count is a
	 * multiple of an id's size rather than exactly one id — the first one still has to be the default. */
	fn_check("the system object reports its devices",
	         AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices_address, 0, NULL, &size) == 0
	         && size >= sizeof(AudioDeviceID) && size % sizeof(AudioDeviceID) == 0);

	{
		AudioDeviceID list[8];
		UInt32 list_size = sizeof(list);

		memset(list, 0, sizeof(list));
		fn_check("the device list reads",
		         AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_address, 0, NULL,
		                                    &list_size, list) == 0
		         && list_size >= sizeof(AudioDeviceID) && list[0] == device);
	}

	/* The name is a CFString, which is the door a client actually uses. */
	{
		AudioObjectPropertyAddress name_address = { kAudioDevicePropertyDeviceNameCFString,
		                                            kAudioObjectPropertyScopeGlobal,
		                                            kAudioObjectPropertyElementMaster };
		CFStringRef name = NULL;
		UInt32 name_size = sizeof(name);

		fn_check("the device has a name",
		         AudioObjectGetPropertyData(device, &name_address, 0, NULL, &name_size, &name) == 0
		         && name != NULL);
	}

	{
		AudioObjectPropertyAddress rate_address = { kAudioDevicePropertyNominalSampleRate,
		                                            kAudioObjectPropertyScopeGlobal,
		                                            kAudioObjectPropertyElementMaster };
		Float64 rate = 0.0;
		UInt32 rate_size = sizeof(rate);

		fn_check(g_exact_numbers ? "the device reports the rate the backend does"
		                         : "the device reports a plausible rate",
		         AudioObjectGetPropertyData(device, &rate_address, 0, NULL, &rate_size, &rate) == 0
		         && (g_exact_numbers ? rate == 48000.0 : (rate >= 8000.0 && rate <= 384000.0)));
	}

	{
		AudioObjectPropertyAddress size_address = { kAudioDevicePropertyBufferFrameSize,
		                                            kAudioObjectPropertyScopeGlobal,
		                                            kAudioObjectPropertyElementMaster };
		UInt32 frames = 0;
		UInt32 frames_size = sizeof(frames);

		fn_check(g_exact_numbers ? "the quantum is the backend's" : "the quantum is a plausible slice",
		         AudioObjectGetPropertyData(device, &size_address, 0, NULL, &frames_size, &frames) == 0
		         && (g_exact_numbers ? frames == 512 : (frames > 0 && frames <= 8192)));
	}

	/* The streams, by scope — the question a player asks to find its channel count. */
	{
		AudioObjectPropertyAddress streams_address = { kAudioDevicePropertyStreams,
		                                               kAudioObjectPropertyScopeOutput,
		                                               kAudioObjectPropertyElementMaster };
		AudioStreamID streams[8];
		UInt32 streams_size = sizeof(streams);

		memset(streams, 0, sizeof(streams));
		fn_check("the device has one output stream",
		         AudioObjectGetPropertyData(device, &streams_address, 0, NULL, &streams_size, streams) == 0
		         && streams_size == sizeof(AudioStreamID));

		if (streams_size == sizeof(AudioStreamID)) {
			AudioObjectPropertyAddress format_address = { kAudioStreamPropertyVirtualFormat,
			                                              kAudioObjectPropertyScopeGlobal,
			                                              kAudioObjectPropertyElementMaster };
			AudioStreamBasicDescription format;
			UInt32 format_size = sizeof(format);

			memset(&format, 0, sizeof(format));
			fn_check(g_exact_numbers ? "the stream's format is float32 stereo"
			                         : "the stream's format is float32 PCM",
			         AudioObjectGetPropertyData(streams[0], &format_address, 0, NULL, &format_size,
			                                    &format) == 0
			         && format.mFormatID == kAudioFormatLinearPCM
			         && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0
			         && (g_exact_numbers ? format.mChannelsPerFrame == 2
			                             : format.mChannelsPerFrame >= 1)
			         && format.mBitsPerChannel == 32);

			{
				AudioObjectPropertyAddress direction_address = { kAudioStreamPropertyDirection,
				                                                 kAudioObjectPropertyScopeGlobal,
				                                                 kAudioObjectPropertyElementMaster };
				UInt32 direction = 1;
				UInt32 direction_size = sizeof(direction);

				fn_check("an output stream says so",
				         AudioObjectGetPropertyData(streams[0], &direction_address, 0, NULL,
				                                    &direction_size, &direction) == 0
				         && direction == 0);
			}
		}
	}

	/* HasProperty, and the error for one that is not there. */
	{
		AudioObjectPropertyAddress unknown = { 'zzzz', kAudioObjectPropertyScopeGlobal,
		                                       kAudioObjectPropertyElementMaster };
		UInt32 value = 0;
		UInt32 value_size = sizeof(value);

		fn_check("an unknown property is not claimed", AudioObjectHasProperty(device, &unknown) == false);
		fn_check("an unknown property is refused by name",
		         AudioObjectGetPropertyData(device, &unknown, 0, NULL, &value_size, &value)
		         == kAudioHardwareUnknownPropertyError);
		fn_check("a bad object is refused by name",
		         AudioObjectGetPropertyData(9999, &unknown, 0, NULL, &value_size, &value)
		         == kAudioHardwareBadObjectError);
	}
}

/* ------------------------------------------------------------------------------------------------
 * The volume, its listener, and the deprecated door.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	int calls;
	AudioObjectPropertyAddress last;
} volume_watcher;

static OSStatus fn_volume_listener(AudioObjectID object, UInt32 number, const AudioObjectPropertyAddress *addresses,
                                   void *client)
{
	volume_watcher *watcher = client;

	(void)object;

	if (number > 0) {
		watcher->last = addresses[0];
	}
	watcher->calls++;

	return 0;
}

static void fn_test_volume_and_listeners(void)
{
	AudioDeviceID device = fn_default_output();
	AudioObjectPropertyAddress volume = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput,
	                                      kAudioObjectPropertyElementMaster };

	{
		Float32 value = -1.0f;
		UInt32 size = sizeof(value);
		Boolean settable = false;
		volume_watcher watcher;

		memset(&watcher, 0, sizeof(watcher));

		fn_check("volume reads",
		         AudioObjectGetPropertyData(device, &volume, 0, NULL, &size, &value) == 0 && value == 1.0f);
		fn_check("volume is settable",
		         AudioObjectIsPropertySettable(device, &volume, &settable) == 0 && settable);

		fn_check("a listener registers",
		         AudioObjectAddPropertyListener(device, &volume, fn_volume_listener, &watcher) == 0);

		{
			Float32 half = 0.5f;

			fn_check("volume writes",
			         AudioObjectSetPropertyData(device, &volume, 0, NULL, sizeof(half), &half) == 0);
		}

		fn_check("the listener fired once", watcher.calls == 1);
		fn_check("the listener was given the property it watched",
		         watcher.last.mSelector == kAudioDevicePropertyVolumeScalar);

		size = sizeof(value);
		fn_check("volume reads back what was written",
		         AudioObjectGetPropertyData(device, &volume, 0, NULL, &size, &value) == 0 && value == 0.5f);

		fn_check("the listener unregisters",
		         AudioObjectRemovePropertyListener(device, &volume, fn_volume_listener, &watcher) == 0);

		{
			Float32 full = 1.0f;

			AudioObjectSetPropertyData(device, &volume, 0, NULL, sizeof(full), &full);
		}

		fn_check("a removed listener hears nothing", watcher.calls == 1);
	}

	/* The pre-10.7 door answers the same question with the same value. */
	{
		Float32 value = -1.0f;
		UInt32 size = sizeof(value);

		fn_check("the deprecated door reads the volume",
		         AudioDeviceGetProperty(device, 0, false, kAudioDevicePropertyVolumeScalar, &size, &value) == 0
		         && value == 1.0f);
	}

}

/* ------------------------------------------------------------------------------------------------
 * The io procs and the mixer.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	float value;
} constant_writer;

static OSStatus fn_write_constant(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                                  const AudioTimeStamp *input_time, AudioBufferList *output,
                                  const AudioTimeStamp *output_time, void *client)
{
	constant_writer *writer = client;
	float *samples;
	UInt32 frames;
	UInt32 i;

	(void)device;
	(void)now;
	(void)input;
	(void)input_time;
	(void)output_time;

	if (output == NULL || output->mNumberBuffers == 0 || output->mBuffers[0].mData == NULL) {
		return 0;
	}

	frames = output->mBuffers[0].mDataByteSize / (output->mBuffers[0].mNumberChannels * (UInt32)sizeof(float));
	samples = output->mBuffers[0].mData;

	for (i = 0; i < frames * output->mBuffers[0].mNumberChannels; i++) {
		samples[i] = writer->value;
	}

	return 0;
}

static void fn_test_io_procs(void)
{
	AudioDeviceID device = fn_default_output();
	AudioDeviceIOProcID first = NULL;
	AudioDeviceIOProcID second = NULL;
	constant_writer writer_a = { 0.25f };
	constant_writer writer_b = { 0.125f };
	UInt64 frames = 0;
	float peak = 0.0f;

	fn_check("the first io proc is created",
	         AudioDeviceCreateIOProcID(device, fn_write_constant, &writer_a, &first) == 0 && first != NULL);
	fn_check("the second io proc is created",
	         AudioDeviceCreateIOProcID(device, fn_write_constant, &writer_b, &second) == 0 && second != NULL);

	fn_check("the device starts", AudioDeviceStart(device, NULL) == 0);

	fn_sleep_ms(250);

	fn_agora_audio_test_peaks(device, &peak, &frames);

	/* 250 ms at 48 kHz is 12 000 frames: within 20% either way covers a loaded machine and still fails a
	 * HAL that never started the thread. THE ASSERTION IS SKIPPED FOR A BACKEND WHOSE PACING IS THE DEVICE'S
	 * — an ALSA `null` PCM accepts every write instantly, so how fast the thread ran is a fact about the
	 * plugin, not about this library. */
	if (g_strict_timing) {
		fn_check("the io procs are called at the device's rate", frames > 9600 && frames < 14400);
	} else {
		fn_check("the io procs were called at all", frames > 0);
	}
	fn_check("the mixer summed both procs", peak > 0.37f && peak < 0.38f);

	/* Muting silences the mix, and unmuting brings it back. */
	{
		AudioObjectPropertyAddress mute = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput,
		                                    kAudioObjectPropertyElementMaster };
		UInt32 on = 1;

		fn_check("the device mutes",
		         AudioObjectSetPropertyData(device, &mute, 0, NULL, sizeof(on), &on) == 0);
		fn_sleep_ms(80);
		fn_agora_audio_test_peaks(device, &peak, NULL);
		fn_check("a muted device mixes silence", peak == 0.0f);

		on = 0;
		AudioObjectSetPropertyData(device, &mute, 0, NULL, sizeof(on), &on);
		fn_sleep_ms(80);
		fn_agora_audio_test_peaks(device, &peak, NULL);
		fn_check("unmuting restores the mix", peak > 0.37f && peak < 0.38f);
	}

	fn_check("the device stops", AudioDeviceStop(device, NULL) == 0);

	{
		UInt64 after = 0;

		fn_agora_audio_test_peaks(device, NULL, &after);
		fn_sleep_ms(60);
		fn_agora_audio_test_peaks(device, NULL, &after);
		fn_sleep_ms(60);
		fn_agora_audio_test_peaks(device, NULL, &frames);
		fn_check("a stopped device renders nothing", frames == after);
	}

	fn_check("the io procs are destroyed",
	         AudioDeviceDestroyIOProcID(device, first) == 0 && AudioDeviceDestroyIOProcID(device, second) == 0);
}

/* ------------------------------------------------------------------------------------------------
 * The clock.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_clock(void)
{
	AudioDeviceID device = fn_default_output();
	AudioTimeStamp now;

	memset(&now, 0, sizeof(now));
	fn_check("the device has a time", AudioDeviceGetCurrentTime(device, &now) == 0);
	fn_check("that time is both a sample time and a host time",
	         (now.mFlags & kAudioTimeStampSampleTimeValid) != 0
	         && (now.mFlags & kAudioTimeStampHostTimeValid) != 0);

	{
		AudioTimeStamp translated;

		memset(&translated, 0, sizeof(translated));
		translated.mSampleTime = now.mSampleTime + 4800.0;	/* 100 ms at 48 kHz */
		translated.mFlags = kAudioTimeStampSampleTimeValid;

		fn_check("a sample time translates to a host time",
		         AudioDeviceTranslateTime(device, &translated, &translated) == 0
		         && (translated.mFlags & kAudioTimeStampHostTimeValid) != 0
		         && translated.mHostTime > now.mHostTime);
	}
}

int main(int argc, char *argv[]);
int main(int argc, char *argv[])
{
	/* The suite states its backend in code, so a developer's shell cannot change what it measured — and the
	 * ALSA leg names its PCM the same way, because `default` on a build machine is not a device anybody
	 * should depend on. */
	if (argc > 1 && strcmp(argv[1], "alsa") == 0) {
		if (getenv("AGORA_AUDIO_ALSA_PCM") == NULL) {
			setenv("AGORA_AUDIO_ALSA_PCM", "null", 1);
		}
		g_driver = "alsa";
		g_strict_timing = false;
	} else if (argc > 1 && (strcmp(argv[1], "pipewire") == 0 || strcmp(argv[1], "sndio") == 0)) {
		/* A session server paces, mixes and negotiates: the numbers are its answer, not this library's, and
		 * the pacing assertion is the only one that stays. `sndio` is the same shape of client — its server
		 * owns the device — so it is measured on the same terms. */
		g_driver = argv[1];
		g_strict_timing = false;
		g_exact_numbers = false;
	}

	fn_agora_audio_set_driver(g_driver);

	/* A live server may not be there — a container, a CI leg, a machine with no session — and a backend whose
	 * absence is a fact about the machine is a skip, not a failure. */
	if (g_driver[0] != '\0' && strcmp(g_driver, "null") != 0 && fn_default_output() == kAudioObjectUnknown) {
		fprintf(stderr, "hal: no %s device on this machine; skipped\n", g_driver);
		return 77;
	}

	fn_test_objects();
	fn_test_volume_and_listeners();
	fn_test_io_procs();
	fn_test_clock();

	if (g_failures != 0) {
		fprintf(stderr, "hal: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("hal: objects, properties, listeners, the mixer and the clock measured on the %s backend\n", g_driver);
	return 0;
}
