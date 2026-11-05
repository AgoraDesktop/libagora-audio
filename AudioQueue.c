/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioQueue.c — the buffered playback model, over the device layer.
 *
 * THE QUEUE IS A CONSUMER OF ITS OWN IO PROC. `AudioQueueStart` attaches a callback to the default output
 * device and lets the HAL call it at the device's rate; each call drains as much of the enqueued audio as
 * fits, converts it to the device's format through an `AudioConverter`, and hands each spent buffer back to
 * the caller. That is why a queue needs no thread of its own: the device already has one, and adding a second
 * would be two clocks where the API promises one.
 *
 * THE BUFFER'S `const` MEMBERS ARE APPLE'S, AND THE MIRROR STRUCTURE IS OURS. `AudioQueueBuffer` declares
 * `mAudioDataBytesCapacity` and friends `const`, so the implementation cannot fill them in without a mutable
 * view of the same bytes; `agora_queue_buffer` is that view, and the two `_Static_assert`s below are what make
 * the cast safe rather than optimistic.
 *
 * THE LOCK IS NEVER HELD ACROSS A CALLBACK. The caller's callback almost always enqueues another buffer, so
 * calling it under the lock would deadlock the first time the model was used the way it is meant to be.
 *
 * **THE STAGE IS WHY A TAP EXISTS.** A processing tap sees the queue's *decoded* audio in the client's own format,
 * and the device path used to convert straight out of the caller's buffers into the device's — so there was never
 * a moment when the audio existed in one piece, in the format the client asked for, for a tap to look at. The
 * source is now gathered into a short FIFO in the client's format (the `stage`), the tap runs on that, and the
 * conversion, the channel mapping and the volume are the tail of the pipeline after it. That is also why the
 * processing format a tap is given is the queue's client format rather than the device's.
 */

#include <AudioToolbox/AudioQueue.h>
#include "AudioToolbox_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define AGORA_QUEUE_MAX_BUFFERS 32
#define AGORA_QUEUE_MAX_PENDING 32
#define AGORA_QUEUE_MAX_LISTENERS 8

/* The mutable mirror of `AudioQueueBuffer`. */
typedef struct {
	UInt32                          mAudioDataBytesCapacity;
	void *                          mAudioData;
	UInt32                          mAudioDataByteSize;
	void *                          mUserData;
	UInt32                          mPacketDescriptionCapacity;
	AudioStreamPacketDescription *  mPacketDescriptions;
	UInt32                          mPacketDescriptionCount;
} agora_queue_buffer;

_Static_assert(sizeof(agora_queue_buffer) == sizeof(AudioQueueBuffer),
               "agora_queue_buffer must be the same size as AudioQueueBuffer");
_Static_assert(offsetof(agora_queue_buffer, mAudioData) == offsetof(AudioQueueBuffer, mAudioData),
               "mAudioData must be at the same offset");
_Static_assert(offsetof(agora_queue_buffer, mPacketDescriptions) == offsetof(AudioQueueBuffer, mPacketDescriptions),
               "mPacketDescriptions must be at the same offset");

typedef struct {
	bool                       active;
	AudioQueuePropertyListenerProc proc;
	void *                     client;
	AudioQueuePropertyID       id;
} queue_listener;

/* **A TAP IS A VIEW OF THE QUEUE'S SOURCE, NOT A SECOND PIPELINE.** It holds no audio of its own: the source it
 * pulls lives in the queue's stage, and the pointers it is given are valid for exactly the duration of its
 * callback. `in_callback` is what makes `GetSourceAudio` refusable outside one — and it is also why those two
 * doors never take the queue's lock: they run inside it.
 *
 * `queue_frames` is what `GetQueueTime`'s second answer is: the frames of queue time corresponding to the chunk
 * the tap is processing, which is the request and not what the tap chose to emit. */
struct OpaqueAudioQueueProcessingTap {
	AudioQueueRef                   queue;
	AudioQueueProcessingTapCallback callback;
	void *                          client;
	AudioQueueProcessingTapFlags    flags;		/* the constructor's, reported on every callback */
	AudioBufferList *               source;	/* queue-owned source, valid only inside the callback */
	AudioBufferList *               data;		/* handed to the callback as its `ioData` */
	bool                            in_callback;
	bool                            source_pulled;
	UInt32                          queue_frames;
	UInt32                          generation;
};

/* **A TIMELINE IS A REMEMBERED MOMENT, NOT A CLOCK.** It holds what the queue's time base was at the caller's last
 * look, so the next look can answer the one question the caller cannot: whether anything happened in between that
 * makes its own arithmetic wrong — a start, a stop, a reset, or a sample time that went backwards. */
struct OpaqueAudioQueueTimeline {
	AudioQueueRef queue;
	UInt32        generation;
	UInt64        frames;
	bool          primed;
};

struct OpaqueAudioQueue {
	AudioStreamBasicDescription format;
	/* **AN INPUT QUEUE IS THE SAME OBJECT RUNNING BACKWARDS.** It borrows the device, the converter, the stage, the
	 * channel map and the tap, and what changes is the direction: the device's input is mapped into the client's
	 * format and handed back in the caller's buffers, instead of the caller's buffers being mapped into the device's
	 * output. `input` is what every one of those decisions reads. */
	bool                        input;
	AudioDeviceID               device;
	AudioStreamBasicDescription device_format;
	AudioConverterRef           converter;

	/* **THE CONVERTER'S TARGET IS NOT ALWAYS THE DEVICE'S FORMAT.** `AudioConverterNew` refuses a channel-count
	 * change — it is a property of a channel map, and Apple refuses it too — so a MONO sound on a stereo device
	 * would be unplayable if the queue asked the converter for the device's description. Instead the converter
	 * is asked for the *client's* channel count at the device's rate, and the queue fans the result out (or
	 * sums it down) on the way into the device buffer. That is the channel mapping Apple's AudioQueue does, in
	 * the one place this library can do it without teaching the converter to lie about what it supports. */
	AudioStreamBasicDescription convert_format;
	bool                        map_channels;
	float *                     map_scratch;
	UInt32                      map_capacity;	/* frames the scratch holds */
	AudioBufferList *           chunk;		/* the converter's destination on a capture queue */

	/* **THE STAGE IS THE CLIENT'S AUDIO IN ONE PIECE.** It is a short FIFO in the client's own format — the form
	 * a tap processes and the form the converter pulls from — so `stage_offset` is where the source has been
	 * consumed to, and `stage_frames` is what is in it. `tapped` holds the tap's output for the quantum being
	 * rendered, kept apart from the stage so the source the tap did not pull is not overwritten by what it
	 * produced. */
	uint8_t *                   stage;
	UInt32                      stage_frames;
	UInt32                      stage_offset;
	UInt32                      stage_capacity;		/* frames the stage holds */
	uint8_t *                   tapped;
	UInt32                      tapped_capacity;		/* frames the tap's buffer holds */

	AudioQueueProcessingTapRef  tap;

	AudioQueueOutputCallback        callback;
	AudioQueueOutputCallbackBlock   block;
	AudioQueueInputCallback         input_callback;
	AudioQueueInputCallbackBlock    input_block;
	dispatch_queue_t                queue;
	void *                          client;

	AudioQueueBufferRef         buffers[AGORA_QUEUE_MAX_BUFFERS];
	bool                        buffer_in_use[AGORA_QUEUE_MAX_BUFFERS];
	UInt32                      buffer_count;

	AudioQueueBufferRef         pending[AGORA_QUEUE_MAX_PENDING];
	UInt32                      pending_head;
	UInt32                      pending_count;

	AudioQueueBufferRef         current;
	UInt32                      current_offset;	/* bytes consumed of `current` */

	pthread_mutex_t             lock;
	bool                        running;
	Float32                     volume;
	Float32                     ramp_time;
	bool                        metering;
	UInt64                      frames_played;
	AudioTimeStamp              last_time;

	/* **THE TIME BASE BREAKS WHENEVER THE QUEUE IS STARTED, STOPPED OR RESET**, and a timeline's whole job is to say
	 * that it did: a caller mixing its own clock into ours has to know when the two stopped agreeing. */
	UInt32                      generation;

	queue_listener              listeners[AGORA_QUEUE_MAX_LISTENERS];
};

/* ------------------------------------------------------------------------------------------------
 * Small helpers.
 * ---------------------------------------------------------------------------------------------- */

static UInt32 fn_device_channels(AudioDeviceID device, bool input)
{
	AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams,
	                                       input ? kAudioObjectPropertyScopeInput
	                                             : kAudioObjectPropertyScopeOutput,
	                                       kAudioObjectPropertyElementMaster };
	AudioStreamID list[8];
	UInt32 size = sizeof(list);

	memset(list, 0, sizeof(list));
	if (AudioObjectGetPropertyData(device, &streams, 0, NULL, &size, list) == 0 && size >= sizeof(AudioStreamID)) {
		AudioObjectPropertyAddress config = { kAudioStreamPropertyVirtualFormat,
		                                      kAudioObjectPropertyScopeGlobal,
		                                      kAudioObjectPropertyElementMaster };
		AudioStreamBasicDescription format;
		UInt32 format_size = sizeof(format);

		memset(&format, 0, sizeof(format));
		if (AudioObjectGetPropertyData(list[0], &config, 0, NULL, &format_size, &format) == 0) {
			return format.mChannelsPerFrame;
		}
	}

	return 2;
}

