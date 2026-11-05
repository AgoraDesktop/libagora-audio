/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioHardware-pipewire.c — the third backend: the session's own sound server, on the same object model.
 *
 * THIS BACKEND IS A CLIENT OF PIPEWIRE, NOT A SOUND SERVER, which is the line this whole component draws: the
 * server owns the devices, this library presents them with Apple's face. So it enumerates what the registry
 * says are sinks, streams float32 stereo into the one it was asked for, and lets the server decide the clock,
 * A DEVICE IS A SINK, AND ITS INPUT IS THE SESSION'S DEFAULT SOURCE. PipeWire models playback and capture as different
 * nodes (`Audio/Sink`, `Audio/Source`), while this HAL's device is one object with two scopes — so the device the sinks
 * describe offers a capture stream too, and that stream is autoconnected to whatever source the session calls default,
 * with the graph's adapter bringing it to the device's channel count and rate. `has_input` is true only when the server
 * has at least one source node, which is the same "a description is not an answer" rule the sink list follows.
 *
 * **THE TWO STREAMS RENDER ON ONE CYCLE, AND THE PLAYBACK ONE IS THE CLOCK.** Both callbacks run on the same thread
 * loop, so the capture callback leaves its quantum in the engine and the playback callback passes the last one to the
 * mixer — once, so a source that stalls yields no input rather than the same buffer for ever. When the two quanta
 * disagree the cycle goes without input: the seam takes one frame count for both directions, and a half-filled input
 * buffer would be a lie about the audio rather than a small loss of it.
 *
 * the quantum and the resampling. Nothing here mixes, converts or resamples — `fn_agora_device_render` does
 * that, above the driver, exactly as it does for ALSA and the null device.
 *
 * THE REGISTRY IS ASYNCHRONOUS AND THIS INTERFACE IS NOT. `AudioObjectGetPropertyData` must answer *now*, so
 * the driver connects once, does two round trips (the globals, then the metadata that names the default sink),
 * and freezes what it found into a table. A sink that appears later is invisible until the process restarts —
 * stated here rather than hidden, because the alternative is a callback that fires while a client is reading
 * the device list.
 *
 * THE DEFAULT SINK IS FIRST, because `kAudioHardwarePropertyDefaultOutputDevice` is the first device with an
 * output in this HAL, and "which one is default" is a question only the server can answer: it is the
 * `default.audio.sink` entry in PipeWire's `default` metadata, and it is why the metadata object is bound at
 * all.
 *
 * THE PROCESS CALLBACK IS NOT THE REALTIME ONE. `PW_STREAM_FLAG_RT_PROCESS` is deliberately not set: the mixer
 * takes a lock and reads a device's io procs, which is not realtime-safe, so the stream is ordinary and the
 * server tolerates a late buffer instead of the mixer tolerating a corrupted one.
 *
 * A BLOCK THAT IS NOT COMPILED WHEN PIPEWIRE IS NOT THERE: `AGORA_WITH_PIPEWIRE` is set by the build when
 * `pkg-config libpipewire-0.3` finds the library, and the provider answers NULL otherwise so the HAL falls
 * back to the null backend instead of failing to link.
 */

#include <CoreAudio/AudioHardware.h>
#include "AudioToolbox_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AGORA_WITH_PIPEWIRE)

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

#define AGORA_PW_DEVICE_BASE 300
#define AGORA_PW_MAX_SINKS   8
#define AGORA_PW_MAX_STREAMS 8

typedef struct {
	AudioDeviceID id;
	char          name[128];	/* node.name: what `target.object` is given */
	char          description[256];
	UInt32        channels;
	UInt32        rate;
	UInt32        quantum;
	bool          valid;
} pw_sink;

typedef struct {
	bool                running;
	AudioDeviceID       device;
	struct pw_stream *  stream;
	struct spa_hook     stream_listener;	/* must outlive the stream it is added to */
	struct pw_stream *  capture_stream;	/* NULL when the session has no source to record from */
	struct spa_hook     capture_listener;
	float *             capture;		/* the last quantum the source gave us */
	UInt32              capture_capacity;	/* frames `capture` holds */
	UInt32              captured_frames;	/* frames in it, 0 once the cycle has used them */
	UInt32              channels;
	UInt32              rate;
} pw_engine;

