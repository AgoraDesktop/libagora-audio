/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardware-alsa.c — the second backend: a real PCM under the same object model.
 *
 * IT OPENS A NAME, NOT A CARD. `AGORA_AUDIO_ALSA_PCM` (default "default") is handed to `snd_pcm_open`, because
 * opening `hw:` directly is exclusive and is what makes a desktop stop making sound when another application
 * has the device. The `default` PCM is the one that goes through `dmix`, the session server's ALSA door, or
 * the plugin chain — whichever this system has — and that is the right place for a client to be.
 *
 * THE WRITE PACES THE THREAD. In blocking mode `snd_pcm_writei` returns when the device has room for more, so
 * the mixer is asked for exactly what the device can take and no timer is involved: the device's clock is the
 * clock, which is why this backend's frames-per-second assertion is not made in the suite (see `hal.c`).
 *
 * A BLOCK THAT IS NOT COMPILED WHEN ALSA IS NOT THERE: `AGORA_WITH_ALSA` is set by the build when
 * `pkg-config alsa` finds the library, and the driver provider answers NULL otherwise so the HAL falls back
 * to the null backend instead of failing to link.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(AGORA_WITH_ALSA)
#include <alsa/asoundlib.h>
#endif

#define AGORA_ALSA_DEVICE_ID 200

#if defined(AGORA_WITH_ALSA)

typedef struct {
	bool           running;
	bool           quit;
	pthread_t      thread;
	AudioDeviceID  device;
	snd_pcm_t *    pcm;
	UInt32         frames;
	UInt32         channels;
	UInt32         rate;
	char           pcm_name[256];
} alsa_engine;

static alsa_engine g_alsa;
static pthread_mutex_t g_alsa_lock = PTHREAD_MUTEX_INITIALIZER;

/* **THE PROBE IS A REAL OPEN, BECAUSE ALSA DESCRIBES A DEVICE THAT MAY NOT EXIST.** `snd_pcm_open` on the default
 * PCM is the only question whose answer means anything: with no card, or with a card claimed by something else, it
 * fails, and the HAL should then report no device rather than a device that fails at `AudioDeviceStart`. The result is
 * cached for the process, because this is called on every enumeration and an open is not free; `AGORA_AUDIO_ALSA_PCM`
 * is read once, which is what the environment variable's contract already is.
 *
 * MEASURED: on this machine it opens, and the `hal-alsa` leg (against ALSA's `null` PCM) still measures the write loop
 * — the probe does not keep the handle, so the driver opens it again for playback. */
static bool fn_alsa_probe(void)
{
	static int probed = -1;

	if (probed < 0) {
		const char *name = getenv("AGORA_AUDIO_ALSA_PCM");
		const char *pcm_name = (name != NULL && name[0] != '\0') ? name : "default";
		snd_pcm_t *pcm = NULL;

		probed = (snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) ? 1 : 0;
		if (pcm != NULL) {
			snd_pcm_close(pcm);
		}
		if (probed == 0) {
			fprintf(stderr, "libagora-audio: ALSA PCM \"%s\" did not open; no ALSA device\n", pcm_name);
		}
	}
	return probed == 1;
}

static UInt32 fn_alsa_device_count(void)
{
	return fn_alsa_probe() ? 1 : 0;
}

static bool fn_alsa_device_describe(UInt32 index, agora_device_desc *out)
{
	if (index != 0 || out == NULL) {
		return false;
	}

	out->id = AGORA_ALSA_DEVICE_ID;
	out->name = "ALSA Default";
	out->uid = "agora:alsa:default";
	out->manufacturer = "ALSA";
	out->transport = kAudioDeviceTransportTypeBuiltIn;
	out->nominal_rate = 48000.0;
	out->buffer_frames = 512;
	out->channels = 2;
	out->has_input = false;
	out->has_output = true;
	out->terminal_type = kAudioStreamTerminalTypeSpeaker;

	return true;
}

static void *fn_alsa_thread(void *argument)
{
	alsa_engine *engine = argument;
	float *mix = malloc((size_t)engine->frames * engine->channels * sizeof(float));

	if (mix == NULL) {
		return NULL;
	}

	while (!engine->quit) {
		snd_pcm_sframes_t written;

		fn_agora_device_render(engine->device, mix, engine->frames);

		written = snd_pcm_writei(engine->pcm, mix, (snd_pcm_uframes_t)engine->frames);
		if (written < 0) {
			/* An underrun is normal after a scheduling hiccup; a dead device is not. */
			if (written == -EPIPE) {
				snd_pcm_prepare(engine->pcm);
				continue;
			}
			if (written == -ESTRPIPE) {
				while (snd_pcm_resume(engine->pcm) == -EAGAIN) {
					/* wait until the device comes back */
				}
				snd_pcm_prepare(engine->pcm);
				continue;
			}
			break;
		}
	}

	free(mix);
	return NULL;
}