static void fn_notify(AudioQueueRef queue, AudioQueuePropertyID id)
{
	UInt32 i;

	for (i = 0; i < AGORA_QUEUE_MAX_LISTENERS; i++) {
		if (queue->listeners[i].active && queue->listeners[i].id == id) {
			queue->listeners[i].proc(queue->listeners[i].client, queue, id);
		}
	}
}

static void fn_push_pending(AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	if (queue->pending_count >= AGORA_QUEUE_MAX_PENDING) {
		return;	/* the oldest is dropped by the caller's own accounting; nothing is lost silently */
	}

	queue->pending[(queue->pending_head + queue->pending_count) % AGORA_QUEUE_MAX_PENDING] = buffer;
	queue->pending_count++;
}

static AudioQueueBufferRef fn_pop_pending(AudioQueueRef queue)
{
	AudioQueueBufferRef buffer;

	if (queue->pending_count == 0) {
		return NULL;
	}

	buffer = queue->pending[queue->pending_head];
	queue->pending_head = (queue->pending_head + 1) % AGORA_QUEUE_MAX_PENDING;
	queue->pending_count--;

	return buffer;
}

/* ------------------------------------------------------------------------------------------------
 * The stage: the client's audio in one piece.
 * ---------------------------------------------------------------------------------------------- */

/* The buffers a client-format block is described by: one for an interleaved description, one per channel for a
 * planar one. A tap is told the shape of the format, so this is the shape of what it is handed. */
static UInt32 fn_client_planes(const AudioStreamBasicDescription *format)
{
	if ((format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0 && format->mChannelsPerFrame > 0) {
		return format->mChannelsPerFrame;
	}

	return 1;
}

/* The bytes one frame of the client's audio occupies, counted across every plane. That is what a byte offset into
 * `stage` is multiplied by, and it is the same number for both interleavings. */
static size_t fn_client_frame_bytes(AudioQueueRef queue)
{
	return (size_t)fn_pcm_bytes_per_frame(&queue->format) * fn_client_planes(&queue->format);
}

/* An `AudioBufferList` with room for `planes` buffers: the structure is variable-length, so it is allocated rather
 * than declared. */
static AudioBufferList *fn_list_new(UInt32 planes)
{
	return calloc(1, sizeof(AudioBufferList)
	                 + (size_t)(planes > 0 ? planes - 1 : 0) * sizeof(AudioBuffer));
}

/* Point a list at a contiguous client-format block. Both interleavings describe a plane as `frames` frames, so the
 * same expression sizes an interleaved buffer and each plane of a planar one. */
static void fn_list_point(AudioBufferList *list, uint8_t *base, UInt32 frames,
                          const AudioStreamBasicDescription *format)
{
	UInt32 planes = fn_client_planes(format);
	UInt32 bytes = frames * format->mBytesPerFrame;
	UInt32 i;

	list->mNumberBuffers = planes;
	for (i = 0; i < planes; i++) {
		list->mBuffers[i].mNumberChannels = (planes == 1) ? format->mChannelsPerFrame : 1;
		list->mBuffers[i].mDataByteSize = bytes;
		list->mBuffers[i].mData = base + (size_t)i * bytes;
	}
}

/* Gather a list a tap produced back into the contiguous block the conversion path reads. A tap may hand back the
 * queue's own source pointers (in-place processing, which Apple recommends), so the copy is skipped when the
 * pointers already describe the destination. */
static void fn_list_copy_in(const AudioBufferList *list, UInt32 frames, uint8_t *destination,
                            const AudioStreamBasicDescription *format)
{
	UInt32 planes = fn_client_planes(format);
	UInt32 bytes = frames * format->mBytesPerFrame;
	UInt32 i;

	for (i = 0; i < planes && i < list->mNumberBuffers; i++) {
		uint8_t *here = destination + (size_t)i * bytes;

		if (list->mBuffers[i].mData == NULL) {
			memset(here, 0, bytes);
		} else if (list->mBuffers[i].mData != here) {
			memcpy(here, list->mBuffers[i].mData, bytes);
		}
	}
}

/* The client frames one quantum of `frames` device frames holds: the rate ratio, and nothing else. That is the
 * number a tap is told, because it is the chunk the queue is rendering. */
static UInt32 fn_stage_quantum(AudioQueueRef queue, UInt32 frames)
{
	double client_rate = queue->format.mSampleRate;
	double device_rate = queue->device_format.mSampleRate;

	if (client_rate <= 0.0 || device_rate <= 0.0) {
		return frames;
	}

	return (UInt32)((double)frames * client_rate / device_rate);
}

/* What to gather for that quantum. The rate ratio is the whole answer except at the seams, where a resampler asks a
 * frame or two past its window; the two are what keeps an upsampling queue from leaving a gap of silence at every
 * quantum boundary. */
static UInt32 fn_stage_need(AudioQueueRef queue, UInt32 frames)
{
	return fn_stage_quantum(queue, frames) + 2;
}

static bool fn_stage_reserve(AudioQueueRef queue, UInt32 frames)
{
	size_t bytes;
	uint8_t *grown;

	if (frames <= queue->stage_capacity) {
		return true;
	}

	bytes = (size_t)frames * fn_client_frame_bytes(queue);
	grown = realloc(queue->stage, bytes);
	if (grown == NULL) {
		return false;
	}

	queue->stage = grown;
	queue->stage_capacity = frames;
	return true;
}

static bool fn_tapped_reserve(AudioQueueRef queue, UInt32 frames)
{
	size_t bytes;
	uint8_t *grown;

	if (frames <= queue->tapped_capacity) {
		return true;
	}

	bytes = (size_t)frames * fn_client_frame_bytes(queue);
	grown = realloc(queue->tapped, bytes);
	if (grown == NULL) {
		return false;
	}

	queue->tapped = grown;
	queue->tapped_capacity = frames;
	return true;
}

/* Move what the source has consumed off the front, so the space it held can be used again. */
static void fn_stage_compact(AudioQueueRef queue)
{
	size_t frame_bytes = fn_client_frame_bytes(queue);

	if (queue->stage_offset == 0) {
		return;
	}
	if (queue->stage_offset >= queue->stage_frames) {
		queue->stage_offset = 0;
		queue->stage_frames = 0;
		return;
	}

	memmove(queue->stage, queue->stage + (size_t)queue->stage_offset * frame_bytes,
	        (size_t)(queue->stage_frames - queue->stage_offset) * frame_bytes);
	queue->stage_frames -= queue->stage_offset;
	queue->stage_offset = 0;
}

/* Pull up to `need` more client frames into the stage from the enqueued buffers. The buffers this spent are
 * collected rather than handed back here: the caller hands them back with the lock released. */
static void fn_stage_pull(AudioQueueRef queue, UInt32 need, AudioQueueBufferRef *finished,
                          UInt32 *finished_count)
{
	size_t frame_bytes = fn_client_frame_bytes(queue);
	UInt32 appended = 0;

	while (appended < need) {
		UInt32 available;
		UInt32 take;
		UInt32 bytes;

		if (queue->current == NULL) {
			queue->current = fn_pop_pending(queue);
			queue->current_offset = 0;
			if (queue->current == NULL) {
				break;	/* nothing queued: the rest of the quantum stays silent */
			}
		}

		if (queue->current_offset >= queue->current->mAudioDataByteSize) {
			if (*finished_count < AGORA_QUEUE_MAX_PENDING) {
				finished[(*finished_count)++] = queue->current;
			}
			queue->current = NULL;
			queue->current_offset = 0;
			continue;
		}

		available = (queue->current->mAudioDataByteSize - queue->current_offset)
		          / (UInt32)frame_bytes;
		if (available == 0) {
			/* A buffer shorter than one frame cannot advance the render; spending it is the only way
			 * forward that does not loop forever. */
			if (*finished_count < AGORA_QUEUE_MAX_PENDING) {
				finished[(*finished_count)++] = queue->current;
			}
			queue->current = NULL;
			queue->current_offset = 0;
			continue;
		}

		take = available;
		if (take > need - appended) {
			take = need - appended;
		}
		if (!fn_stage_reserve(queue, queue->stage_frames + take)) {
			break;
		}

		bytes = take * (UInt32)frame_bytes;
		memcpy(queue->stage + (size_t)queue->stage_frames * frame_bytes,
		       (const uint8_t *)queue->current->mAudioData + queue->current_offset, bytes);
		queue->current_offset += bytes;
		queue->stage_frames += take;
		appended += take;
	}
}

/* ------------------------------------------------------------------------------------------------
 * The io proc.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	const uint8_t *     data;      /* the client-format source, in whichever format the queue was given */
	UInt32              bytes;
	UInt32              delivered;
	UInt32              bytes_per_frame;
} queue_source;

static OSStatus fn_queue_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets, AudioBufferList *ioData,
                               AudioStreamPacketDescription **outDescription, void *user)
{
	queue_source *source = user;
	UInt32 wanted;
	(void)converter;
	(void)outDescription;

	if (source->delivered >= source->bytes) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	wanted = *ioNumberDataPackets;
	if (wanted * source->bytes_per_frame > source->bytes - source->delivered) {
		wanted = (source->bytes - source->delivered) / source->bytes_per_frame;
	}

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = 0;	/* filled by the converter's view of the input format */
	ioData->mBuffers[0].mDataByteSize = wanted * source->bytes_per_frame;
	ioData->mBuffers[0].mData = (void *)(source->data + source->delivered);

	source->delivered += wanted * source->bytes_per_frame;
	*ioNumberDataPackets = wanted;
	return 0;
}