static struct {
	bool                    initialised;
	struct pw_thread_loop * loop;
	struct pw_context *     context;
	struct pw_core *        core;
	struct pw_registry *    registry;
	struct pw_metadata *    metadata;
	struct spa_hook         registry_listener;
	struct spa_hook         metadata_listener;
	struct spa_hook         core_listener;

	/* The sync barrier: `pw_core_sync` asks the server for a `done` event with a sequence number, and the
	 * waiting thread is woken by that event. Without it a property read could answer before the globals
	 * arrived, which is the whole difference between a device list and no device list. */
	int                     sync_seq;

	char                    default_sink[128];
	UInt32                  sink_count;
	UInt32                  source_count;	/* whether this session has anything to record from */
	pw_sink                 sinks[AGORA_PW_MAX_SINKS];
	pw_engine               engines[AGORA_PW_MAX_STREAMS];
} g_pw;

static pthread_mutex_t g_pw_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------------------------------
 * Enumeration: the globals, and the metadata that says which one is default.
 * ---------------------------------------------------------------------------------------------- */

static void fn_pw_note_default(const char *value)
{
	struct spa_json iterator;
	struct spa_json object;
	char key[64];

	/* `default.audio.sink` is a JSON object: {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"} */
	if (value == NULL) {
		return;
	}
	spa_json_init(&iterator, value, (int)strlen(value));
	if (spa_json_enter_object(&iterator, &object) <= 0) {
		return;
	}

	/* `spa_json_next` consumes the value after each key, whether or not this code wants it — the iterator
	 * is a cursor, and a key read without its value desynchronises everything after it. */
	while (spa_json_get_string(&object, key, sizeof(key)) > 0) {
		const char *raw = NULL;

		if (spa_json_next(&object, &raw) <= 0) {
			return;
		}
		if (strcmp(key, "name") == 0 && raw != NULL) {
			struct spa_json string;
			char parsed[128];

			spa_json_init(&string, raw, (int)strlen(raw));
			if (spa_json_get_string(&string, parsed, sizeof(parsed)) > 0) {
				snprintf(g_pw.default_sink, sizeof(g_pw.default_sink), "%s", parsed);
				return;
			}
		}
	}
}

static int fn_pw_on_metadata_property(void *data, uint32_t subject, const char *key,
                                      const char *type, const char *value)
{
	(void)data;
	(void)subject;
	(void)type;

	if (key != NULL && strcmp(key, "default.audio.sink") == 0) {
		fn_pw_note_default(value);
	}
	return 0;
}

static const struct pw_metadata_events kMetadataEvents = {
	PW_VERSION_METADATA_EVENTS,
	.property = fn_pw_on_metadata_property
};

static void fn_pw_on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                            uint32_t version, const struct spa_dict *props)
{
	(void)data;
	(void)permissions;
	(void)version;

	if (props == NULL) {
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);

		if (name != NULL && strcmp(name, "default") == 0) {
			g_pw.metadata = pw_registry_bind(g_pw.registry, id, PW_TYPE_INTERFACE_Metadata,
			                                 PW_VERSION_METADATA, 0);
			if (g_pw.metadata != NULL) {
				pw_metadata_add_listener(g_pw.metadata, &g_pw.metadata_listener, &kMetadataEvents, NULL);
			}
		}
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
		return;
	}

	{
		const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
		pw_sink *sink;

		if (media_class == NULL || node_name == NULL) {
			return;
		}

		/* A source is not a device here — the sinks are — but whether one exists is what decides whether a
		 * recording is possible at all. */
		if (strcmp(media_class, "Audio/Source") == 0) {
			g_pw.source_count++;
			return;
		}

		if (strcmp(media_class, "Audio/Sink") != 0) {
			return;
		}
		if (g_pw.sink_count >= AGORA_PW_MAX_SINKS) {
			return;
		}

		sink = &g_pw.sinks[g_pw.sink_count];
		memset(sink, 0, sizeof(*sink));
		sink->id = (AudioDeviceID)(AGORA_PW_DEVICE_BASE + g_pw.sink_count);
		snprintf(sink->name, sizeof(sink->name), "%s", node_name);
		snprintf(sink->description, sizeof(sink->description), "%s",
		         description != NULL ? description : node_name);

		/* The channel count is the one the node advertises if it does, and stereo if it says nothing:
		 * this library's streams are float32 stereo everywhere else, and inventing 7.1 for a node that
		 * did not answer would be a worse lie than two channels it certainly accepts. */
		sink->channels = 2;
		{
			const char *channels = spa_dict_lookup(props, "audio.channels");

			if (channels != NULL) {
				long value = strtol(channels, NULL, 10);

				if (value > 0 && value <= 64) {
					sink->channels = (UInt32)value;
				}
			}
		}

		sink->rate = 48000;
		{
			const char *rate = spa_dict_lookup(props, "audio.rate");

			if (rate != NULL) {
				long value = strtol(rate, NULL, 10);

				if (value >= 8000 && value <= 384000) {
					sink->rate = (UInt32)value;
				}
			}
		}

		sink->quantum = 512;
		sink->valid = true;
		g_pw.sink_count++;
	}
}

