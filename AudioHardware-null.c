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
	out->has_input = false;
	out->has_output = true;
	out->terminal_type = kAudioStreamTerminalTypeSpeaker;

	return true;
}

static void *fn_null_thread(void *argument)
{
	null_engine *engine = argument;
	float *mix = malloc((size_t)engine->frames * engine->channels * sizeof(float));
	struct timespec period;

	if (mix == NULL) {
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

		fn_agora_device_render(engine->device, mix, engine->frames);
	}

	free(mix);
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