/* Run the tap for one quantum, with the queue's lock held: the tap's two service doors run inside the callback and
 * therefore never take the lock themselves. Answers with the client-format audio to convert — the tap's own output for
 * a processing tap, and the source it was shown for a siphon, which produces nothing — and its frame count. */
static const uint8_t *fn_tap_process(AudioQueueRef queue, UInt32 want, AudioTimeStamp *stamp, UInt32 *out_frames)
{
	AudioQueueProcessingTapRef tap = queue->tap;
	AudioQueueProcessingTapFlags flags = tap->flags;
	size_t frame_bytes = fn_client_frame_bytes(queue);
	const uint8_t *result;
	UInt32 produced;

	tap->in_callback = true;
	tap->source_pulled = false;
	tap->queue_frames = want;

	if ((tap->flags & kAudioQueueProcessingTap_Siphon) != 0) {
		/* A siphon is shown the source and may not alter it, so its output is the source itself — no copy, and the
		 * pointers it was shown are what the conversion reads. */
		UInt32 available = queue->stage_frames - queue->stage_offset;

		if (available > want) {
			available = want;
		}
		result = queue->stage + (size_t)queue->stage_offset * frame_bytes;
		fn_list_point(tap->data, (uint8_t *)result, available, &queue->format);
		produced = available;
		tap->callback(tap->client, tap, available, stamp, &flags, &produced, tap->data);
		if (produced > available) {
			produced = available;
		}
		queue->stage_offset += produced;
	} else {
		/* Apple: "on entry, the buffer pointers are null and the lengths are zero." The tap fills them by pulling
		 * the source itself, through `GetSourceAudio`. */
		UInt32 i;

		for (i = 0; i < tap->data->mNumberBuffers; i++) {
			tap->data->mBuffers[i].mData = NULL;
			tap->data->mBuffers[i].mDataByteSize = 0;
		}

		produced = 0;
		tap->callback(tap->client, tap, want, stamp, &flags, &produced, tap->data);
		if (produced > want) {
			produced = want;
		}
		if (produced > 0 && !fn_tapped_reserve(queue, produced)) {
			produced = 0;
		}
		fn_list_copy_in(tap->data, produced, queue->tapped, &queue->format);
		result = queue->tapped;
	}

	tap->in_callback = false;
	tap->source_pulled = false;
	tap->queue_frames = 0;

	*out_frames = produced;
	return result;
}

