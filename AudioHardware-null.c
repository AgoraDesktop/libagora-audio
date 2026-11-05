/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardware-null.c — the device that exists everywhere, so the HAL can be measured anywhere.
 *
 * ONE DEVICE, 48 kHz, STEREO, 512 frames a quantum, WITH A REAL DATA THREAD. The thread sleeps for the
 * quantum's duration and then calls the mixer, which is what makes the timing assertions in the suite mean
 * something: an io proc that is called "at the device's rate" can only be checked against a clock, and the
 * clock here is the same one a real backend will keep, because the driver seam is the driver seam.
 *
 * IT PLAYS NOTHING, AND THAT IS THE POINT. A test that needs a sound card is a test that does not run on a
 * build box; this backend is the one CI and a headless machine both have. It is also the model every later
 * backend is written against: describe your devices, run a thread, call the mixer.
 *
 * IT ALSO HEARS A RAMP, WHICH IS THE SAME IDEA FOR CAPTURE. A device with no input would make every recording
 * test need hardware, so the device the tests already have describes an input stream too, and its data thread
 * produces a source the suites can predict exactly: frame `n` of the device's life carries
 * `(n % rate) / rate` on the left and **half that on the right**. A ramp is chosen over a tone because the
 * question a capture test asks is not "is this audio" but "are these the frames, in this order, without a gap" —
 * and a ramp answers that in one subtraction. The two channels differing by a factor of two is what makes the
 * channel map measurable as well: a client that sums them must measure 0.75, and one that plays them must not.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AGORA_NULL_DEVICE_ID 100

typedef struct {
	bool           running;
	pthread_t      thread;
	AudioDeviceID  device;
	UInt32         frames;
	UInt32         channels;
	double         rate;
	bool           quit;
	UInt64         captured;	/* frames of the synthetic source produced so far */
} null_engine;

static null_engine g_engine;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static UInt32 fn_null_device_count(void)
{
	return 1;
}

static bool fn_null_device_describe(UInt32 index, agora_device_desc *out)
{
	if (index != 0 || out == NULL) {
		return false;
	}

	out->id = AGORA_NULL_DEVICE_ID;
	out->name = "Agora Null";
	out->uid = "agora:null:0";
	out->manufacturer = "Agora";
	out->transport = kAudioDeviceTransportTypeVirtual;
	out->nominal_rate = 48000.0;
	out->buffer_frames = 512;
	out->channels = 2;
	out->has_input = true;
	out->has_output = true;
	out->terminal_type = kAudioStreamTerminalTypeSpeaker;

	return true;
}

/** The synthetic source: frame `n` is a ramp over one second of the device's life, and the right channel is half
 * the left. Both values are computed rather than stored, so a suite can predict any frame from its index. */
static void fn_null_capture(null_engine *engine, float *capture, UInt32 frames)
{
	UInt32 period = (UInt32)engine->rate;
	UInt32 frame;
	UInt32 channel;

	if (period == 0) {
		period = 48000;
	}

	for (frame = 0; frame < frames; frame++) {
		float ramp = (float)((engine->captured + frame) % period) / (float)period;

		for (channel = 0; channel < engine->channels; channel++) {
			capture[frame * engine->channels + channel] = (channel == 0) ? ramp : ramp * 0.5f;
		}
	}

	engine->captured += frames;
}

static void *fn_null_thread(void *argument)
{
	null_engine *engine = argument;
	float *mix = malloc((size_t)engine->frames * engine->channels * sizeof(float));
	float *capture = malloc((size_t)engine->frames * engine->channels * sizeof(float));
	struct timespec period;

	if (mix == NULL || capture == NULL) {
		free(mix);
		free(capture);
		return NULL;
	}

	/* 512 frames at 48 kHz is 10.666 ms; the thread's period is the quantum's duration, so the io proc's
	 * call rate and the device's rate are the same fact. */
	period.tv_sec = 0;
	period.tv_nsec = (long)((double)engine->frames / engine->rate * 1.0e9);

	while (!engine->quit) {
		struct timespec remaining = period;

		while (nanosleep(&remaining, &remaining) != 0 && !engine->quit) {
			/* Interrupted by a signal; finish the wait unless asked to stop. */
		}

		if (engine->quit) {
			break;
		}

		fn_null_capture(engine, capture, engine->frames);
		fn_agora_device_render(engine->device, capture, mix, engine->frames);
	}

	free(mix);
	free(capture);
	return NULL;
}

static bool fn_null_start(AudioDeviceID id)
{
	bool started = false;

	pthread_mutex_lock(&g_lock);

	if (!g_engine.running) {
		memset(&g_engine, 0, sizeof(g_engine));
		g_engine.device = id;
		g_engine.frames = 512;
		g_engine.channels = 2;
		g_engine.rate = 48000.0;
		g_engine.quit = false;
		g_engine.captured = 0;

		if (pthread_create(&g_engine.thread, NULL, fn_null_thread, &g_engine) == 0) {
			g_engine.running = true;
			started = true;
		}
	} else if (g_engine.device == id) {
		started = true;
	}

	pthread_mutex_unlock(&g_lock);
	return started;
}

static void fn_null_stop(AudioDeviceID id)
{
	pthread_t thread;
	bool join = false;

	pthread_mutex_lock(&g_lock);

	if (g_engine.running && g_engine.device == id) {
		g_engine.quit = true;
		thread = g_engine.thread;
		g_engine.running = false;
		join = true;
	}

	pthread_mutex_unlock(&g_lock);

	if (join) {
		pthread_join(thread, NULL);
	}
}

static const agora_driver kNullDriver = {
	"null",
	fn_null_device_count,
	fn_null_device_describe,
	fn_null_start,
	fn_null_stop
};

const agora_driver *fn_agora_driver_null(void)
{
	return &kNullDriver;
}