static const struct pw_registry_events kRegistryEvents = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = fn_pw_on_global
};

static void fn_pw_on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)data;
	(void)id;
	(void)seq;
	(void)res;
	(void)message;
}

static void fn_pw_on_core_done(void *data, uint32_t id, int seq)
{
	(void)data;
	(void)id;

	if (seq == g_pw.sync_seq) {
		g_pw.sync_seq = -1;
		pw_thread_loop_signal(g_pw.loop, false);
	}
}

static const struct pw_core_events kCoreEvents = {
	PW_VERSION_CORE_EVENTS,
	.done = fn_pw_on_core_done,
	.error = fn_pw_on_core_error
};

/* The barrier: hold the lock, ask for a `done`, wait until the event with that sequence number arrives. The
 * thread loop delivers events with the lock held, so the wait releases it for the duration. */
static void fn_pw_roundtrip(void)
{
	pw_thread_loop_lock(g_pw.loop);
	g_pw.sync_seq = pw_core_sync(g_pw.core, PW_ID_CORE, 0);
	while (g_pw.sync_seq != -1) {
		pw_thread_loop_wait(g_pw.loop);
	}
	pw_thread_loop_unlock(g_pw.loop);
}

/* One connection for the process, made on the first question and kept: a second connect per property read
 * would be a round trip per `AudioObjectGetPropertyData`, which is not what any client expects. */
static bool fn_pw_connect(void)
{
	if (g_pw.initialised) {
		return g_pw.core != NULL;
	}
	g_pw.initialised = true;

	pw_init(NULL, NULL);

	g_pw.loop = pw_thread_loop_new("agora-audio", NULL);
	if (g_pw.loop == NULL) {
		return false;
	}
	g_pw.context = pw_context_new(pw_thread_loop_get_loop(g_pw.loop), NULL, 0);
	if (g_pw.context == NULL || pw_thread_loop_start(g_pw.loop) != 0) {
		return false;
	}

	pw_thread_loop_lock(g_pw.loop);

	g_pw.core = pw_context_connect(g_pw.context, NULL, 0);
	if (g_pw.core != NULL) {
		pw_core_add_listener(g_pw.core, &g_pw.core_listener, &kCoreEvents, NULL);
		g_pw.registry = pw_core_get_registry(g_pw.core, PW_VERSION_REGISTRY, 0);
	}
	if (g_pw.registry != NULL) {
		pw_registry_add_listener(g_pw.registry, &g_pw.registry_listener, &kRegistryEvents, NULL);
	}

	pw_thread_loop_unlock(g_pw.loop);

	if (g_pw.registry == NULL) {
		return false;
	}

	/* Two round trips: the globals first, then the metadata property values that arrive after the bind. */
	fn_pw_roundtrip();
	fn_pw_roundtrip();

	/* THEN THE TABLE IS FROZEN, which is the only way a synchronous property read and an asynchronous
	 * registry can be the same interface: the listeners are removed, so no sink can be appended while a
	 * client is part-way through the device list. A sink that appears later is invisible until the process
	 * restarts — said out loud in the header comment rather than papered over. */
	spa_hook_remove(&g_pw.registry_listener);
	spa_hook_remove(&g_pw.metadata_listener);

	return true;
}