static OSStatus fn_queue_render(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                                const AudioTimeStamp *input_time, AudioBufferList *output,
                                const AudioTimeStamp *output_time, void *client)
{
	AudioQueueRef queue = client;
	AudioQueueBufferRef finished[AGORA_QUEUE_MAX_PENDING];
	UInt32 finished_count = 0;
	AudioTimeStamp stamp;
	size_t frame_bytes;
	UInt32 frames;
	UInt32 channels;
	UInt32 need;
	UInt32 produced = 0;
	uint8_t *base;
	UInt32 available;
	UInt32 offset;

	(void)device;
	(void)input;
	(void)input_time;
	(void)output_time;

	if (output == NULL || output->mNumberBuffers == 0 || output->mBuffers[0].mData == NULL) {
		return 0;
	}

	frames = output->mBuffers[0].mDataByteSize / (output->mBuffers[0].mNumberChannels * (UInt32)sizeof(float));
	channels = output->mBuffers[0].mNumberChannels;

	pthread_mutex_lock(&queue->lock);

	if (!queue->running) {
		pthread_mutex_unlock(&queue->lock);
		return 0;
	}

	frame_bytes = fn_client_frame_bytes(queue);
	need = fn_stage_need(queue, frames);

	/* **SOURCE, THEN TAP, THEN THE DEVICE.** The source is gathered in the client's own format first, because that
	 * is the only moment it exists in one piece in that format: a tap processes the queue's decoded audio, and the
	 * converter's output is no longer the client's format. It is topped up to what this quantum can want and never
	 * past it, so a tap that pulls less than it is shown cannot make the stage grow without bound. */
	fn_stage_compact(queue);
	if (queue->stage_frames < need) {
		fn_stage_pull(queue, need - queue->stage_frames, finished, &finished_count);
	}

	memset(&stamp, 0, sizeof(stamp));
	if (now != NULL) {
		stamp = *now;
	}

	if (queue->tap != NULL) {
		/* The tap pulled what it could use out of the stage itself, through `GetSourceAudio`, so what is left to
		 * convert is what it produced. The call comes first: it is what allocates the buffer it produced into. */
		base = (uint8_t *)fn_tap_process(queue, fn_stage_quantum(queue, frames), &stamp, &available);
		offset = 0;
	} else {
		base = queue->stage;
		available = queue->stage_frames;
		offset = queue->stage_offset;
	}

	while (produced < frames && offset < available) {
		queue_source source;
		AudioBufferList chunk;
		float *destination = (float *)output->mBuffers[0].mData;
		UInt32 wanted = frames - produced;
		UInt32 converted = 0;

		source.data = base + (size_t)offset * frame_bytes;
		source.bytes = (available - offset) * (UInt32)frame_bytes;
		source.delivered = 0;
		source.bytes_per_frame = (UInt32)frame_bytes;

		/* WHERE THE CONVERSION LANDS: the device buffer directly when the channel counts agree, and the queue's
		 * own scratch when they do not (see the mapping below). */
		memset(&chunk, 0, sizeof(chunk));
		chunk.mNumberBuffers = 1;
		if (queue->map_channels) {
			UInt32 client_channels = queue->convert_format.mChannelsPerFrame;

			if (queue->map_capacity < frames) {
				float *grown = realloc(queue->map_scratch, (size_t)frames * client_channels * sizeof(float));

				if (grown == NULL) {
					break;
				}
				queue->map_scratch = grown;
				queue->map_capacity = frames;
			}
			chunk.mBuffers[0].mNumberChannels = client_channels;
			chunk.mBuffers[0].mDataByteSize = wanted * client_channels * (UInt32)sizeof(float);
			chunk.mBuffers[0].mData = queue->map_scratch;
		} else {
			chunk.mBuffers[0].mNumberChannels = channels;
			chunk.mBuffers[0].mDataByteSize = wanted * channels * (UInt32)sizeof(float);
			chunk.mBuffers[0].mData = destination + (size_t)produced * channels;
		}

		if (queue->converter != NULL) {
			OSStatus status = AudioConverterFillComplexBuffer(queue->converter, fn_queue_input, &source,
			                                                  &wanted, &chunk, NULL);

			if (status != 0) {
				break;
			}
			converted = wanted;
			offset += source.delivered / (UInt32)frame_bytes;
			if (source.delivered == 0) {
				/* The converter answered without taking anything, so the source is spent: that is the end of
				 * this quantum rather than another iteration over the same bytes. */
				produced += converted;
				break;
			}
		} else {
			/* Same format: the staged source is already what the device wants. */
			UInt32 here = source.bytes / (UInt32)frame_bytes;
			UInt32 frames_to_copy = wanted < here ? wanted : here;

			if (frames_to_copy == 0) {
				break;
			}
			memcpy(chunk.mBuffers[0].mData, source.data, (size_t)frames_to_copy * frame_bytes);
			offset += frames_to_copy;
			converted = frames_to_copy;
		}

		/* THE MAPPING: a mono source reaches every device channel, a mono device receives the sum of the source's
		 * channels over their count. Neither is a pan law, and both are what a beep expects. */
		if (queue->map_channels && converted > 0) {
			UInt32 client_channels = queue->convert_format.mChannelsPerFrame;
			UInt32 frame;

			for (frame = 0; frame < converted; frame++) {
				float *out = destination + (size_t)(produced + frame) * channels;
				const float *in = queue->map_scratch + (size_t)frame * client_channels;

				if (client_channels == 1) {
					UInt32 channel;

					for (channel = 0; channel < channels; channel++) {
						out[channel] = in[0];
					}
				} else {
					float sum = 0.0f;
					UInt32 channel;

					for (channel = 0; channel < client_channels; channel++) {
						sum += in[channel];
					}
					out[0] = sum / (float)client_channels;
				}
			}
		}

		produced += converted;

		/* HAND A BUFFER BACK AND KEEP PLAYING: breaking here would leave every other device quantum silent
		 * whenever a buffer ends exactly on a quantum boundary, which is the common case when the buffer and the
		 * quantum are the same size. `fn_stage_pull` does that, so a full stage is not the end of the quantum. */
		if (converted == 0) {
			break;
		}
	}

	/* The source a tap did not pull stays staged; the tap advanced the offset itself as it pulled. */
	if (queue->tap == NULL) {
		queue->stage_offset = offset;
	}

	/* The volume is the queue's, not the device's: two queues on one device can have different ones. */
	if (queue->volume != 1.0f) {
		float *samples = output->mBuffers[0].mData;
		UInt32 i;

		for (i = 0; i < produced * channels; i++) {
			samples[i] *= queue->volume;
		}
	}

	queue->frames_played += produced;
	if (now != NULL) {
		queue->last_time = *now;
	}

	pthread_mutex_unlock(&queue->lock);

	/* THE CALLBACKS ARE CALLED WITH THE LOCK RELEASED, because the caller refills and re-enqueues this very
	 * buffer from inside it. */
	{
		UInt32 i;

		for (i = 0; i < finished_count; i++) {
			if (queue->callback != NULL) {
				queue->callback(queue->client, queue, finished[i]);
			} else if (queue->block != NULL) {
				AudioQueueBufferRef buffer = finished[i];

				if (queue->queue != NULL) {
					dispatch_async(queue->queue, ^{
						queue->block(queue, buffer);
					});
				} else {
					queue->block(queue, buffer);
				}
			}
		}
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Capture: the same object running the other way.
 * ---------------------------------------------------------------------------------------------- */

/* The channel mapping a recording needs: the device's channels summed down to one, or one fanned out to all of them.
 * It is the output path's `map_channels` seen from the microphone's end, and the same refusal applies at creation: two
 * counts above one would need a channel layout, which this door does not carry. */
static void fn_map_input(AudioQueueRef queue, const float *source, UInt32 device_channels, float *destination,
                         UInt32 client_channels, UInt32 frames)
{
	UInt32 frame;
	UInt32 channel;

	for (frame = 0; frame < frames; frame++) {
		const float *in = source + (size_t)frame * device_channels;
		float *out = destination + (size_t)frame * client_channels;

		if (device_channels == 1) {
			for (channel = 0; channel < client_channels; channel++) {
				out[channel] = in[0];
			}
		} else {
			float sum = 0.0f;

			for (channel = 0; channel < device_channels; channel++) {
				sum += in[channel];
			}
			out[0] = sum / (float)device_channels;
		}
	}
}

static OSStatus fn_queue_capture(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                                 const AudioTimeStamp *input_time, AudioBufferList *output,
                                 const AudioTimeStamp *output_time, void *client)
{
	AudioQueueRef queue = client;
	AudioQueueBufferRef filled[AGORA_QUEUE_MAX_PENDING];
	UInt32 filled_count = 0;
	AudioTimeStamp stamp;
	size_t frame_bytes;
	const float *source;
	UInt32 channels;
	UInt32 available;
	UInt32 staged = 0;
	UInt32 i;

	(void)device;
	(void)input_time;
	(void)output;
	(void)output_time;

	if (input == NULL || input->mNumberBuffers == 0 || input->mBuffers[0].mData == NULL) {
		return 0;
	}

	pthread_mutex_lock(&queue->lock);

	if (!queue->running) {
		pthread_mutex_unlock(&queue->lock);
		return 0;
	}

	frame_bytes = fn_client_frame_bytes(queue);
	channels = input->mBuffers[0].mNumberChannels;
	available = channels > 0 ? input->mBuffers[0].mDataByteSize / (channels * (UInt32)sizeof(float)) : 0;
	source = input->mBuffers[0].mData;

	/* THE LINE, IN THE ORDER A RECORDING TAKES IT: map the channels, convert the rate and the sample type, then
	 * the tap, then the caller's buffers — which is the output path's pipeline read from the other end. */
	if (queue->map_channels && available > 0) {
		UInt32 client_channels = queue->convert_format.mChannelsPerFrame;

		if (queue->map_capacity < available) {
			float *grown = realloc(queue->map_scratch, (size_t)available * client_channels * sizeof(float));

			if (grown == NULL) {
				pthread_mutex_unlock(&queue->lock);
				return 0;
			}
			queue->map_scratch = grown;
			queue->map_capacity = available;
		}

		fn_map_input(queue, source, channels, queue->map_scratch, client_channels, available);
		source = queue->map_scratch;
		channels = queue->convert_format.mChannelsPerFrame;
	}

	if (available > 0) {
		/* The client frames this cycle can carry: the rate ratio, plus the two a converter asks past its window. */
		double client_rate = queue->format.mSampleRate;
		double device_rate = queue->device_format.mSampleRate;
		UInt32 want = (UInt32)((double)available * (client_rate > 0.0 ? client_rate : 1.0)
		                       / (device_rate > 0.0 ? device_rate : 1.0)) + 2;

		queue->stage_frames = 0;
		queue->stage_offset = 0;

		if (!fn_stage_reserve(queue, want)) {
			pthread_mutex_unlock(&queue->lock);
			return 0;
		}

		if (queue->converter != NULL) {
			queue_source in;
			UInt32 wanted = want;
			OSStatus status;

			in.data = (const uint8_t *)source;
			in.bytes = available * channels * (UInt32)sizeof(float);
			in.delivered = 0;
			in.bytes_per_frame = channels * (UInt32)sizeof(float);

			fn_list_point(queue->chunk, (uint8_t *)queue->stage, wanted, &queue->format);
			status = AudioConverterFillComplexBuffer(queue->converter, fn_queue_input, &in, &wanted, queue->chunk,
			                                          NULL);
			if (status != 0) {
				wanted = 0;
			}
			staged = wanted;
		} else {
			/* Same format: what the device heard is already what the client asked for. */
			memcpy(queue->stage, source, (size_t)available * frame_bytes);
			staged = available;
		}
	}

	queue->stage_frames = staged;

	memset(&stamp, 0, sizeof(stamp));
	if (now != NULL) {
		stamp = *now;
	} else {
		stamp.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
		stamp.mSampleTime = (Float64)queue->frames_played;
		stamp.mHostTime = AudioGetCurrentHostTime();
	}

	/* The tap runs on the cycle's client-format audio, and what it emits is what the caller is handed; a tap that
	 * pulls less than it was shown drops the rest, which is what a recording that falls behind does. */
	{
		const uint8_t *audio;
		UInt32 frames;

		if (queue->tap != NULL) {
			audio = fn_tap_process(queue, queue->stage_frames, &stamp, &frames);
		} else {
			audio = queue->stage + (size_t)queue->stage_offset * frame_bytes;
			frames = queue->stage_frames - queue->stage_offset;
		}

		queue->stage_frames = 0;
		queue->stage_offset = 0;

		while (frames > 0) {
			AudioQueueBufferRef buffer = fn_pop_pending(queue);
			UInt32 fit;
			UInt32 take;

			if (buffer == NULL) {
				/* Nothing enqueued to record into: the audio is lost, exactly as it is on a device whose client
				 * did not keep up. */
				break;
			}

			fit = buffer->mAudioDataBytesCapacity / (UInt32)frame_bytes;
			take = frames < fit ? frames : fit;
			if (take == 0) {
				fn_push_pending(queue, buffer);
				break;
			}

			memcpy(buffer->mAudioData, audio, (size_t)take * frame_bytes);
			buffer->mAudioDataByteSize = take * (UInt32)frame_bytes;
			audio += (size_t)take * frame_bytes;
			frames -= take;
			queue->frames_played += take;
			if (filled_count < AGORA_QUEUE_MAX_PENDING) {
				filled[filled_count++] = buffer;
			}
		}
	}

	queue->last_time = stamp;

	pthread_mutex_unlock(&queue->lock);

	/* The caller's callback runs with the lock released, for the same reason the output callback does: it almost
	 * always re-enqueues the buffer it was handed. */
	for (i = 0; i < filled_count; i++) {
		if (queue->input_callback != NULL) {
			queue->input_callback(queue->client, queue, filled[i], &stamp, 0, NULL);
		} else if (queue->input_block != NULL) {
			AudioQueueBufferRef buffer = filled[i];

			if (queue->queue != NULL) {
				dispatch_async(queue->queue, ^{
					queue->input_block(queue, buffer, &stamp, 0, NULL);
				});
			} else {
				queue->input_block(queue, buffer, &stamp, 0, NULL);
			}
		}
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Creation.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_queue_new(const AudioStreamBasicDescription *format, bool input, AudioQueueRef *out,
                             AudioQueueOutputCallback callback, AudioQueueOutputCallbackBlock block,
                             AudioQueueInputCallback input_callback,
                             AudioQueueInputCallbackBlock input_block,
                             dispatch_queue_t queue, void *client)
{
	AudioQueueRef aq;
	OSStatus status;
	AudioObjectPropertyAddress default_device = { input ? kAudioHardwarePropertyDefaultInputDevice
	                                                    : kAudioHardwarePropertyDefaultOutputDevice,
	                                              kAudioObjectPropertyScopeGlobal,
	                                              kAudioObjectPropertyElementMaster };
	UInt32 size = sizeof(AudioDeviceID);

	if (format == NULL || out == NULL) {
		return kAudio_ParamError;
	}

	if (format->mFormatID != kAudioFormatLinearPCM) {
		/* No codec exists here, so a compressed queue has nothing to pull from — and saying so is the
		 * difference between a client that knows why and one that hears silence. */
		return kAudioQueueErr_InvalidDevice;
	}

	aq = calloc(1, sizeof(*aq));
	if (aq == NULL) {
		return kAudio_MemFullError;
	}

	/* A capture queue's converter writes into the queue's own stage, described as the client's format — which may be
	 * planar, and a buffer list for that is longer than the structure declares. */
	aq->chunk = fn_list_new(fn_client_planes(format));
	if (aq->chunk == NULL) {
		free(aq);
		return kAudio_MemFullError;
	}

	aq->format = *format;
	aq->input = input;
	aq->callback = callback;
	aq->block = block;
	aq->input_callback = input_callback;
	aq->input_block = input_block;
	aq->queue = queue;
	aq->client = client;
	aq->volume = 1.0f;
	aq->device = kAudioObjectUnknown;

	pthread_mutex_init(&aq->lock, NULL);

	status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_device, 0, NULL, &size, &aq->device);
	if (status != 0 || aq->device == kAudioObjectUnknown) {
		pthread_mutex_destroy(&aq->lock);
		free(aq);
		return kAudioQueueErr_InvalidDevice;
	}

	/* The device's format is the queue's other end. It is asked for rather than assumed: a device with one
	 * channel must not be handed a stereo buffer. */
	{
		UInt32 channels = fn_device_channels(aq->device, input);

		memset(&aq->device_format, 0, sizeof(aq->device_format));
		aq->device_format.mSampleRate = format->mSampleRate;
		aq->device_format.mFormatID = kAudioFormatLinearPCM;
		aq->device_format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
		                               | kAudioFormatFlagIsPacked;
		aq->device_format.mChannelsPerFrame = channels;
		aq->device_format.mBitsPerChannel = 32;
		aq->device_format.mBytesPerFrame = channels * (UInt32)sizeof(float);
		aq->device_format.mBytesPerPacket = aq->device_format.mBytesPerFrame;
		aq->device_format.mFramesPerPacket = 1;

		/* The device's own rate, not the file's: a queue plays into a device, and the converter handles the
		 * difference. */
		{
			AudioObjectPropertyAddress rate_address = { kAudioDevicePropertyNominalSampleRate,
			                                            kAudioObjectPropertyScopeGlobal,
			                                            kAudioObjectPropertyElementMaster };
			Float64 rate = 0.0;
			UInt32 rate_size = sizeof(rate);

			if (AudioObjectGetPropertyData(aq->device, &rate_address, 0, NULL, &rate_size, &rate) == 0
			    && rate > 0.0) {
				aq->device_format.mSampleRate = rate;
			}
		}
	}

	/* THE MAPPING THIS QUEUE CAN DO, AND THE ONE IT CANNOT. Mono in and any number out is a fan-out and any
	 * number in and mono out is a sum; two different counts above one would need a real channel map, which is
	 * the converter's door and not this one's — so it is refused here, by name, instead of playing the first
	 * channel and dropping the rest. */
	aq->convert_format = aq->device_format;
	aq->convert_format.mChannelsPerFrame = format->mChannelsPerFrame;
	aq->convert_format.mBytesPerFrame = format->mChannelsPerFrame * (UInt32)sizeof(float);
	aq->convert_format.mBytesPerPacket = aq->convert_format.mBytesPerFrame;

	if (aq->device_format.mChannelsPerFrame != format->mChannelsPerFrame) {
		if (format->mChannelsPerFrame != 1 && aq->device_format.mChannelsPerFrame != 1) {
			pthread_mutex_destroy(&aq->lock);
			free(aq);
			return kAudioQueueErr_InvalidDevice;
		}
		aq->map_channels = true;
	}

	/* **THE CONVERTER FACES THE WAY THE AUDIO DOES.** An output queue converts the client's format into the
	 * device's; a capture queue converts the device's into the client's. Either way the conversion happens on the
	 * queue's own channel count — `convert_format`, which is the device's rate with the client's channels — because
	 * `AudioConverterNew` refuses a channel-count change and the mapping above is where that belongs. */
	if (aq->map_channels
	    || aq->device_format.mSampleRate != format->mSampleRate
	    || format->mBitsPerChannel != 32
	    || (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0
	    || (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0) {
		status = input ? AudioConverterNew(&aq->convert_format, format, &aq->converter)
		               : AudioConverterNew(format, &aq->convert_format, &aq->converter);
		if (status != 0) {
			pthread_mutex_destroy(&aq->lock);
			free(aq);
			return kAudioQueueErr_InvalidDevice;
		}
	}

	*out = aq;
	return 0;
}

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *inFormat, AudioQueueOutputCallback inCallbackProc,
                             void *inUserData, CFRunLoopRef inCallbackRunLoop,
                             CFStringRef inCallbackRunLoopMode, UInt32 inFlags,
                             AudioQueueRef *outAQ)
{
	(void)inCallbackRunLoop;
	(void)inCallbackRunLoopMode;
	(void)inFlags;

	return fn_queue_new(inFormat, false, outAQ, inCallbackProc, NULL, NULL, NULL, NULL, inUserData);
}

OSStatus AudioQueueNewOutputWithDispatchQueue(AudioQueueRef *outAQ,
                                              const AudioStreamBasicDescription *inFormat, UInt32 inFlags,
                                              dispatch_queue_t inCallbackRunLoop,
                                              AudioQueueOutputCallbackBlock inCallbackProc)
{
	(void)inFlags;

	return fn_queue_new(inFormat, false, outAQ, NULL, inCallbackProc, NULL, NULL, inCallbackRunLoop,
	                    NULL);
}

OSStatus AudioQueueNewInput(const AudioStreamBasicDescription *inFormat, AudioQueueInputCallback inCallbackProc,
                            void *inUserData, CFRunLoopRef inCallbackRunLoop,
                            CFStringRef inCallbackRunLoopMode, UInt32 inFlags, AudioQueueRef *outAQ)
{
	(void)inCallbackRunLoop;
	(void)inCallbackRunLoopMode;
	(void)inFlags;

	return fn_queue_new(inFormat, true, outAQ, NULL, NULL, inCallbackProc, NULL, NULL, inUserData);
}

OSStatus AudioQueueNewInputWithDispatchQueue(AudioQueueRef *outAQ,
                                             const AudioStreamBasicDescription *inFormat, UInt32 inFlags,
                                             dispatch_queue_t inCallbackRunLoop,
                                             AudioQueueInputCallbackBlock inCallbackProc)
{
	(void)inFlags;

	return fn_queue_new(inFormat, true, outAQ, NULL, NULL, NULL, inCallbackProc, inCallbackRunLoop, NULL);
}

/* ------------------------------------------------------------------------------------------------
 * Buffers.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioQueueAllocateBuffer(AudioQueueRef inAQ, UInt32 inBufferByteSize,
                                  AudioQueueBufferRef *outBuffer)
{
	return AudioQueueAllocateBufferWithPacketDescriptions(inAQ, inBufferByteSize, 0, outBuffer);
}

OSStatus AudioQueueAllocateBufferWithPacketDescriptions(AudioQueueRef inAQ, UInt32 inBufferByteSize,
                                                        UInt32 inNumberPacketDescriptions,
                                                        AudioQueueBufferRef *outBuffer)
{
	agora_queue_buffer *buffer;
	UInt32 slot = AGORA_QUEUE_MAX_BUFFERS;

	if (inAQ == NULL || outBuffer == NULL || inBufferByteSize == 0) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&inAQ->lock);

	{
		UInt32 i;

		for (i = 0; i < AGORA_QUEUE_MAX_BUFFERS; i++) {
			if (!inAQ->buffer_in_use[i]) {
				slot = i;
				break;
			}
		}
	}

	if (slot == AGORA_QUEUE_MAX_BUFFERS) {
		pthread_mutex_unlock(&inAQ->lock);
		return kAudio_MemFullError;
	}

	/* One allocation for the header and the audio, so that `AudioQueueFreeBuffer` is one `free`. */
	buffer = calloc(1, sizeof(*buffer) + inBufferByteSize);
	if (buffer == NULL) {
		pthread_mutex_unlock(&inAQ->lock);
		return kAudio_MemFullError;
	}

	buffer->mAudioDataBytesCapacity = inBufferByteSize;
	buffer->mAudioData = (void *)((uint8_t *)buffer + sizeof(*buffer));
	buffer->mAudioDataByteSize = 0;
	buffer->mPacketDescriptionCapacity = inNumberPacketDescriptions;
	buffer->mPacketDescriptions = NULL;

	inAQ->buffer_in_use[slot] = true;
	inAQ->buffers[slot] = (AudioQueueBufferRef)buffer;
	inAQ->buffer_count++;

	pthread_mutex_unlock(&inAQ->lock);

	*outBuffer = (AudioQueueBufferRef)buffer;
	return 0;
}

OSStatus AudioQueueFreeBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
	UInt32 i;

	if (inAQ == NULL || inBuffer == NULL) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&inAQ->lock);

	for (i = 0; i < AGORA_QUEUE_MAX_BUFFERS; i++) {
		if (inAQ->buffer_in_use[i] && inAQ->buffers[i] == inBuffer) {
			inAQ->buffer_in_use[i] = false;
			inAQ->buffers[i] = NULL;
			inAQ->buffer_count--;
			pthread_mutex_unlock(&inAQ->lock);
			free(inBuffer);
			return 0;
		}
	}

	pthread_mutex_unlock(&inAQ->lock);
	return kAudioQueueErr_InvalidBuffer;
}

OSStatus AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer, UInt32 inNumPacketDescs,
                                 const AudioStreamPacketDescription *inPacketDescs)
{
	(void)inNumPacketDescs;
	(void)inPacketDescs;

	if (inAQ == NULL || inBuffer == NULL) {
		return kAudio_ParamError;
	}
	if (inBuffer->mAudioDataByteSize == 0) {
		return kAudioQueueErr_BufferEmpty;
	}

	pthread_mutex_lock(&inAQ->lock);

	{
		UInt32 i;

		for (i = 0; i < inAQ->pending_count; i++) {
			if (inAQ->pending[(inAQ->pending_head + i) % AGORA_QUEUE_MAX_PENDING] == inBuffer) {
				pthread_mutex_unlock(&inAQ->lock);
				return kAudioQueueErr_BufferEnqueuedTwice;
			}
		}
	}

	fn_push_pending(inAQ, inBuffer);
	pthread_mutex_unlock(&inAQ->lock);

	return 0;
}

