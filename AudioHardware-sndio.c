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
 * IT RECORDS TOO, WITH A SECOND HANDLE AND THE SAME CONVERSION IN REVERSE. `sio_open(name, SIO_REC, 0)` is its own
 * handle — sndio has no duplex mode — configured with `rchan` where playback uses `pchan`, and the thread reads a
 * quantum *before* it asks the mixer for output so both directions are one cycle. sndio cannot give float, so the
 * 32-bit signed samples are scaled back to float32 here: `SIO_PCM` has no float and that is the interface's shape, not
 * a choice. Capture is optional, exactly as in the ALSA backend: a server with no recording device (or one that
 * grants another rate) leaves playback alone and passes no input.
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
	struct sio_hdl *capture;	/* NULL when no recording device answered, or it granted other parameters */
	UInt32         frames;
	UInt32         channels;
	UInt32         rate;
	float *        mix;		/* the mixer's interleaved float32 */
	int32_t *      pcm;		/* the same frames as sndio's 32-bit signed */
	int32_t *      capture_pcm;	/* what the server recorded, in its own format */
	float *        capture_mix;	/* and the same frames as this library's float32 */
} sndio_engine;

typedef struct {
	bool   probed;
	bool   usable;
	bool   input;		/* whether a recording handle opened as well */
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
 * signed, native endian, most-significant-bit aligned, the device's channel count, our rate — with the channel count
 * on the playback side (`pchan`) or the recording side (`rchan`), because a sndio handle is one direction. */
static void fn_sndio_parameters(struct sio_par *par, UInt32 rate, UInt32 channels, bool capture)
{
	sio_initpar(par);
	par->bits = 32;
	par->bps = SIO_BPS(32);
	par->sig = 1;
	par->le = SIO_LE_NATIVE;
	par->msb = 1;
	par->rchan = capture ? channels : 0;
	par->pchan = capture ? 0 : channels;
	par->rate = rate;
	par->appbufsz = rate * AGORA_SNDIO_FRAMES / 48000;	/* the same slice, at this device's rate */
}

/* Whether the server granted the sample format this driver produces or consumes, and a channel count and rate at all.
 * A granted format other than 32-bit signed native-endian would be interpreted as something else by the other side, so
 * the driver refuses it rather than playing — or recording — noise. */
static bool fn_sndio_granted(const struct sio_par *par, bool capture)
{
	return par->bits == 32 && par->sig == 1 && par->le == SIO_LE_NATIVE && par->msb == 1
	    && (capture ? par->rchan : par->pchan) > 0 && par->rate > 0;
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

	fn_sndio_parameters(&wanted, 48000, 2, false);
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

	/* And the other direction, asked the same way: a server with a playback device need not have a recording one. */
	if (g_sndio_device.usable) {
		struct sio_hdl *recording = sio_open(fn_sndio_name(), SIO_REC, 0);

		if (recording != NULL) {
			struct sio_par capture_wanted;
			struct sio_par capture_actual;

			fn_sndio_parameters(&capture_wanted, g_sndio_device.rate, g_sndio_device.channels, true);
			memset(&capture_actual, 0, sizeof(capture_actual));
			g_sndio_device.input = sio_setpar(recording, &capture_wanted) != 0
			                    && sio_getpar(recording, &capture_actual) != 0
			                    && fn_sndio_granted(&capture_actual, true)
			                    && capture_actual.rate == g_sndio_device.rate
			                    && capture_actual.rchan == g_sndio_device.channels;
			sio_close(recording);
		}
	}

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
		out->has_input = g_sndio_device.input;
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
	const size_t capture_bytes = (size_t)engine->frames * engine->channels * sizeof(int32_t);
	const size_t bytes = (size_t)engine->frames * engine->channels * sizeof(int32_t);

	while (!engine->quit) {
		const float *capture = NULL;
		UInt32 capture_frames = 0;
		UInt32 i;
		size_t written;

		/* **THE INPUT IS READ BEFORE THE MIXER IS ASKED FOR OUTPUT.** A sndio handle is one direction, so the two
		 * directions are two handles on one server; reading first is what keeps them in the same cycle. A short
		 * read (the server going away, a signal) is passed as the shorter input it is. */
		if (engine->capture != NULL) {
			size_t read = sio_read(engine->capture, engine->capture_pcm, capture_bytes);

			if (read == 0 && sio_eof(engine->capture)) {
				break;	/* the server is gone */
			}

			if (read > 0) {
				size_t samples = read / sizeof(int32_t);
				UInt32 frame;

				/* 32-BIT SIGNED TO FLOAT32, clamped the same way the other direction is: -1.0 in sndio's
				 * scale is -2147483648, which divides to exactly -1.0 in a double and is one step past it in
				 * the float it lands in. */
				for (frame = 0; frame < samples; frame++) {
					double scaled = (double)engine->capture_pcm[frame] / 2147483647.0;

					engine->capture_mix[frame] = (float)scaled;
				}

				capture = engine->capture_mix;
				capture_frames = (UInt32)(samples / engine->channels);
			}
		}

		fn_agora_device_render(engine->device, capture, capture_frames, engine->mix, engine->frames);

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

static bool fn_sndio_start(AudioDeviceID id, bool want_input)
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
			fn_sndio_parameters(&wanted, g_sndio_device.rate, g_sndio_device.channels, false);
			accepted = sio_setpar(handle, &wanted) != 0;

			memset(&actual, 0, sizeof(actual));
			if (accepted) {
				accepted = sio_getpar(handle, &actual) != 0;
			}

			/* THE PARAMETERS THE SERVER GRANTED ARE THE ONES THE THREAD WILL WRITE. If it rounded the sample
			 * format away from 32-bit signed native-endian — which `sio_setpar` is allowed to do — the bytes this
			 * driver produces would be interpreted as something else, so it refuses rather than playing noise. */
			if (!accepted || !fn_sndio_granted(&actual, false)) {
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
				g_sndio.capture_pcm = malloc((size_t)g_sndio.frames * g_sndio.channels * sizeof(int32_t));
				g_sndio.capture_mix = malloc((size_t)g_sndio.frames * g_sndio.channels * sizeof(float));

				/* The recording side, when the server has one: its own handle, its own parameters, its own read
				 * thread of control (the same thread — one cycle, two directions). */
				if (want_input && g_sndio_device.input) {
					struct sio_hdl *recording = sio_open(fn_sndio_name(), SIO_REC, 0);

					if (recording != NULL) {
						struct sio_par capture_wanted;
						struct sio_par capture_actual;
						bool capture_accepted;

						fn_sndio_parameters(&capture_wanted, g_sndio.rate, g_sndio.channels, true);
						capture_accepted = sio_setpar(recording, &capture_wanted) != 0;
						memset(&capture_actual, 0, sizeof(capture_actual));
						if (capture_accepted) {
							capture_accepted = sio_getpar(recording, &capture_actual) != 0;
						}

						/* The rate and the channel count must be this device's: the HAL describes one of each
						 * for both directions, so a recording stream on another clock would be handed to the io
						 * procs as if it were this one. */
						if (capture_accepted && fn_sndio_granted(&capture_actual, true)
						    && capture_actual.rate == g_sndio.rate
						    && capture_actual.rchan == g_sndio.channels
						    && sio_start(recording) != 0) {
							g_sndio.capture = recording;
						} else {
							sio_close(recording);
						}
					}
				}

				if (g_sndio.mix != NULL && g_sndio.pcm != NULL && g_sndio.capture_pcm != NULL
				    && g_sndio.capture_mix != NULL && sio_start(handle) != 0) {
					g_sndio.quit = false;
					if (pthread_create(&g_sndio.thread, NULL, fn_sndio_thread, &g_sndio) == 0) {
						g_sndio.running = true;
						started = true;
					}
				}
			}

			if (!started) {
				if (g_sndio.capture != NULL) {
					sio_stop(g_sndio.capture);
					sio_close(g_sndio.capture);
				}
				free(g_sndio.mix);
				free(g_sndio.pcm);
				free(g_sndio.capture_pcm);
				free(g_sndio.capture_mix);
				g_sndio.mix = NULL;
				g_sndio.pcm = NULL;
				g_sndio.capture_pcm = NULL;
				g_sndio.capture_mix = NULL;
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
	if (g_sndio.capture != NULL) {
		sio_stop(g_sndio.capture);
		sio_close(g_sndio.capture);
		g_sndio.capture = NULL;
	}
	if (g_sndio.handle != NULL) {
		sio_stop(g_sndio.handle);
		sio_close(g_sndio.handle);
		g_sndio.handle = NULL;
	}
	free(g_sndio.mix);
	free(g_sndio.pcm);
	free(g_sndio.capture_pcm);
	free(g_sndio.capture_mix);
	g_sndio.mix = NULL;
	g_sndio.pcm = NULL;
	g_sndio.capture_pcm = NULL;
	g_sndio.capture_mix = NULL;
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