/* The default sink to the front, so that the HAL's "first output device" *is* the server's default. Order is
 * otherwise the registry's, which is stable for the life of the process. */
static void fn_pw_put_default_first(void)
{
	UInt32 i;

	if (g_pw.default_sink[0] == '\0') {
		return;
	}

	for (i = 1; i < g_pw.sink_count; i++) {
		if (strcmp(g_pw.sinks[i].name, g_pw.default_sink) == 0) {
			pw_sink swap = g_pw.sinks[0];
			UInt32 j;

			g_pw.sinks[0] = g_pw.sinks[i];
			g_pw.sinks[i] = swap;

			/* The ids follow the order the HAL lists them in, so they are renumbered too: an id is how a
			 * client names a device, and it must survive the reordering. */
			for (j = 0; j < g_pw.sink_count; j++) {
				g_pw.sinks[j].id = (AudioDeviceID)(AGORA_PW_DEVICE_BASE + j);
			}
			return;
		}
	}
}

static UInt32 fn_pw_device_count(void)
{
	bool connected;

	pthread_mutex_lock(&g_pw_lock);
	connected = fn_pw_connect();
	if (connected) {
		fn_pw_put_default_first();
	}
	pthread_mutex_unlock(&g_pw_lock);

	return connected ? g_pw.sink_count : 0;
}

static bool fn_pw_device_describe(UInt32 index, agora_device_desc *out)
{
	bool found = false;

	if (out == NULL) {
		return false;
	}

	pthread_mutex_lock(&g_pw_lock);
	if (fn_pw_connect() && index < g_pw.sink_count) {
		const pw_sink *sink = &g_pw.sinks[index];

		out->id = sink->id;
		out->name = sink->description;
		out->uid = sink->name;
		out->manufacturer = "PipeWire";
		out->transport = kAudioDeviceTransportTypeVirtual;
		out->nominal_rate = sink->rate;
		out->buffer_frames = sink->quantum;
		out->channels = sink->channels;
		out->has_input = g_pw.source_count > 0;
		out->has_output = true;
		out->terminal_type = kAudioStreamTerminalTypeSpeaker;
		found = true;
	}
	pthread_mutex_unlock(&g_pw_lock);

	return found;
}

/* ------------------------------------------------------------------------------------------------
 * Streaming: one `pw_stream` per started device.
 * ---------------------------------------------------------------------------------------------- */

static pw_engine *fn_pw_engine(AudioDeviceID id)
{
	UInt32 i;

	for (i = 0; i < AGORA_PW_MAX_STREAMS; i++) {
		if (g_pw.engines[i].running && g_pw.engines[i].device == id) {
			return &g_pw.engines[i];
		}
	}
	return NULL;
}

static const pw_sink *fn_pw_sink(AudioDeviceID id)
{
	UInt32 i;

	for (i = 0; i < g_pw.sink_count; i++) {
		if (g_pw.sinks[i].id == id) {
			return &g_pw.sinks[i];
		}
	}
	return NULL;
}

static void fn_pw_on_process(void *data)
{
	pw_engine *engine = data;
	struct pw_buffer *buffer;

	if ((buffer = pw_stream_dequeue_buffer(engine->stream)) == NULL) {
		return;	/* the server has nothing for us this cycle */
	}

	{
		struct spa_buffer *spa_buffer = buffer->buffer;

		if (spa_buffer->n_datas > 0 && spa_buffer->datas[0].data != NULL) {
			float *samples = spa_buffer->datas[0].data;
			UInt32 stride = engine->channels * (UInt32)sizeof(float);
			UInt32 frames = spa_buffer->datas[0].maxsize / stride;

			if (frames > 4096) {
				frames = 4096;	/* the mixer's slice, and more than the server ever asks for */
			}
			if (frames == 0) {
				pw_stream_queue_buffer(engine->stream, buffer);
				return;
			}

			/* **THE CAPTURE QUANTUM IS USED ONCE, AND ON ITS OWN TERMS.** It was left here by the input stream's
			 * callback; taking it and clearing the count means a source that stalls leaves the next cycle without
			 * input rather than replaying the one before it. The graph negotiates the two quanta separately, so the
			 * input keeps its own length rather than being bent to this one. */
			{
				const float *capture = NULL;
				UInt32 capture_frames = 0;

				if (engine->capture != NULL && engine->captured_frames > 0) {
					capture = engine->capture;
					capture_frames = engine->captured_frames;
					engine->captured_frames = 0;
				}

				fn_agora_device_render(engine->device, capture, capture_frames, samples, frames);
			}

			spa_buffer->datas[0].chunk->offset = 0;
			spa_buffer->datas[0].chunk->stride = (int32_t)stride;
			spa_buffer->datas[0].chunk->size = frames * stride;
		}
	}

	pw_stream_queue_buffer(engine->stream, buffer);
}

