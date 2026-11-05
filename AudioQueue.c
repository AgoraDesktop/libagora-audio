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

struct OpaqueAudioQueue {
	AudioStreamBasicDescription format;
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

	AudioQueueOutputCallback        callback;
	AudioQueueOutputCallbackBlock   block;
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

	queue_listener              listeners[AGORA_QUEUE_MAX_LISTENERS];
};

/* ------------------------------------------------------------------------------------------------
 * Small helpers.
 * ---------------------------------------------------------------------------------------------- */

static UInt32 fn_device_channels(AudioDeviceID device)
{
	AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
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
 * The io proc.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	AudioQueueBufferRef buffer;
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
	ioData->mBuffers[0].mData = (uint8_t *)source->buffer->mAudioData + source->delivered;

	source->delivered += wanted * source->bytes_per_frame;
	*ioNumberDataPackets = wanted;
	return 0;
}

static OSStatus fn_queue_render(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
                                const AudioTimeStamp *input_time, AudioBufferList *output,
                                const AudioTimeStamp *output_time, void *client)
{
	AudioQueueRef queue = client;
	AudioQueueBufferRef finished[AGORA_QUEUE_MAX_PENDING];
	UInt32 finished_count = 0;
	UInt32 frames;
	UInt32 channels;
	UInt32 produced = 0;
	UInt32 bytes_per_frame;

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

	bytes_per_frame = fn_pcm_bytes_per_frame(&queue->format);

	while (produced < frames) {
		queue_source source;
		AudioBufferList chunk;
		float *destination;
		UInt32 wanted;
		UInt32 converted;
		OSStatus status;

		if (queue->current == NULL) {
			queue->current = fn_pop_pending(queue);
			queue->current_offset = 0;
			if (queue->current == NULL) {
				break;	/* nothing to play: the rest of the buffer stays silent */
			}
		}

		if (queue->current_offset >= queue->current->mAudioDataByteSize) {
			/* HAND IT BACK AND KEEP PLAYING: breaking here would leave every other device quantum silent
			 * whenever a buffer ends exactly on a quantum boundary, which is the common case when the
			 * buffer and the quantum are the same size. */
			if (finished_count < AGORA_QUEUE_MAX_PENDING) {
				finished[finished_count++] = queue->current;
			}
			queue->current = NULL;
			queue->current_offset = 0;
			continue;
		}

		source.buffer = queue->current;
		source.bytes = queue->current->mAudioDataByteSize - queue->current_offset;
		source.delivered = 0;
		source.bytes_per_frame = bytes_per_frame;

		wanted = frames - produced;

		/* WHERE THE CONVERSION LANDS: the device buffer directly when the channel counts agree, and the queue's
		 * own scratch when they do not (see the mapping below). */
		destination = (float *)output->mBuffers[0].mData;
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
			status = AudioConverterFillComplexBuffer(queue->converter, fn_queue_input, &source, &wanted, &chunk,
			                                         NULL);
			if (status != 0) {
				break;
			}
			converted = wanted;
		} else {
			/* Same format: the buffer is already what the device wants. */
			UInt32 available = source.bytes / bytes_per_frame;
			UInt32 frames_to_copy = wanted < available ? wanted : available;
			size_t bytes = (size_t)frames_to_copy * bytes_per_frame;

			memcpy(chunk.mBuffers[0].mData, (uint8_t *)source.buffer->mAudioData + queue->current_offset, bytes);
			source.delivered = (UInt32)bytes;
			converted = frames_to_copy;
		}

		/* THE MAPPING: a mono source reaches every device channel, a mono device receives the sum of the
		 * source's channels over their count. Neither is a pan law, and both are what a beep expects. */
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

		queue->current_offset += source.delivered;
		produced += converted;

		if (converted == 0) {
			break;
		}
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
 * Creation.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_queue_new(const AudioStreamBasicDescription *format, AudioQueueRef *out,
                             AudioQueueOutputCallback callback, AudioQueueOutputCallbackBlock block,
                             dispatch_queue_t queue, void *client)
{
	AudioQueueRef aq;
	OSStatus status;
	AudioObjectPropertyAddress default_device = { kAudioHardwarePropertyDefaultOutputDevice,
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

	aq->format = *format;
	aq->callback = callback;
	aq->block = block;
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

	/* The device's format is the queue's destination format. It is asked for rather than assumed: a device
	 * with one channel must not be handed a stereo buffer. */
	{
		UInt32 channels = fn_device_channels(aq->device);

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

	if (aq->map_channels
	    || aq->device_format.mSampleRate != format->mSampleRate
	    || format->mBitsPerChannel != 32
	    || (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0
	    || (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0) {
		status = AudioConverterNew(format, &aq->convert_format, &aq->converter);
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

	return fn_queue_new(inFormat, outAQ, inCallbackProc, NULL, NULL, inUserData);
}

OSStatus AudioQueueNewOutputWithDispatchQueue(AudioQueueRef *outAQ,
                                              const AudioStreamBasicDescription *inFormat, UInt32 inFlags,
                                              dispatch_queue_t inCallbackRunLoop,
                                              AudioQueueOutputCallbackBlock inCallbackProc)
{
	(void)inFlags;

	return fn_queue_new(inFormat, outAQ, NULL, inCallbackProc, inCallbackRunLoop, NULL);
}

OSStatus AudioQueueNewInput(const AudioStreamBasicDescription *inFormat, AudioQueueInputCallback inCallbackProc,
                            void *inUserData, CFRunLoopRef inCallbackRunLoop,
                            CFStringRef inCallbackRunLoopMode, UInt32 inFlags, AudioQueueRef *outAQ)
{
	(void)inFormat;
	(void)inCallbackProc;
	(void)inUserData;
	(void)inCallbackRunLoop;
	(void)inCallbackRunLoopMode;
	(void)inFlags;
	(void)outAQ;

	/* Recording needs an input stream opened and scheduled; the device layer has no input backend yet, and a
	 * queue that accepted the call and recorded nothing would be the worst of the two answers. */
	return kAudioQueueErr_InvalidDevice;
}

OSStatus AudioQueueNewInputWithDispatchQueue(AudioQueueRef *outAQ,
                                             const AudioStreamBasicDescription *inFormat, UInt32 inFlags,
                                             dispatch_queue_t inCallbackRunLoop,
                                             AudioQueueInputCallbackBlock inCallbackProc)
{
	(void)outAQ;
	(void)inFormat;
	(void)inFlags;
	(void)inCallbackRunLoop;
	(void)inCallbackProc;

	return kAudioQueueErr_InvalidDevice;
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
		OSStatus status = AudioDeviceCreateIOProcID(inAQ->device, fn_queue_render, inAQ, &id);

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

	if (!inImmediate) {
		/* DRAIN: wait for the enqueued audio to finish. The io proc owns the queue's clock, so this waits on
		 * its progress rather than on a timer. */
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
	pthread_mutex_unlock(&inAQ->lock);

	/* The io proc id is the proc pointer, which is how `AudioDeviceDestroyIOProcID` finds it. */
	AudioDeviceStop(inAQ->device, fn_queue_render);
	AudioDeviceDestroyIOProcID(inAQ->device, fn_queue_render);

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
	OSStatus status = AudioQueueFlush(inAQ);

	if (status == 0 && inAQ->converter != NULL) {
		AudioConverterReset(inAQ->converter);
	}

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
	(void)inTimeline;

	if (inAQ == NULL) {
		return kAudio_ParamError;
	}

	if (outDiscontinuity != NULL) {
		*outDiscontinuity = false;
	}

	if (outTimeStamp != NULL) {
		pthread_mutex_lock(&inAQ->lock);
		if ((inAQ->last_time.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
			memset(&inAQ->last_time, 0, sizeof(inAQ->last_time));
			inAQ->last_time.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
			inAQ->last_time.mHostTime = AudioGetCurrentHostTime();
		}
		*outTimeStamp = inAQ->last_time;
		pthread_mutex_unlock(&inAQ->lock);
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * The doors this phase refuses, by name.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioQueueOfflineRender(AudioQueueRef inAQ, const AudioTimeStamp *inTimestamp,
                                 AudioQueueBufferRef ioBuffer, UInt32 inNumberFrames)
{
	(void)inAQ;
	(void)inTimestamp;
	(void)ioBuffer;
	(void)inNumberFrames;

	return kAudioQueueErr_InvalidOfflineMode;
}

OSStatus AudioQueueCreateTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef *outTimeline)
{
	(void)inAQ;
	(void)outTimeline;

	return kAudioQueueErr_InvalidRunState;
}

OSStatus AudioQueueDisposeTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef inTimeline)
{
	(void)inAQ;
	(void)inTimeline;

	return kAudioQueueErr_InvalidRunState;
}

OSStatus AudioQueueProcessingTapNew(AudioQueueRef inAQ, AudioQueueProcessingTapCallback inCallback,
                                    void *inClientData, AudioQueueProcessingTapFlags inFlags,
                                    UInt32 *outMaxFrames, AudioStreamBasicDescription *outProcessingFormat,
                                    AudioQueueProcessingTapRef *outAQTap)
{
	(void)inAQ;
	(void)inCallback;
	(void)inClientData;
	(void)inFlags;
	(void)outMaxFrames;
	(void)outProcessingFormat;
	(void)outAQTap;

	return kAudioQueueErr_InvalidTapContext;
}
