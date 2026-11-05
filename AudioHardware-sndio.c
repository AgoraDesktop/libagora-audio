/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardware-sndio.c — the fourth backend: `sndio`, the BSDs' sound server, on the same object model.
 *
 * WRITTEN, AND NOT YET MEASURED WHERE IT MATTERS — the same label, for the same reason, as
 * `libagora-watch`'s `EventStream-kqueue.c`. This tree is developed on Linux, where the suite exercises the
 * `null`, ALSA and PipeWire backends; `sndio` is what FreeBSD, NetBSD and OpenBSD ship, and a run on one of
 * them is the thing that would turn "written" into "measured". It is NOT untested code here either: this
 * machine has `libsndio-dev` 1.10.0, so the file compiles and links against the real header and library, and
 * the suite leg (`hal-sndio`) runs and SKIPS (77) because no `sndiod` is listening. What cannot be checked
 * here is what a real server does with the parameters below.
 *
 * IT IS A CLIENT OF A SOUND SERVER, LIKE PIPEWIRE, AND THE PROBE SAYS SO. `sio_open` is the only question
 * `sndio` answers before a stream exists — there is no registry to enumerate — so `device_count` opens and
 * closes a handle once and caches the answer. No server means no device, which is exactly the fallback
 * condition the HAL wants and the reason this backend can be compiled into a Linux library harmlessly.
 *
 * THE SAMPLES ARE CONVERTED HERE, BECAUSE SNDIO HAS NO FLOAT. `struct sio_par` describes signed or unsigned
 * integers of 8, 16, 24 or 32 bits — there is no `SIO_PCM_FORMAT_FLOAT` — so the mixer's interleaved float32
 * is scaled to 32-bit signed and written. That is one conversion in the driver thread, which is where ALSA's
 * float and PipeWire's `SPA_AUDIO_FORMAT_F32` do not need one; the difference is the interface's, not a
 * choice.
 *
 * A BLOCK THAT IS NOT COMPILED WHEN SNDIO IS NOT THERE: `AGORA_WITH_SNDIO` is set by the build when
 * `pkg-config sndio` finds the library, and the provider answers NULL otherwise.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AGORA_WITH_SNDIO)
#include <sndio.h>
#endif

#define AGORA_SNDIO_DEVICE_ID 400
#define AGORA_SNDIO_FRAMES    512

#if defined(AGORA_WITH_SNDIO)

typedef struct {
	bool           running;
	bool           quit;
	pthread_t      thread;
	AudioDeviceID  device;
	struct sio_hdl *handle;
	UInt32         frames;
	UInt32         channels;
	UInt32         rate;
	float *        mix;	/* the mixer's interleaved float32 */
	int32_t *      pcm;	/* the same frames as sndio's 32-bit signed */
} sndio_engine;

typedef struct {
	bool   probed;
	bool   usable;
	UInt32 channels;
	UInt32 rate;
	char   name[256];
} sndio_device;

static sndio_engine g_sndio;
static sndio_device g_sndio_device;
static pthread_mutex_t g_sndio_lock = PTHREAD_MUTEX_INITIALIZER;

/* `AGORA_AUDIO_SNDIO_DEV` mirrors `AGORA_AUDIO_ALSA_PCM`: unset means sndio's own rules (`AUDIODEV`, then the
 * server's default), which is the same courtesy the ALSA backend pays. */
static const char *fn_sndio_name(void)
{
	const char *name = getenv("AGORA_AUDIO_SNDIO_DEV");

	return (name != NULL && name[0] != '\0') ? name : NULL;
}

/* The parameters this backend asks for, in one place so the probe and the stream cannot disagree: 32-bit
 * signed, native endian, most-significant-bit aligned, the device's channel count, our rate. */
static void fn_sndio_parameters(struct sio_par *par, UInt32 rate, UInt32 channels)
{
	sio_initpar(par);
	par->bits = 32;
	par->bps = SIO_BPS(32);
	par->sig = 1;
	par->le = SIO_LE_NATIVE;
	par->msb = 1;
	par->rchan = 0;		/* playback only */
	par->pchan = channels;
	par->rate = rate;
	par->appbufsz = rate * AGORA_SNDIO_FRAMES / 48000;	/* the same slice, at this device's rate */
}