static void fn_pw_on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
                                   const char *error)
{
	(void)data;
	(void)old;
	(void)state;
	(void)error;
}

/* The input stream's callback does not mix anything: it copies the quantum the server offered into the engine and
 * hands the buffer straight back, untouched — an input buffer is read, not filled, and its bytes are the server's. */
static void fn_pw_on_capture_process(void *data)
{
	pw_engine *engine = data;
	struct pw_buffer *buffer;

	if ((buffer = pw_stream_dequeue_buffer(engine->capture_stream)) == NULL) {
		return;
	}

	{
		struct spa_buffer *spa_buffer = buffer->buffer;

		if (spa_buffer->n_datas > 0 && spa_buffer->datas[0].data != NULL
		    && spa_buffer->datas[0].chunk != NULL) {
			float *samples = spa_buffer->datas[0].data;
			UInt32 stride = engine->channels * (UInt32)sizeof(float);
			UInt32 frames = stride > 0 ? spa_buffer->datas[0].chunk->size / stride : 0;

			if (frames > engine->capture_capacity) {
				frames = engine->capture_capacity;
			}
			if (frames > 0) {
				memcpy(engine->capture, samples, (size_t)frames * stride);
				engine->captured_frames = frames;
			}
		}
	}

	pw_stream_queue_buffer(engine->capture_stream, buffer);
}

static const struct pw_stream_events kStreamEvents = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = fn_pw_on_state_changed,
	.process = fn_pw_on_process
};

static const struct pw_stream_events kCaptureEvents = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = fn_pw_on_state_changed,
	.process = fn_pw_on_capture_process
};

