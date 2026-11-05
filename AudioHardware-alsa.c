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
 * IT RECORDS FROM THE SAME PCM WHEN THAT PCM CAN RECORD. ALSA is one device with two directions, and the two are
 * asked for separately: `snd_pcm_open(SND_PCM_STREAM_CAPTURE)` on the same name, configured with the same format, and
 * read *before* the mixer is asked for output so that the two directions stay on one cycle. Capture is optional —
 * a PCM that plays but cannot record (or grants a different rate) leaves the output alone and passes no input, which
 * is the difference between a desktop that cannot record and one that cannot make a sound. MEASURED on ALSA's `null`
 * PCM: capture opens, `snd_pcm_readi` returns the full quantum every cycle and every sample is 0.0, because that
 * plugin records silence — so the ALSA leg of the suite measures *delivery*, and the value assertion is the null
 * backend's (which hears a ramp).
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
	snd_pcm_t *    playback;
	snd_pcm_t *    capture;		/* NULL when this PCM cannot record, or grants another rate */
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

/* Whether this PCM can record, asked the same way the playback probe asks: a real open, cached, because an open is
 * not free and `AGORA_AUDIO_ALSA_PCM` is read once. A PCM that plays and cannot record is a real device, so the two
 * answers are independent. */
static bool fn_alsa_capture_opens(void)
{
	static int probed = -1;

	if (probed < 0) {
		const char *name = getenv("AGORA_AUDIO_ALSA_PCM");
		const char *pcm_name = (name != NULL && name[0] != '\0') ? name : "default";
		snd_pcm_t *pcm = NULL;

		probed = (snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) == 0) ? 1 : 0;
		if (pcm != NULL) {
			snd_pcm_close(pcm);
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
	out->has_input = fn_alsa_capture_opens();
	out->has_output = true;
	out->terminal_type = kAudioStreamTerminalTypeSpeaker;

	return true;
}

/* Configure an opened PCM for the engine's format, and prepare it. The rate and the quantum are in/out because the
 * device may not grant what was asked — and the temporaries are not decoration: a `snd_pcm_uframes_t` is
 * `unsigned long`, so handing ALSA the address of a `UInt32` field writes eight bytes into a four-byte slot. */
static bool fn_alsa_configure(snd_pcm_t *pcm, UInt32 channels, UInt32 *rate, UInt32 *frames)
{
	snd_pcm_hw_params_t *params = NULL;
	unsigned int granted_rate = *rate;
	snd_pcm_uframes_t granted_period = *frames;
	bool ok;

	snd_pcm_hw_params_alloca(&params);

	ok = snd_pcm_hw_params_any(pcm, params) >= 0
	  && snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0
	  && snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_FLOAT_LE) >= 0
	  && snd_pcm_hw_params_set_channels(pcm, params, channels) >= 0
	  && snd_pcm_hw_params_set_rate_near(pcm, params, &granted_rate, 0) >= 0
	  && snd_pcm_hw_params_set_period_size_near(pcm, params, &granted_period, 0) >= 0
	  && snd_pcm_hw_params(pcm, params) >= 0;

	if (!ok) {
		return false;
	}

	*rate = granted_rate;
	*frames = (UInt32)granted_period;

	return snd_pcm_prepare(pcm) >= 0;
}