/* One open, one setpar, one getpar, one close — cached, because a property read must not talk to a server on
 * every call. `sio_setpar` before `sio_start` negotiates without playing anything. */
static void fn_sndio_probe(void)
{
	struct sio_hdl *handle;
	struct sio_par wanted;
	struct sio_par actual;

	if (g_sndio_device.probed) {
		return;
	}
	g_sndio_device.probed = true;
	g_sndio_device.usable = false;
	g_sndio_device.channels = 2;
	g_sndio_device.rate = 48000;

	handle = sio_open(fn_sndio_name(), SIO_PLAY, 0);
	if (handle == NULL) {
		return;	/* no server, no device: the HAL falls through to the next backend */
	}

	fn_sndio_parameters(&wanted, 48000, 2);
	if (sio_setpar(handle, &wanted) == 0) {
		sio_close(handle);
		return;
	}

	/* WHAT THE SERVER GRANTED, not what was asked for: a device fixed at 44.1 kHz stereo must be described as
	 * such, or the HAL builds a stream at a rate the server will resample anyway and the suite asserts a lie. */
	memset(&actual, 0, sizeof(actual));
	if (sio_getpar(handle, &actual) != 0 && actual.pchan > 0 && actual.rate > 0) {
		g_sndio_device.channels = actual.pchan;
		g_sndio_device.rate = actual.rate;
		g_sndio_device.usable = true;
	}

	sio_close(handle);

	if (g_sndio_device.usable) {
		const char *name = fn_sndio_name();

		snprintf(g_sndio_device.name, sizeof(g_sndio_device.name), "%s",
		         name != NULL ? name : "sndio default");
	}
}

static UInt32 fn_sndio_device_count(void)
{
	pthread_mutex_lock(&g_sndio_lock);
	fn_sndio_probe();
	pthread_mutex_unlock(&g_sndio_lock);

	return g_sndio_device.usable ? 1 : 0;
}

static bool fn_sndio_device_describe(UInt32 index, agora_device_desc *out)
{
	bool described = false;

	if (out == NULL || index != 0) {
		return false;
	}

	pthread_mutex_lock(&g_sndio_lock);
	fn_sndio_probe();
	if (g_sndio_device.usable) {
		out->id = AGORA_SNDIO_DEVICE_ID;
		out->name = g_sndio_device.name;
		out->uid = "agora:sndio:default";
		out->manufacturer = "sndio";
		out->transport = kAudioDeviceTransportTypeBuiltIn;
		out->nominal_rate = (Float64)g_sndio_device.rate;
		out->buffer_frames = AGORA_SNDIO_FRAMES;
		out->channels = g_sndio_device.channels;
		out->has_input = false;
		out->has_output = true;
		out->terminal_type = kAudioStreamTerminalTypeSpeaker;
		described = true;
	}
	pthread_mutex_unlock(&g_sndio_lock);

	return described;
}

static void *fn_sndio_thread(void *argument)
{
	sndio_engine *engine = argument;
	const size_t bytes = (size_t)engine->frames * engine->channels * sizeof(int32_t);

	while (!engine->quit) {
		UInt32 i;
		size_t written;

		fn_agora_device_render(engine->device, NULL, engine->mix, engine->frames);

		/* FLOAT TO 32-BIT SIGNED, once per sample, clamped rather than wrapped: a mixer that saturates at the
		 * rails must not turn +1.0 into -1.0 on the way out. */
		for (i = 0; i < engine->frames * engine->channels; i++) {
			float sample = engine->mix[i];
			double scaled = (double)sample * 2147483647.0;

			if (scaled > 2147483647.0) {
				scaled = 2147483647.0;
			}
			if (scaled < -2147483648.0) {
				scaled = -2147483648.0;
			}
			engine->pcm[i] = (int32_t)scaled;
		}

		written = sio_write(engine->handle, engine->pcm, bytes);
		if (written != bytes) {
			/* A short write is the server going away, not back-pressure: `sio_write` blocks until the buffer
			 * has room, so the only way to come back short is `sio_eof`. */
			if (sio_eof(engine->handle)) {
				break;
			}
		}
	}

	return NULL;
}

