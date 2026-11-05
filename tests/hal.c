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

/** Read the mixer's last peak until it says what the caller expects, or a deadline passes.
 *
 * **A PROPERTY CHANGE REACHES THE MIXER WHEN THE DEVICE NEXT RENDERS**, and on a live session server that instant is
 * the server's, not the caller's: this suite read the peak once after a fixed sleep and failed about one run in three
 * on `hal-pipewire` — on a machine where nothing was wrong. A deadline is the honest way to ask a question about a
 * clock somebody else owns. */
static bool fn_peaks_until(AudioDeviceID device, float low, float high, long deadline_ms)
{
	long waited = 0;

	for (;;) {
		float peak = -1.0f;

		fn_agora_audio_test_peaks(device, &peak, NULL);
		if (peak >= low && peak <= high) {
			return true;
		}
		if (waited >= deadline_ms) {
			return false;
		}

		fn_sleep_ms(10);
		waited += 10;
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
		fn_check("a muted device mixes silence", fn_peaks_until(device, 0.0f, 0.0f, 200));

		on = 0;
		AudioObjectSetPropertyData(device, &mute, 0, NULL, sizeof(on), &on);
		fn_check("unmuting restores the mix", fn_peaks_until(device, 0.37f, 0.38f, 200));
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

/* ------------------------------------------------------------------------------------------------
 * The input side of the device: a capture stream, and the io proc that is handed it.
 *
 * THE NULL DEVICE HEARS A RAMP, SO THIS IS MEASURABLE WITHOUT A MICROPHONE. Frame `n` of the device's life is
 * `(n % rate) / rate` on the left and half that on the right, so "the io proc was given the device's input" is one
 * subtraction: consecutive frames step by 1/48000, and a dropped or duplicated frame is a step that is not.
 *
 * THE OTHER TWO LEGS CAN ONLY MEASURE DELIVERY, AND SAY SO. ALSA's `null` PCM opens for capture and returns the full
 * quantum every cycle, but every sample is 0.0 — a plugin recording silence is not a defect; the session server's
 * default source returns whatever the room sounds like. A suite that demanded a ramp there would be demanding the
 * fixture of a different backend, so the values are asserted on the backend that owns a known source and the shape
 * (a stream, a direction, a cycle's worth of frames) on all of them. `sndio` opens no capture stream yet, so it says so
 * and moves on.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	int    calls;
	UInt32 frames;
	bool   saw_input;
	bool   check_ramp;	/* the null backend's ramp is the only source this suite knows */
	bool   ramp;		/* every consecutive pair stepped up the ramp (a wrap is allowed) */
	bool   continuous;	/* and the first frame of each cycle carries on from the last one's last */
	bool   half_right;	/* the right channel is half the left, in the same frame */
	float  previous;	/* the previous cycle's last left sample */
} input_watch;

static OSStatus fn_watch_input(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                               const AudioTimeStamp *input_time, AudioBufferList *output,
                               const AudioTimeStamp *output_time, void *client)
{
	input_watch *watch = client;
	const float *samples;
	UInt32 channels;
	UInt32 frames;
	UInt32 i;

	(void)device;
	(void)now;
	(void)input_time;
	(void)output;
	(void)output_time;

	if (input == NULL || input->mNumberBuffers == 0 || input->mBuffers[0].mData == NULL) {
		return 0;
	}

	samples = input->mBuffers[0].mData;
	channels = input->mBuffers[0].mNumberChannels;
	frames = channels > 0 ? input->mBuffers[0].mDataByteSize / (channels * (UInt32)sizeof(float)) : 0;
	if (frames < 2 || channels < 2) {
		return 0;
	}

	watch->calls++;
	watch->frames = frames;
	watch->saw_input = true;

	if (!watch->check_ramp) {
		return 0;
	}

	watch->half_right = watch->half_right && (samples[1] == samples[0] * 0.5f);

	/* **THE SEAM BETWEEN TWO CYCLES IS WHERE A FRAME GOES MISSING.** One buffer's samples being in order says nothing
	 * about the join between buffers, which is exactly what a dropped or duplicated quantum looks like. */
	if (watch->calls > 1) {
		float delta = samples[0] - watch->previous;
		bool stepped = (delta > 1.5e-5f && delta < 2.5e-5f) || delta < -0.5f;

		if (!stepped) {
			watch->continuous = false;
		}
	}
	watch->previous = samples[(frames - 1) * channels];

	for (i = 1; i < frames; i++) {
		float delta = samples[i * channels] - samples[(i - 1) * channels];
		bool stepped = (delta > 1.5e-5f && delta < 2.5e-5f)	/* one frame up the ramp */
		            || delta < -0.5f;				/* the one-second wrap */

		if (!stepped) {
			watch->ramp = false;
		}
	}

	return 0;
}

static void fn_test_input_device(void)
{
	input_watch watch;
	AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultInputDevice,
	                                       kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMaster };
	AudioDeviceIOProcID proc = NULL;
	AudioDeviceID input = kAudioObjectUnknown;
	UInt32 size = sizeof(input);

	/* Every backend that is compiled in has a capture path now; a server that has nothing to record from is a fact
	 * about the machine, not a missing door, so the leg states what it measured rather than skipping. */
	memset(&watch, 0, sizeof(watch));

	watch.check_ramp = strcmp(g_driver, "null") == 0;
	watch.ramp = true;
	watch.continuous = true;
	watch.half_right = true;

	/* Not a failure: a machine with no microphone is a machine with no microphone. What *would* be a failure is a
	 * backend that offers one and then hands the io proc nothing, which is what the rest of this measures. */
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &input) != 0
	    || input == kAudioObjectUnknown) {
		printf("      input: this backend describes no capture stream on this machine\n");
		return;
	}

	/* The input scope describes one stream, and the stream says which side of the device it is. */
	address.mSelector = kAudioDevicePropertyStreams;
	address.mScope = kAudioObjectPropertyScopeInput;
	{
		AudioStreamID streams[4];
		UInt32 streams_size = sizeof(streams);

		fn_check("the device has one input stream",
		         AudioObjectGetPropertyData(input, &address, 0, NULL, &streams_size, &streams) == 0
		             && streams_size == sizeof(AudioStreamID));
		if (streams_size == sizeof(AudioStreamID)) {
			AudioObjectPropertyAddress direction = { kAudioStreamPropertyDirection,
			                                         kAudioObjectPropertyScopeGlobal,
			                                         kAudioObjectPropertyElementMaster };
			UInt32 value = 99;
			UInt32 value_size = sizeof(value);

			fn_check("the stream is an input, by its own direction",
			         AudioObjectGetPropertyData(streams[0], &direction, 0, NULL, &value_size, &value) == 0
			             && value == 1);
		}
	}

	fn_check("an io proc on the input device is created",
	         AudioDeviceCreateIOProcID(input, fn_watch_input, &watch, &proc) == 0 && proc != NULL);
	fn_check("the input device starts", AudioDeviceStart(input, NULL) == 0);

	/* A session server's quantum is the graph's, not ours, so "a cycle arrived" is a *when*: wait for the first one
	 * that carries input, with a deadline, rather than assuming one lands inside a fixed sleep. */
	{
		long waited = 0;

		while (watch.calls < 2 && waited < 500) {
			fn_sleep_ms(10);
			waited += 10;
		}
	}

	fn_check("the io proc is handed the device's input", watch.calls > 0 && watch.saw_input);
	fn_check("a cycle of input is the device's quantum", !g_exact_numbers || watch.frames == 512);
	if (watch.check_ramp) {
		fn_check("the input is the device's ramp, in order and without a gap", watch.ramp);
		fn_check("one cycle carries on from the one before it", watch.continuous);
		fn_check("the input's two channels are not the same signal", watch.half_right);
		printf("      input: %d calls of %u frames, the null backend's ramp\n", watch.calls,
		       (unsigned)watch.frames);
	} else {
		printf("      input: %d calls of %u frames, delivered (this PCM chooses the samples)\n", watch.calls,
		       (unsigned)watch.frames);
	}
	fn_check("the input device stops", AudioDeviceStop(input, NULL) == 0);
	fn_check("the io proc is destroyed", AudioDeviceDestroyIOProcID(input, proc) == 0);
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
	fn_test_input_device();

	if (g_failures != 0) {
		fprintf(stderr, "hal: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("hal: objects, properties, listeners, the mixer and the clock measured on the %s backend\n", g_driver);
	return 0;
}