static bool fn_pw_start(AudioDeviceID id, bool want_input)
{
	bool started = false;

	pthread_mutex_lock(&g_pw_lock);

	if (fn_pw_engine(id) != NULL) {
		pthread_mutex_unlock(&g_pw_lock);
		return true;	/* already playing this device */
	}

	if (fn_pw_connect()) {
		const pw_sink *sink = fn_pw_sink(id);
		pw_engine *engine = NULL;
		UInt32 i;

		for (i = 0; i < AGORA_PW_MAX_STREAMS; i++) {
			if (!g_pw.engines[i].running) {
				engine = &g_pw.engines[i];
				break;
			}
		}

		if (sink != NULL && engine != NULL) {
			struct pw_properties *properties;
			uint8_t pod_buffer[1024];
			struct spa_pod_builder builder;
			struct spa_audio_info_raw info;
			struct spa_pod *parameters[1];
			const struct spa_pod *params[1];

			memset(engine, 0, sizeof(*engine));
			engine->device = id;
			engine->channels = sink->channels;
			engine->rate = sink->rate;

			/* The mixer's slice, and more than the graph ever asks for: a quantum larger than this is clipped
			 * rather than dropped, which is the same bound the playback callback uses. */
			engine->capture_capacity = 4096;
			engine->capture = calloc((size_t)engine->capture_capacity * engine->channels, sizeof(float));
			if (engine->capture == NULL) {
				pthread_mutex_unlock(&g_pw_lock);
				return false;
			}

			properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
			                               PW_KEY_MEDIA_CATEGORY, "Playback",
			                               PW_KEY_MEDIA_ROLE, "Music",
			                               PW_KEY_NODE_NAME, "agora-audio-output",
			                               PW_KEY_NODE_DESCRIPTION, "Agora audio output",
			                               PW_KEY_TARGET_OBJECT, sink->name,
			                               NULL);

			pw_thread_loop_lock(g_pw.loop);

			engine->stream = pw_stream_new(g_pw.core, "agora-audio-output", properties);
			if (engine->stream != NULL) {
					pw_stream_add_listener(engine->stream, &engine->stream_listener, &kStreamEvents, engine);

				memset(&info, 0, sizeof(info));
				info.format = SPA_AUDIO_FORMAT_F32;
				info.rate = sink->rate;
				info.channels = sink->channels;

				spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
				parameters[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
				params[0] = parameters[0];

				/* NOT `PW_STREAM_FLAG_RT_PROCESS`: the mixer takes a lock, and a late buffer is a smaller
				 * problem than a corrupted one. */
				if (pw_stream_connect(engine->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
				                      params, 1) >= 0) {
					engine->running = true;
					started = true;
				} else {
					pw_stream_destroy(engine->stream);
					engine->stream = NULL;
				}
			}

			/* **THE OTHER DIRECTION, ONLY WHEN A CLIENT ASKED FOR IT.** A session server publishes a node per
			 * stream: an input stream that nobody asked for is a microphone held open (and, on some setups, a
			 * permission prompt) for the whole of a playback. It is optional for a second reason too — a session
			 * with no source still plays, and the cycle then goes without input. */
			if (started && want_input && g_pw.source_count > 0) {
				struct pw_properties *input_properties;

				input_properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
				                                     PW_KEY_MEDIA_CATEGORY, "Capture",
				                                     PW_KEY_MEDIA_ROLE, "Production",
				                                     PW_KEY_NODE_NAME, "agora-audio-input",
				                                     PW_KEY_NODE_DESCRIPTION, "Agora audio input",
				                                     NULL);

				engine->capture_stream = pw_stream_new(g_pw.core, "agora-audio-input", input_properties);
				if (engine->capture_stream != NULL) {
					pw_stream_add_listener(engine->capture_stream, &engine->capture_listener, &kCaptureEvents,
					                       engine);

					memset(&info, 0, sizeof(info));
					info.format = SPA_AUDIO_FORMAT_F32;
					info.rate = sink->rate;
					info.channels = sink->channels;

					spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
					parameters[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
					params[0] = parameters[0];

					if (pw_stream_connect(engine->capture_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
					                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
					                      params, 1) < 0) {
						pw_stream_destroy(engine->capture_stream);
						engine->capture_stream = NULL;
					}
				}
			}

			pw_thread_loop_unlock(g_pw.loop);
		}

		/* A start that did not take leaves its slot unreachable (`fn_pw_engine` looks for a *running* engine), so
		 * the buffer it allocated goes back now rather than at the next `memset` over the slot. */
		if (!started && engine != NULL) {
			free(engine->capture);
			engine->capture = NULL;
			engine->capture_capacity = 0;
		}
	}

	pthread_mutex_unlock(&g_pw_lock);
	return started;
}

static void fn_pw_stop(AudioDeviceID id)
{
	pw_engine *engine;

	pthread_mutex_lock(&g_pw_lock);

	engine = fn_pw_engine(id);

	if (engine != NULL) {
		struct pw_stream *stream = engine->stream;
		struct pw_stream *capture_stream = engine->capture_stream;

		engine->stream = NULL;
		engine->capture_stream = NULL;
		engine->captured_frames = 0;
		engine->running = false;

		if (stream != NULL || capture_stream != NULL) {
			pw_thread_loop_lock(g_pw.loop);
			if (capture_stream != NULL) {
				pw_stream_disconnect(capture_stream);
				pw_stream_destroy(capture_stream);
			}
			if (stream != NULL) {
				pw_stream_disconnect(stream);
				pw_stream_destroy(stream);
			}
			pw_thread_loop_unlock(g_pw.loop);
		}

		free(engine->capture);
		engine->capture = NULL;
		engine->capture_capacity = 0;
	}

	pthread_mutex_unlock(&g_pw_lock);
}

static const agora_driver kPipeWireDriver = {
	"pipewire",
	fn_pw_device_count,
	fn_pw_device_describe,
	fn_pw_start,
	fn_pw_stop
};

#endif /* AGORA_WITH_PIPEWIRE */

const agora_driver *fn_agora_driver_pipewire(void)
{
#if defined(AGORA_WITH_PIPEWIRE)
	return &kPipeWireDriver;
#else
	return NULL;
#endif
}