static bool fn_sndio_start(AudioDeviceID id)
{
	bool started = false;

	pthread_mutex_lock(&g_sndio_lock);

	if (g_sndio.running) {
		started = (g_sndio.device == id);	/* one stream per process, like the ALSA backend */
	} else {
		struct sio_hdl *handle = sio_open(fn_sndio_name(), SIO_PLAY, 0);

		if (handle != NULL) {
			struct sio_par wanted;
			struct sio_par actual;
			bool accepted;

			fn_sndio_probe();
			fn_sndio_parameters(&wanted, g_sndio_device.rate, g_sndio_device.channels);
			accepted = sio_setpar(handle, &wanted) != 0;

			memset(&actual, 0, sizeof(actual));
			if (accepted) {
				accepted = sio_getpar(handle, &actual) != 0;
			}

			/* THE PARAMETERS THE SERVER GRANTED ARE THE ONES THE THREAD WILL WRITE. If it rounded the sample
			 * format away from 32-bit signed native-endian — which `sio_setpar` is allowed to do — the bytes this
			 * driver produces would be interpreted as something else, so it refuses rather than playing noise. */
			if (accepted
			    && (actual.bits != 32 || actual.sig != 1 || actual.le != SIO_LE_NATIVE || actual.msb != 1
			        || actual.pchan == 0 || actual.rate == 0)) {
				accepted = false;
			}

			if (accepted) {
				memset(&g_sndio, 0, sizeof(g_sndio));
				g_sndio.device = id;
				g_sndio.channels = actual.pchan;
				g_sndio.rate = actual.rate;
				g_sndio.frames = AGORA_SNDIO_FRAMES;
				g_sndio.handle = handle;
				g_sndio.mix = malloc((size_t)g_sndio.frames * g_sndio.channels * sizeof(float));
				g_sndio.pcm = malloc((size_t)g_sndio.frames * g_sndio.channels * sizeof(int32_t));

				if (g_sndio.mix != NULL && g_sndio.pcm != NULL && sio_start(handle) != 0) {
					g_sndio.quit = false;
					if (pthread_create(&g_sndio.thread, NULL, fn_sndio_thread, &g_sndio) == 0) {
						g_sndio.running = true;
						started = true;
					}
				}
			}

			if (!started) {
				free(g_sndio.mix);
				free(g_sndio.pcm);
				g_sndio.mix = NULL;
				g_sndio.pcm = NULL;
				sio_close(handle);
				memset(&g_sndio, 0, sizeof(g_sndio));
			}
		}
	}

	pthread_mutex_unlock(&g_sndio_lock);
	return started;
}

static void fn_sndio_stop(AudioDeviceID id)
{
	pthread_t thread;
	bool join = false;

	pthread_mutex_lock(&g_sndio_lock);

	if (g_sndio.running && g_sndio.device == id) {
		g_sndio.quit = true;
		thread = g_sndio.thread;
		g_sndio.running = false;
		join = true;
	}

	pthread_mutex_unlock(&g_sndio_lock);

	if (!join) {
		return;
	}

	/* THE THREAD IS JOINED BEFORE THE HANDLE IS TOUCHED: `sio_write` blocks until the server has room, so a
	 * handle closed underneath it is a use-after-free rather than a short write. `sio_stop` after the join also
	 * lets the server drain what is queued, which is what a stop should do. */
	pthread_join(thread, NULL);

	pthread_mutex_lock(&g_sndio_lock);
	if (g_sndio.handle != NULL) {
		sio_stop(g_sndio.handle);
		sio_close(g_sndio.handle);
		g_sndio.handle = NULL;
	}
	free(g_sndio.mix);
	free(g_sndio.pcm);
	g_sndio.mix = NULL;
	g_sndio.pcm = NULL;
	pthread_mutex_unlock(&g_sndio_lock);
}

static const agora_driver kSndioDriver = {
	"sndio",
	fn_sndio_device_count,
	fn_sndio_device_describe,
	fn_sndio_start,
	fn_sndio_stop
};

#endif /* AGORA_WITH_SNDIO */

const agora_driver *fn_agora_driver_sndio(void)
{
#if defined(AGORA_WITH_SNDIO)
	return &kSndioDriver;
#else
	return NULL;
#endif
}