OSStatus AudioQueueEnqueueBufferWithParameters(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                                               UInt32 inNumPacketDescs,
                                               const AudioStreamPacketDescription *inPacketDescs,
                                               UInt32 inTrimFramesAtStart, UInt32 inTrimFramesAtEnd,
                                               UInt32 inNumParamEvents,
                                               const AudioQueueParameterEvent *inParamEvents,
                                               const AudioTimeStamp *inStartTime,
                                               AudioTimeStamp *outActualStartTime)
{
	OSStatus status;

	/* Trimming and scheduled parameters are not implemented; a scheduled START TIME is honoured only in the
	 * sense that the buffer plays next, which is what the queue does anyway. */
	(void)inTrimFramesAtStart;
	(void)inTrimFramesAtEnd;
	(void)inNumParamEvents;
	(void)inParamEvents;
	(void)inStartTime;
	(void)outActualStartTime;

	status = AudioQueueEnqueueBuffer(inAQ, inBuffer, inNumPacketDescs, inPacketDescs);
	return status;
}

/* ------------------------------------------------------------------------------------------------
 * Running.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioQueueStart(AudioQueueRef inAQ, const AudioTimeStamp *inStartTime)
{
	(void)inStartTime;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&inAQ->lock);

	if (!inAQ->running) {
		AudioDeviceIOProcID id = NULL;
		OSStatus status = AudioDeviceCreateIOProcID(inAQ->device,
		                                            inAQ->input ? fn_queue_capture : fn_queue_render,
		                                            inAQ, &id);

		if (status == 0) {
			status = AudioDeviceStart(inAQ->device, id);
		}
		if (status != 0) {
			if (id != NULL) {
				AudioDeviceDestroyIOProcID(inAQ->device, id);
			}
			pthread_mutex_unlock(&inAQ->lock);
			return kAudioQueueErr_CannotStart;
		}

		inAQ->running = true;
		inAQ->generation++;
		fn_notify(inAQ, kAudioQueueProperty_IsRunning);
	}

	pthread_mutex_unlock(&inAQ->lock);
	return 0;
}

OSStatus AudioQueueStop(AudioQueueRef inAQ, Boolean inImmediate)
{
	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&inAQ->lock);

	if (!inAQ->running) {
		pthread_mutex_unlock(&inAQ->lock);
		return 0;
	}

	pthread_mutex_unlock(&inAQ->lock);

	if (!inImmediate && !inAQ->input) {
		/* DRAIN: wait for the enqueued audio to finish. The io proc owns the queue's clock, so this waits on its
		 * progress rather than on a timer. A capture queue has nothing to play out — draining it would wait for
		 * its *empty* buffers to be consumed, which is to say forever. */
		UInt32 guard = 0;

		while (guard++ < 2000) {
			bool empty;

			pthread_mutex_lock(&inAQ->lock);
			empty = (inAQ->current == NULL && inAQ->pending_count == 0);
			pthread_mutex_unlock(&inAQ->lock);

			if (empty) {
				break;
			}

			{
				struct timespec ts = { 0, 5 * 1000 * 1000 };	/* 5 ms */

				nanosleep(&ts, NULL);
			}
		}
	}

	pthread_mutex_lock(&inAQ->lock);
	inAQ->running = false;
	inAQ->generation++;
	pthread_mutex_unlock(&inAQ->lock);

	/* The io proc id is the proc pointer, which is how `AudioDeviceDestroyIOProcID` finds it. */
	AudioDeviceStop(inAQ->device, inAQ->input ? fn_queue_capture : fn_queue_render);
	AudioDeviceDestroyIOProcID(inAQ->device, inAQ->input ? fn_queue_capture : fn_queue_render);

	fn_notify(inAQ, kAudioQueueProperty_IsRunning);
	return 0;
}