static bool fn_alsa_start(AudioDeviceID id)
{
	bool started = false;

	pthread_mutex_lock(&g_alsa_lock);

	if (!g_alsa.running) {
		const char *name = getenv("AGORA_AUDIO_ALSA_PCM");
		snd_pcm_hw_params_t *params = NULL;
		int status;

		memset(&g_alsa, 0, sizeof(g_alsa));
		g_alsa.device = id;
		g_alsa.frames = 512;
		g_alsa.channels = 2;
		g_alsa.rate = 48000;
		snprintf(g_alsa.pcm_name, sizeof(g_alsa.pcm_name), "%s", name != NULL ? name : "default");

		status = snd_pcm_open(&g_alsa.pcm, g_alsa.pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
		if (status < 0) {
			pthread_mutex_unlock(&g_alsa_lock);
			return false;
		}

		snd_pcm_hw_params_alloca(&params);
		snd_pcm_hw_params_any(g_alsa.pcm, params);
		snd_pcm_hw_params_set_access(g_alsa.pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
		snd_pcm_hw_params_set_format(g_alsa.pcm, params, SND_PCM_FORMAT_FLOAT_LE);
		snd_pcm_hw_params_set_channels(g_alsa.pcm, params, g_alsa.channels);
		/* A `snd_pcm_uframes_t` IS `unsigned long`, NOT THE `UInt32` THESE FIELDS ARE: passing the field's
		 * address makes ALSA write eight bytes into a four-byte slot and corrupt the struct behind it, which
		 * presents as a malloc assertion somewhere else entirely. Locals, then copy back. */
		{
			unsigned int rate = g_alsa.rate;
			snd_pcm_uframes_t period = g_alsa.frames;

			snd_pcm_hw_params_set_rate_near(g_alsa.pcm, params, &rate, 0);
			snd_pcm_hw_params_set_period_size_near(g_alsa.pcm, params, &period, 0);

			g_alsa.rate = rate;
			g_alsa.frames = (UInt32)period;
		}

		if (snd_pcm_hw_params(g_alsa.pcm, params) < 0 || snd_pcm_prepare(g_alsa.pcm) < 0) {
			snd_pcm_close(g_alsa.pcm);
			g_alsa.pcm = NULL;
			pthread_mutex_unlock(&g_alsa_lock);
			return false;
		}

		g_alsa.quit = false;
		if (pthread_create(&g_alsa.thread, NULL, fn_alsa_thread, &g_alsa) == 0) {
			g_alsa.running = true;
			started = true;
		} else {
			snd_pcm_close(g_alsa.pcm);
			g_alsa.pcm = NULL;
		}
	} else if (g_alsa.device == id) {
		started = true;
	}

	pthread_mutex_unlock(&g_alsa_lock);
	return started;
}

static void fn_alsa_stop(AudioDeviceID id)
{
	pthread_t thread;
	bool join = false;

	pthread_mutex_lock(&g_alsa_lock);

	if (g_alsa.running && g_alsa.device == id) {
		g_alsa.quit = true;
		thread = g_alsa.thread;
		g_alsa.running = false;
		join = true;
	}

	pthread_mutex_unlock(&g_alsa_lock);

	if (join) {
		pthread_join(thread, NULL);

		pthread_mutex_lock(&g_alsa_lock);
		if (g_alsa.pcm != NULL) {
			snd_pcm_drain(g_alsa.pcm);
			snd_pcm_close(g_alsa.pcm);
			g_alsa.pcm = NULL;
		}
		pthread_mutex_unlock(&g_alsa_lock);
	}
}

static const agora_driver kAlsaDriver = {
	"alsa",
	fn_alsa_device_count,
	fn_alsa_device_describe,
	fn_alsa_start,
	fn_alsa_stop
};

#endif /* AGORA_WITH_ALSA */

const agora_driver *fn_agora_driver_alsa(void)
{
#if defined(AGORA_WITH_ALSA)
	return &kAlsaDriver;
#else
	return NULL;
#endif
}