static void *fn_alsa_thread(void *argument)
{
	alsa_engine *engine = argument;
	size_t samples = (size_t)engine->frames * engine->channels;
	float *mix = malloc(samples * sizeof(float));
	float *input = malloc(samples * sizeof(float));

	if (mix == NULL || input == NULL) {
		free(mix);
		free(input);
		return NULL;
	}

	while (!engine->quit) {
		const float *capture = NULL;
		UInt32 capture_frames = 0;
		snd_pcm_sframes_t written;

		/* **THE INPUT IS READ BEFORE THE MIXER IS ASKED FOR OUTPUT.** Both directions are one cycle of one device,
		 * so the cycle starts with what the device has already heard. A short read (an xrun, a signal) is passed as
		 * the shorter input it is — the seam carries the input's own length — rather than dropped or padded. */
		if (engine->capture != NULL) {
			snd_pcm_sframes_t heard = snd_pcm_readi(engine->capture, input,
			                                        (snd_pcm_uframes_t)engine->frames);

			if (heard > 0) {
				capture = input;
				capture_frames = (UInt32)heard;
			} else if (heard == -EPIPE || heard == -ESTRPIPE) {
				snd_pcm_prepare(engine->capture);
			}
		}

		fn_agora_device_render(engine->device, capture, capture_frames, mix, engine->frames);

		written = snd_pcm_writei(engine->playback, mix, (snd_pcm_uframes_t)engine->frames);
		if (written < 0) {
			/* An underrun is normal after a scheduling hiccup; a dead device is not. */
			if (written == -EPIPE) {
				snd_pcm_prepare(engine->playback);
				continue;
			}
			if (written == -ESTRPIPE) {
				while (snd_pcm_resume(engine->playback) == -EAGAIN) {
					/* wait until the device comes back */
				}
				snd_pcm_prepare(engine->playback);
				continue;
			}
			break;
		}
	}

	free(mix);
	free(input);
	return NULL;
}

static bool fn_alsa_start(AudioDeviceID id, bool want_input)
{
	bool started = false;

	pthread_mutex_lock(&g_alsa_lock);

	if (!g_alsa.running) {
		const char *name = getenv("AGORA_AUDIO_ALSA_PCM");

		memset(&g_alsa, 0, sizeof(g_alsa));
		g_alsa.device = id;
		g_alsa.frames = 512;
		g_alsa.channels = 2;
		g_alsa.rate = 48000;
		snprintf(g_alsa.pcm_name, sizeof(g_alsa.pcm_name), "%s", name != NULL ? name : "default");

		if (snd_pcm_open(&g_alsa.playback, g_alsa.pcm_name, SND_PCM_STREAM_PLAYBACK, 0) < 0
		    || !fn_alsa_configure(g_alsa.playback, g_alsa.channels, &g_alsa.rate, &g_alsa.frames)) {
			if (g_alsa.playback != NULL) {
				snd_pcm_close(g_alsa.playback);
				g_alsa.playback = NULL;
			}
			pthread_mutex_unlock(&g_alsa_lock);
			return false;
		}

		/* Capture is the other direction of the same PCM, and it is optional: a device that plays but cannot
		 * record, or records at another rate, leaves the output alone. The rate is the one thing that must
		 * agree — the HAL describes one rate for the device, so a capture stream on another clock would be
		 * handed to io procs as if it were this one. */
		{
			UInt32 capture_rate = g_alsa.rate;
			UInt32 capture_frames = g_alsa.frames;

			/* **ONLY WHEN A CLIENT ASKED TO RECORD**: a capture PCM held open is a microphone held open, and a
			 * playback client has no business doing that. */
			if (want_input && fn_alsa_capture_opens()
			    && snd_pcm_open(&g_alsa.capture, g_alsa.pcm_name, SND_PCM_STREAM_CAPTURE, 0) == 0) {
				if (!fn_alsa_configure(g_alsa.capture, g_alsa.channels, &capture_rate, &capture_frames)
				    || capture_rate != g_alsa.rate) {
					snd_pcm_close(g_alsa.capture);
					g_alsa.capture = NULL;
				}
			} else {
				g_alsa.capture = NULL;
			}
		}

		g_alsa.quit = false;
		if (pthread_create(&g_alsa.thread, NULL, fn_alsa_thread, &g_alsa) == 0) {
			g_alsa.running = true;
			started = true;
		} else {
			snd_pcm_close(g_alsa.playback);
			g_alsa.playback = NULL;
			if (g_alsa.capture != NULL) {
				snd_pcm_close(g_alsa.capture);
				g_alsa.capture = NULL;
			}
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
		if (g_alsa.capture != NULL) {
			snd_pcm_close(g_alsa.capture);
			g_alsa.capture = NULL;
		}
		if (g_alsa.playback != NULL) {
			snd_pcm_drain(g_alsa.playback);
			snd_pcm_close(g_alsa.playback);
			g_alsa.playback = NULL;
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