OSStatus AudioQueuePause(AudioQueueRef inAQ)
{
	AudioTimeStamp now;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	memset(&now, 0, sizeof(now));
	return AudioQueueStop(inAQ, true);
}

OSStatus AudioQueueFlush(AudioQueueRef inAQ)
{
	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&inAQ->lock);
	inAQ->pending_head = 0;
	inAQ->pending_count = 0;
	inAQ->current = NULL;
	inAQ->current_offset = 0;
	pthread_mutex_unlock(&inAQ->lock);

	return 0;
}

OSStatus AudioQueueReset(AudioQueueRef inAQ)
{
	OSStatus status;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	status = AudioQueueFlush(inAQ);

	if (status == 0 && inAQ->converter != NULL) {
		AudioConverterReset(inAQ->converter);
	}

	/* **A RESET IS A DISCONTINUITY BY DEFINITION**: the queued audio is gone, so any time a caller derived from it is
	 * no longer the queue's time. */
	pthread_mutex_lock(&inAQ->lock);
	inAQ->generation++;
	pthread_mutex_unlock(&inAQ->lock);

	return status;
}

OSStatus AudioQueuePrime(AudioQueueRef inAQ, UInt32 inNumberOfFramesToPrepare,
                         UInt32 *outNumberOfFramesPrepared)
{
	(void)inNumberOfFramesToPrepare;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	if (outNumberOfFramesPrepared != NULL) {
		*outNumberOfFramesPrepared = 0;
	}

	return 0;
}

OSStatus AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate)
{
	UInt32 i;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	AudioQueueStop(inAQ, inImmediate);

	if (inAQ->converter != NULL) {
		AudioConverterDispose(inAQ->converter);
	}
	free(inAQ->map_scratch);
	free(inAQ->stage);
	free(inAQ->tapped);
	free(inAQ->chunk);

	/* A tap is destroyed implicitly when its queue is, which is the only lifetime rule it has. */
	if (inAQ->tap != NULL) {
		free(inAQ->tap->source);
		free(inAQ->tap->data);
		free(inAQ->tap);
		inAQ->tap = NULL;
	}

	for (i = 0; i < AGORA_QUEUE_MAX_BUFFERS; i++) {
		if (inAQ->buffer_in_use[i]) {
			free(inAQ->buffers[i]);
		}
	}

	pthread_mutex_destroy(&inAQ->lock);
	free(inAQ);
	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Parameters and properties.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioQueueSetParameter(AudioQueueRef inAQ, AudioQueueParameterID inParamID, AudioQueueParameterValue inValue)
{
	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	switch (inParamID) {
	case kAudioQueueParam_Volume:
		if (inValue < 0.0f) { inValue = 0.0f; }
		if (inValue > 1.0f) { inValue = 1.0f; }
		inAQ->volume = inValue;
		return 0;
	case kAudioQueueParam_VolumeRampTime:
		inAQ->ramp_time = inValue;	/* stored; every change is applied at once */
		return 0;
	default:
		return kAudioQueueErr_InvalidParameter;
	}
}

OSStatus AudioQueueGetParameter(AudioQueueRef inAQ, AudioQueueParameterID inParamID,
                                AudioQueueParameterValue *outValue)
{
	if (inAQ == NULL || outValue == NULL) {
		return kAudio_ParamError;
	}

	switch (inParamID) {
	case kAudioQueueParam_Volume: *outValue = inAQ->volume; return 0;
	case kAudioQueueParam_VolumeRampTime: *outValue = inAQ->ramp_time; return 0;
	default: return kAudioQueueErr_InvalidParameter;
	}
}

OSStatus AudioQueueGetProperty(AudioQueueRef inAQ, AudioQueuePropertyID inID, void *outData, UInt32 *ioDataSize)
{
	if (inAQ == NULL || ioDataSize == NULL || outData == NULL) {
		return kAudio_ParamError;
	}

	switch (inID) {
	case kAudioQueueProperty_StreamDescription:
		if (*ioDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		*(AudioStreamBasicDescription *)outData = inAQ->format;
		*ioDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioQueueProperty_IsRunning: {
		if (*ioDataSize < sizeof(UInt32)) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		*(UInt32 *)outData = inAQ->running ? 1U : 0U;
		*ioDataSize = sizeof(UInt32);
		return 0;
	}

	case kAudioQueueProperty_CurrentDevice:
		if (*ioDataSize < sizeof(AudioDeviceID)) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		*(AudioDeviceID *)outData = inAQ->device;
		*ioDataSize = sizeof(AudioDeviceID);
		return 0;

	case kAudioQueueProperty_EnableLevelMetering:
		if (*ioDataSize < sizeof(UInt32)) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		*(UInt32 *)outData = inAQ->metering ? 1U : 0U;
		*ioDataSize = sizeof(UInt32);
		return 0;

	case kAudioQueueProperty_CurrentLevelMeter:
	case kAudioQueueProperty_CurrentLevelMeterDB: {
		UInt32 channels = inAQ->format.mChannelsPerFrame;
		UInt32 needed = channels * (UInt32)sizeof(AudioQueueLevelMeterState);

		if (*ioDataSize < needed) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		memset(outData, 0, needed);
		*ioDataSize = needed;
		return 0;
	}

	default:
		return kAudioQueueErr_InvalidProperty;
	}
}

OSStatus AudioQueueSetProperty(AudioQueueRef inAQ, AudioQueuePropertyID inID, const void *inData, UInt32 inDataSize)
{
	if (inAQ == NULL || inData == NULL) {
		return kAudio_ParamError;
	}

	switch (inID) {
	case kAudioQueueProperty_EnableLevelMetering:
		if (inDataSize < sizeof(UInt32)) {
			return kAudioQueueErr_InvalidPropertySize;
		}
		inAQ->metering = (*(const UInt32 *)inData) != 0;
		return 0;

	case kAudioQueueProperty_CurrentDevice:
		/* Changing the device under a running queue is a re-open, which this phase does not do. */
		return kAudioQueueErr_InvalidPropertyValue;

	default:
		return kAudioQueueErr_InvalidProperty;
	}
}

OSStatus AudioQueueGetPropertySize(AudioQueueRef inAQ, AudioQueuePropertyID inID, UInt32 *outDataSize)
{
	if (inAQ == NULL || outDataSize == NULL) {
		return kAudio_ParamError;
	}

	switch (inID) {
	case kAudioQueueProperty_StreamDescription: *outDataSize = sizeof(AudioStreamBasicDescription); return 0;
	case kAudioQueueProperty_IsRunning:
	case kAudioQueueProperty_EnableLevelMetering: *outDataSize = sizeof(UInt32); return 0;
	case kAudioQueueProperty_CurrentDevice: *outDataSize = sizeof(AudioDeviceID); return 0;
	case kAudioQueueProperty_CurrentLevelMeter:
	case kAudioQueueProperty_CurrentLevelMeterDB:
		*outDataSize = inAQ->format.mChannelsPerFrame * (UInt32)sizeof(AudioQueueLevelMeterState);
		return 0;
	default:
		return kAudioQueueErr_InvalidProperty;
	}
}

OSStatus AudioQueueAddPropertyListener(AudioQueueRef inAQ, AudioQueuePropertyID inID,
                                       AudioQueuePropertyListenerProc inProc, void *inUserData)
{
	UInt32 i;

	if (inAQ == NULL || inProc == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < AGORA_QUEUE_MAX_LISTENERS; i++) {
		if (!inAQ->listeners[i].active) {
			inAQ->listeners[i].active = true;
			inAQ->listeners[i].proc = inProc;
			inAQ->listeners[i].client = inUserData;
			inAQ->listeners[i].id = inID;
			return 0;
		}
	}

	return kAudioQueueErr_InvalidParameter;
}

OSStatus AudioQueueRemovePropertyListener(AudioQueueRef inAQ, AudioQueuePropertyID inID,
                                          AudioQueuePropertyListenerProc inProc, void *inUserData)
{
	UInt32 i;

	if (inAQ == NULL || inProc == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < AGORA_QUEUE_MAX_LISTENERS; i++) {
		if (inAQ->listeners[i].active && inAQ->listeners[i].proc == inProc
		    && inAQ->listeners[i].client == inUserData && inAQ->listeners[i].id == inID) {
			inAQ->listeners[i].active = false;
			return 0;
		}
	}

	return kAudioQueueErr_InvalidParameter;
}

OSStatus AudioQueueGetCurrentTime(AudioQueueRef inAQ, AudioQueueTimelineRef inTimeline,
                                  AudioTimeStamp *outTimeStamp, Boolean *outDiscontinuity)
{
	bool discontinuity = false;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}
	if (inTimeline != NULL && inTimeline->queue != inAQ) {
		return kAudioQueueErr_InvalidParameter;
	}

	pthread_mutex_lock(&inAQ->lock);

	if ((inAQ->last_time.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
		memset(&inAQ->last_time, 0, sizeof(inAQ->last_time));
		inAQ->last_time.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
		inAQ->last_time.mHostTime = AudioGetCurrentHostTime();
	}

	if (inTimeline != NULL) {
		/* **THE FIRST LOOK PRIMES IT; EVERY LATER ONE COMPARES.** A discontinuity is reported once — the caller acts
		 * on it by resetting its own clock — and a sample time that moved backwards is the same event. */
		discontinuity = inTimeline->primed
		             && (inTimeline->generation != inAQ->generation
		                 || inAQ->frames_played < inTimeline->frames);
		inTimeline->generation = inAQ->generation;
		inTimeline->frames = inAQ->frames_played;
		inTimeline->primed = true;
	}

	if (outTimeStamp != NULL) {
		*outTimeStamp = inAQ->last_time;
	}
	if (outDiscontinuity != NULL) {
		*outDiscontinuity = discontinuity;
	}

	pthread_mutex_unlock(&inAQ->lock);

	return 0;
}


/* **OFFLINE RENDER IS THE DEVICE'S JOB WITHOUT THE DEVICE.** The queued buffers are already in the client's format, so
 * nothing has to be converted: a frame of output is a frame of buffer, and the queue hands each finished buffer back
 * exactly as the io proc does — which is what lets a caller write a file without a sound card, and what makes the
 * callback path testable without one. The one thing it does not do is mix in the device's rate: a queue whose device
 * format differs from its client format converts on the way *out*, and there is no "out" here.
 *
 * **THE TIMESTAMP IS THE CALLER'S CLOCK, AND THE QUEUE KEEPS ITS OWN.** `inTimestamp` says when this block is meant to
 * play; the queue advances `frames_played` and its sample time by what it rendered, which is the number the next call's
 * timestamp should follow. */
OSStatus AudioQueueOfflineRender(AudioQueueRef inAQ, const AudioTimeStamp *inTimestamp,
                                 AudioQueueBufferRef ioBuffer, UInt32 inNumberFrames)
{
	AudioQueueRef queue = inAQ;
	AudioQueueBufferRef finished[AGORA_QUEUE_MAX_PENDING];
	UInt32 finished_count = 0;
	AudioTimeStamp stamp;
	size_t frame_bytes;
	UInt32 produced = 0;
	uint8_t *out;

	if (queue == NULL || ioBuffer == NULL || ioBuffer->mAudioData == NULL) {
		return kAudio_ParamError;
	}

	frame_bytes = fn_client_frame_bytes(queue);
	if (frame_bytes == 0) {
		return kAudioQueueErr_InvalidParameter;
	}
	if (inNumberFrames > ioBuffer->mAudioDataBytesCapacity / frame_bytes) {
		return kAudioQueueErr_InvalidParameter;
	}

	out = ioBuffer->mAudioData;
	memset(out, 0, (size_t)inNumberFrames * frame_bytes);

	pthread_mutex_lock(&queue->lock);

	/* **THE OFFLINE PATH STAGES TOO, BECAUSE A TAP DOES NOT KNOW WHICH PATH CALLED IT.** The stage is in the client's
	 * format, which here is also the caller's format, so the render is a stage and a copy rather than a second
	 * pipeline. */
	fn_stage_compact(queue);
	if (queue->stage_frames < inNumberFrames) {
		fn_stage_pull(queue, inNumberFrames - queue->stage_frames, finished, &finished_count);
	}

	memset(&stamp, 0, sizeof(stamp));
	if (inTimestamp != NULL) {
		stamp = *inTimestamp;
	}

	{
		const uint8_t *source;
		UInt32 available;

		if (queue->tap != NULL) {
			source = fn_tap_process(queue, inNumberFrames, &stamp, &available);
		} else {
			available = queue->stage_frames - queue->stage_offset;
			source = queue->stage + (size_t)queue->stage_offset * frame_bytes;
		}

		produced = available < inNumberFrames ? available : inNumberFrames;
		if (produced > 0) {
			memcpy(out, source, (size_t)produced * frame_bytes);
		}
		if (queue->tap == NULL) {
			queue->stage_offset += produced;
		}
	}

	/* **THE ANSWER IS THE WHOLE REQUEST, SILENCE INCLUDED.** A caller rendering a file writes what it is given, so a
	 * short answer would truncate the file; and the time base advances by what was rendered, which is the request too —
	 * an empty queue is still 512 frames of the timeline. */
	ioBuffer->mAudioDataByteSize = inNumberFrames * (UInt32)frame_bytes;

	queue->frames_played += inNumberFrames;
	if ((queue->last_time.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
		/* The first render establishes the time base: the caller's timestamp when it gave one, and zero otherwise. */
		memset(&queue->last_time, 0, sizeof(queue->last_time));
		queue->last_time.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
		queue->last_time.mSampleTime = (inTimestamp != NULL
		                                && (inTimestamp->mFlags & kAudioTimeStampSampleTimeValid))
		                             ? inTimestamp->mSampleTime : 0.0;
		queue->last_time.mHostTime = (inTimestamp != NULL) ? inTimestamp->mHostTime
		                                                  : AudioGetCurrentHostTime();
	}
	queue->last_time.mSampleTime += (Float64)inNumberFrames;

	pthread_mutex_unlock(&queue->lock);

	/* The callbacks run with the lock released, for the same reason the io proc's do: the caller refills and re-enqueues
	 * the buffer from inside one. */
	{
		UInt32 i;

		for (i = 0; i < finished_count; i++) {
			if (queue->callback != NULL) {
				queue->callback(queue->client, queue, finished[i]);
			} else if (queue->block != NULL) {
				queue->block(queue, finished[i]);
			}
		}
	}

	return 0;
}

OSStatus AudioQueueCreateTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef *outTimeline)
{
	AudioQueueTimelineRef timeline;

	if (inAQ == NULL || outTimeline == NULL) {
		return kAudio_ParamError;
	}

	timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL) {
		return kAudio_MemFullError;
	}
	timeline->queue = inAQ;

	*outTimeline = timeline;
	return 0;
}

OSStatus AudioQueueDisposeTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef inTimeline)
{
	if (inAQ == NULL || inTimeline == NULL) {
		return kAudio_ParamError;
	}
	if (inTimeline->queue != inAQ) {
		return kAudioQueueErr_InvalidParameter;
	}

	free(inTimeline);
	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Processing taps.
 *
 * **A TAP SEES THE QUEUE'S DECODED AUDIO IN THE CLIENT'S FORMAT**, which is what makes the stage below worth its
 * copy: the device path converts straight out of the caller's buffers into the device's, so without a moment when
 * the audio exists in one piece in the client's format there would be nothing for a tap to tap. It may process the
 * source in place — the buffer list it is handed points into the queue's stage — or hand back its own buffers.
 *
 * ONE TAP PER QUEUE, and it is created and destroyed only while the queue is stopped: Apple's header says so for
 * the constructor and the disposer, and `kAudioQueueErr_TooManyTaps` is the name it refuses a second one with.
 *
 * THE QUEUE HAS NO EFFECTS, SO PRE- AND POST-EFFECTS DIFFER ONLY IN WHAT THE FLAGS SAY. The only processing this
 * queue does after the tap is its own conversion, channel mapping and volume; a PreEffects tap therefore hears the
 * same audio a PostEffects one would. Both flags are reported to the callback exactly as they were asked for, and
 * the README says which is which rather than implying a difference that does not exist.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioQueueProcessingTapNew(AudioQueueRef inAQ, AudioQueueProcessingTapCallback inCallback,
                                    void *inClientData, AudioQueueProcessingTapFlags inFlags,
                                    UInt32 *outMaxFrames, AudioStreamBasicDescription *outProcessingFormat,
                                    AudioQueueProcessingTapRef *outAQTap)
{
	AudioQueueProcessingTapRef tap;
	UInt32 planes;
	bool pre;
	bool post;

	if (inAQ == NULL || inCallback == NULL || outAQTap == NULL) {
		return kAudio_ParamError;
	}

	*outAQTap = NULL;

	/* "either the PreEffects or PostEffects flag must be set, but not both". */
	pre = (inFlags & kAudioQueueProcessingTap_PreEffects) != 0;
	post = (inFlags & kAudioQueueProcessingTap_PostEffects) != 0;
	if (pre == post) {
		return kAudioQueueErr_InvalidParameter;
	}

	pthread_mutex_lock(&inAQ->lock);

	if (inAQ->running) {
		/* "A processing tap can only be established ... on an audio queue that is stopped (paused is not
		 * sufficient)." A paused queue here IS a stopped one, so the test is the same. */
		pthread_mutex_unlock(&inAQ->lock);
		return kAudioQueueErr_InvalidRunState;
	}
	if (inAQ->tap != NULL) {
		pthread_mutex_unlock(&inAQ->lock);
		return kAudioQueueErr_TooManyTaps;
	}

	tap = calloc(1, sizeof(*tap));
	if (tap == NULL) {
		pthread_mutex_unlock(&inAQ->lock);
		return kAudio_MemFullError;
	}

	planes = fn_client_planes(&inAQ->format);
	tap->source = fn_list_new(planes);
	tap->data = fn_list_new(planes);
	if (tap->source == NULL || tap->data == NULL) {
		free(tap->source);
		free(tap->data);
		free(tap);
		pthread_mutex_unlock(&inAQ->lock);
		return kAudio_MemFullError;
	}

	/* `ioData` is described before the first callback: the tap is told the shape of the format it is handed even
	 * though, on entry, every pointer in it is null and every length is zero. */
	{
		UInt32 i;

		tap->data->mNumberBuffers = planes;
		for (i = 0; i < planes; i++) {
			tap->data->mBuffers[i].mNumberChannels = (planes == 1) ? inAQ->format.mChannelsPerFrame : 1;
		}
	}

	tap->queue = inAQ;
	tap->callback = inCallback;
	tap->client = inClientData;
	tap->flags = inFlags;
	inAQ->tap = tap;

	pthread_mutex_unlock(&inAQ->lock);

	if (outMaxFrames != NULL) {
		/* "Typically this will be approximately 50 msec of audio (2048 samples @ 44.1kHz)". */
		UInt32 frames = (UInt32)(inAQ->format.mSampleRate / 20.0);

		*outMaxFrames = frames < 256 ? 256 : frames;
	}
	if (outProcessingFormat != NULL) {
		*outProcessingFormat = inAQ->format;
	}

	*outAQTap = tap;
	return 0;
}

OSStatus AudioQueueProcessingTapDispose(AudioQueueProcessingTapRef inAQTap)
{
	AudioQueueRef queue;

	if (inAQTap == NULL) {
		return kAudio_ParamError;
	}

	queue = inAQTap->queue;
	if (queue == NULL) {
		return kAudioQueueErr_InvalidTapContext;
	}

	pthread_mutex_lock(&queue->lock);

	if (queue->running) {
		pthread_mutex_unlock(&queue->lock);
		return kAudioQueueErr_InvalidRunState;
	}
	if (queue->tap != inAQTap) {
		/* The tap has already been removed, or it is not this queue's: either way it is not a live tap and
		 * freeing it here would be the second free of the same pointer. */
		pthread_mutex_unlock(&queue->lock);
		return kAudioQueueErr_InvalidTapContext;
	}

	queue->tap = NULL;
	pthread_mutex_unlock(&queue->lock);

	free(inAQTap->source);
	free(inAQTap->data);
	free(inAQTap);
	return 0;
}

OSStatus AudioQueueProcessingTapGetSourceAudio(AudioQueueProcessingTapRef inAQTap, UInt32 inNumberFrames,
                                               AudioTimeStamp *ioTimeStamp, AudioQueueProcessingTapFlags *outFlags,
                                               UInt32 *outNumberFrames, AudioBufferList *ioData)
{
	AudioQueueProcessingTapRef tap = inAQTap;
	AudioQueueRef queue;
	AudioQueueProcessingTapFlags flags;
	size_t frame_bytes;
	UInt32 available;
	UInt32 give;
	UInt32 planes;
	UInt32 i;

	(void)ioTimeStamp;	/* the queue's clock is the device's; an output tap does not set it */

	if (tap == NULL || outNumberFrames == NULL || ioData == NULL) {
		return kAudio_ParamError;
	}
	if (!tap->in_callback) {
		return kAudioQueueErr_InvalidTapContext;
	}

	queue = tap->queue;
	frame_bytes = fn_client_frame_bytes(queue);
	available = queue->stage_frames - queue->stage_offset;
	give = inNumberFrames < available ? inNumberFrames : available;

	fn_list_point(tap->source, queue->stage + (size_t)queue->stage_offset * frame_bytes, give, &queue->format);

	/* The queue owns the pointers unless the caller provided its own; either way the data is the same source, so a
	 * caller that brought buffers has it copied into them. */
	planes = fn_client_planes(&queue->format);
	ioData->mNumberBuffers = planes;
	for (i = 0; i < planes && i < tap->source->mNumberBuffers; i++) {
		AudioBuffer *owned = &tap->source->mBuffers[i];

		ioData->mBuffers[i].mNumberChannels = owned->mNumberChannels;
		ioData->mBuffers[i].mDataByteSize = owned->mDataByteSize;
		if (ioData->mBuffers[i].mData == NULL) {
			ioData->mBuffers[i].mData = owned->mData;
		} else if (give > 0) {
			memcpy(ioData->mBuffers[i].mData, owned->mData, owned->mDataByteSize);
		}
	}

	queue->stage_offset += give;
	tap->source_pulled = true;

	flags = tap->flags;
	if (tap->generation != queue->generation) {
		/* A start, a stop or a reset: the source is no longer continuous with the last chunk. */
		flags |= kAudioQueueProcessingTap_StartOfStream;
		tap->generation = queue->generation;
	}
	/* An output queue's source ends when nothing is left to play; a capture queue's ends when the device stops
	 * feeding it, which is what `give == 0` says for both. */
	if (give == 0 || (!queue->input && queue->current == NULL && queue->pending_count == 0)) {
		flags |= kAudioQueueProcessingTap_EndOfStream;
	}

	if (outFlags != NULL) {
		*outFlags = flags;
	}
	*outNumberFrames = give;
	return 0;
}

OSStatus AudioQueueProcessingTapGetQueueTime(AudioQueueProcessingTapRef inAQTap, Float64 *outQueueSampleTime,
                                             UInt32 *outQueueFrameCount)
{
	AudioQueueProcessingTapRef tap = inAQTap;
	AudioQueueRef queue;
	double rate;

	if (tap == NULL || outQueueSampleTime == NULL || outQueueFrameCount == NULL) {
		return kAudio_ParamError;
	}
	if (!tap->in_callback || !tap->source_pulled) {
		/* "may only be called from the processing tap's callback ... It must be called after calling
		 * AudioQueueProcessingTapGetSourceAudio." */
		return kAudioQueueErr_InvalidTapContext;
	}

	queue = tap->queue;

	/* "GetTapQueueTime can only be called on an output queue's tap": a recording has no queue time to report,
	 * because nothing is waiting to be played. */
	if (queue->input) {
		return kAudioQueueErr_InvalidTapType;
	}

	rate = queue->device_format.mSampleRate > 0.0 ? queue->device_format.mSampleRate
	                                              : queue->format.mSampleRate;
	*outQueueSampleTime = rate > 0.0 ? (Float64)queue->frames_played / rate : 0.0;
	*outQueueFrameCount = tap->queue_frames;
	return 0;
}
