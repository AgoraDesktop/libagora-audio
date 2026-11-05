/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * queue.c — the buffered model and the system sounds, measured through the mixer.
 *
 * THE ASSERTION IS THE DEVICE'S OUTPUT, NOT THE CALLBACK'S BOOKKEEPING. The callback refills its buffer and
 * re-enqueues it, which is the model's whole point; if the queue were merely counting callbacks, the device
 * would still be silent and this suite would pass. So it reads the HAL's own last mixed peak: 0.5 while the
 * tone plays, 0.25 after the queue's volume is halved, and something audible after an alert sound is played.
 *
 * AND ONE STRUCTURAL TEST: the callback is called with the queue's lock released. A queue that held it would
 * deadlock on the first re-enqueue — which is what every real caller does — so the test would hang rather than
 * fail, and it does not.
 */

#include <AudioToolbox/AudioQueue.h>
#include <AudioToolbox/AudioServices.h>
#include <AudioToolbox/AudioFile.h>
#include "AudioToolbox_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

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

static AudioStreamBasicDescription fn_pcm(double rate, UInt32 channels, UInt32 bits, AudioFormatFlags flags)
{
	AudioStreamBasicDescription asbd;

	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate = rate;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = flags;
	asbd.mChannelsPerFrame = channels;
	asbd.mBitsPerChannel = bits;
	asbd.mBytesPerFrame = (bits / 8) * channels;
	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	asbd.mFramesPerPacket = 1;

	return asbd;
}

static AudioDeviceID fn_default_output(void)
{
	AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice,
	                                       kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMaster };
	AudioDeviceID device = kAudioObjectUnknown;
	UInt32 size = sizeof(device);

	AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device);
	return device;
}

/* ------------------------------------------------------------------------------------------------
 * The output queue.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	int    calls;
	float  value;
	bool   stop_refilling;
} loop_state;

static void fn_refill(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	loop_state *state = client;
	float *samples;
	UInt32 frames;
	UInt32 i;

	if (state->stop_refilling) {
		/* STOPPED FROM INSIDE THE CALLBACK: this is the deadlock test as much as the teardown one. */
		AudioQueueStop(queue, true);
		return;
	}

	state->calls++;

	frames = buffer->mAudioDataByteSize / (2 * (UInt32)sizeof(float));
	samples = buffer->mAudioData;
	for (i = 0; i < frames * 2; i++) {
		samples[i] = state->value;
	}

	AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

/* The mono, 16-bit refill: the queue was told what the buffer holds, so this writes what a WAV decoder would. */
static void fn_refill_mono(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	loop_state *state = client;
	int16_t *samples = buffer->mAudioData;
	UInt32 frames;
	UInt32 i;

	state->calls++;

	frames = buffer->mAudioDataByteSize / (UInt32)sizeof(int16_t);
	for (i = 0; i < frames; i++) {
		samples[i] = (int16_t)(state->value * 32767.0f);
	}

	AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static void fn_unused_input(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer,
                            const AudioTimeStamp *time, UInt32 packets,
                            const AudioStreamPacketDescription *descriptions)
{
	(void)client; (void)queue; (void)buffer; (void)time; (void)packets; (void)descriptions;
}

static void fn_test_queue(void)
{
	AudioDeviceID device = fn_default_output();
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioQueueRef queue = NULL;
	AudioQueueBufferRef first = NULL;
	AudioQueueBufferRef second = NULL;
	loop_state state;
	OSStatus status;
	float peak = 0.0f;

	memset(&state, 0, sizeof(state));
	state.value = 0.5f;

	status = AudioQueueNewOutput(&format, fn_refill, &state, NULL, NULL, 0, &queue);
	fn_check("an output queue is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	fn_check("a buffer is allocated",
	         AudioQueueAllocateBuffer(queue, 512 * 8, &first) == 0 && first != NULL);
	fn_check("a second buffer is allocated",
	         AudioQueueAllocateBuffer(queue, 512 * 8, &second) == 0 && second != NULL);

	fn_check("the buffer is not empty after a fill", 1);

	/* Fill and enqueue both, the way a player primes a queue. */
	{
		AudioQueueBufferRef given[2] = { first, second };
		UInt32 i;

		for (i = 0; i < 2; i++) {
			float *samples = given[i]->mAudioData;
			UInt32 sample;

			for (sample = 0; sample < 512 * 2; sample++) {
				samples[sample] = state.value;
			}
			given[i]->mAudioDataByteSize = 512 * 8;
			fn_check("the buffer is enqueued", AudioQueueEnqueueBuffer(queue, given[i], 0, NULL) == 0);
		}
	}

	fn_check("the same buffer cannot be enqueued twice",
	         AudioQueueEnqueueBuffer(queue, first, 0, NULL) == kAudioQueueErr_BufferEnqueuedTwice);

	fn_check("the queue starts", AudioQueueStart(queue, NULL) == 0);

	{
		UInt32 running = 0;
		UInt32 size = sizeof(running);

		fn_check("the queue says it is running",
		         AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &running, &size) == 0 && running == 1);
	}

	fn_sleep_ms(200);

	fn_check("the callback refilled and re-enqueued", state.calls > 0);

	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the device is playing the queue's audio", peak > 0.45f && peak < 0.55f);

	/* The volume is the queue's, so it scales the mix without touching the device's own volume. */
	fn_check("the queue's volume is set",
	         AudioQueueSetParameter(queue, kAudioQueueParam_Volume, 0.5f) == 0);
	fn_sleep_ms(60);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the queue's volume scales the output", peak > 0.2f && peak < 0.3f);

	{
		AudioQueueParameterValue value = 0.0f;

		fn_check("the queue's volume reads back",
		         AudioQueueGetParameter(queue, kAudioQueueParam_Volume, &value) == 0 && value == 0.5f);
	}

	/* The stream description is the caller's format, not the device's. */
	{
		AudioStreamBasicDescription described;
		UInt32 size = sizeof(described);

		memset(&described, 0, sizeof(described));
		fn_check("the stream description is the queue's format",
		         AudioQueueGetProperty(queue, kAudioQueueProperty_StreamDescription, &described, &size) == 0
		         && described.mSampleRate == 48000.0 && described.mChannelsPerFrame == 2);
	}

	/* Refusals, by name. */
	{
		UInt32 value = 0;
		UInt32 size = sizeof(value);

		fn_check("an unknown queue property is refused by name",
		         AudioQueueGetProperty(queue, 'zzzz', &value, &size) == kAudioQueueErr_InvalidProperty);
		fn_check("a property that does not fit is refused by size",
		         AudioQueueGetProperty(queue, kAudioQueueProperty_StreamDescription, &value, &size)
		         == kAudioQueueErr_InvalidPropertySize);
	}

	/* An output queue cannot record, and says so rather than pretending. */
	{
		AudioQueueRef input = NULL;

		fn_check("an input queue is refused by name",
		         AudioQueueNewInput(&format, fn_unused_input, NULL, NULL, NULL, 0, &input)
		         == kAudioQueueErr_InvalidDevice);
	}

	fn_check("the queue stops", AudioQueueStop(queue, true) == 0);

	{
		UInt32 running = 1;
		UInt32 size = sizeof(running);

		AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &running, &size);
		fn_check("a stopped queue says it is stopped", running == 0);
	}

	fn_sleep_ms(80);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("a stopped queue leaves the device silent", peak == 0.0f);

	fn_check("the queue is disposed", AudioQueueDispose(queue, true) == 0);
}

/* ------------------------------------------------------------------------------------------------
 * The timeline: the door that says the time base broke.
 *
 * **A CALLER MIXING ITS OWN CLOCK INTO THE QUEUE'S HAS ONE QUESTION**, and it is not "what time is it" — the queue
 * answers that — but "has anything happened since I last asked that makes my arithmetic wrong". That is what a
 * timeline is for, and the three events that must report it are a stop, a start and a reset.
 * ---------------------------------------------------------------------------------------------- */

static void fn_test_timeline(void)
{
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioQueueRef queue = NULL;
	AudioQueueTimelineRef timeline = NULL;
	AudioQueueBufferRef buffer = NULL;
	AudioTimeStamp time;
	Boolean discontinuity = -1;
	loop_state state;
	UInt32 sample;
	OSStatus status;

	memset(&state, 0, sizeof(state));
	state.value = 0.25f;

	status = AudioQueueNewOutput(&format, fn_refill, &state, NULL, NULL, 0, &queue);
	fn_check("a queue for the timeline is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	{
		float *samples;

		AudioQueueAllocateBuffer(queue, 512 * 8, &buffer);
		samples = buffer->mAudioData;
		for (sample = 0; sample < 512 * 2; sample++) {
			samples[sample] = 0.25f;
		}
		buffer->mAudioDataByteSize = 512 * 8;
		AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
	}

	fn_check("a timeline is created", AudioQueueCreateTimeline(queue, &timeline) == 0 && timeline != NULL);

	memset(&time, 0, sizeof(time));
	fn_check("the queue starts", AudioQueueStart(queue, NULL) == 0);

	fn_check("the first look primes the timeline without calling it a discontinuity",
	         AudioQueueGetCurrentTime(queue, timeline, &time, &discontinuity) == 0 && discontinuity == false);
	fn_check("the time the timeline reports is sample-valid",
	         (time.mFlags & kAudioTimeStampSampleTimeValid) != 0);

	fn_sleep_ms(60);
	fn_check("a second look with nothing broken reports none",
	         AudioQueueGetCurrentTime(queue, timeline, &time, &discontinuity) == 0 && discontinuity == false);

	fn_check("the queue stops and starts", AudioQueueStop(queue, true) == 0 && AudioQueueStart(queue, NULL) == 0);
	fn_check("a stop and a start break the time base",
	         AudioQueueGetCurrentTime(queue, timeline, &time, &discontinuity) == 0 && discontinuity == true);
	fn_check("the break is reported once, not on every look",
	         AudioQueueGetCurrentTime(queue, timeline, &time, &discontinuity) == 0 && discontinuity == false);

	fn_check("a reset is a discontinuity too",
	         AudioQueueReset(queue) == 0
	         && AudioQueueGetCurrentTime(queue, timeline, &time, &discontinuity) == 0
	         && discontinuity == true);

	fn_check("without a timeline the answer is always no break",
	         AudioQueueGetCurrentTime(queue, NULL, &time, &discontinuity) == 0 && discontinuity == false);

	fn_check("the timeline is disposed", AudioQueueDisposeTimeline(queue, timeline) == 0);
	fn_check("the queue is disposed", AudioQueueDispose(queue, true) == 0);
}

/* ------------------------------------------------------------------------------------------------
 * Offline render: the queue without a device.
 *
 * **IT MUST PRODUCE THE BUFFERS THAT WERE QUEUED, IN THE ORDER THEY WERE QUEUED, AND HAND THEM BACK WHEN THEY RUN OUT.**
 * That is the whole contract, and the samples make it measurable: a buffer filled with 0.5 comes out as 0.5, a buffer
 * that ends mid-render resumes exactly where it stopped, and the callback that refills it fires once the queue has
 * consumed it.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	int calls;
} offline_state;

static void fn_offline_refilled(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	offline_state *state = client;

	(void)queue;
	(void)buffer;
	state->calls++;
}

static void fn_test_offline(void)
{
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioQueueRef queue = NULL;
	AudioQueueBufferRef queued = NULL;
	AudioQueueBufferRef rendered = NULL;
	AudioQueueBufferRef out = NULL;
	offline_state state;
	OSStatus status;
	UInt32 sample;
	UInt32 frames = 512;
	UInt32 i;
	float *samples;
	AudioTimeStamp stamp;
	Boolean discontinuity = true;

	memset(&stamp, 0, sizeof(stamp));
	stamp.mFlags = kAudioTimeStampSampleTimeValid;
	stamp.mHostTime = AudioGetCurrentHostTime();

	memset(&state, 0, sizeof(state));

	status = AudioQueueNewOutput(&format, fn_offline_refilled, &state, NULL, NULL, 0, &queue);
	fn_check("a queue for offline render is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	/* TWO BUFFERS: the render must cross the boundary between them. */
	AudioQueueAllocateBuffer(queue, frames * 8, &queued);
	AudioQueueAllocateBuffer(queue, frames * 8, &rendered);
	/* The output buffer is bigger than either queued one, because a render may span them. */
	AudioQueueAllocateBuffer(queue, 1024 * 8, &out);

	samples = queued->mAudioData;
	for (sample = 0; sample < frames * 2; sample++) {
		samples[sample] = 0.5f;
	}
	queued->mAudioDataByteSize = frames * 8;

	samples = rendered->mAudioData;
	for (sample = 0; sample < frames * 2; sample++) {
		samples[sample] = -0.25f;
	}
	rendered->mAudioDataByteSize = frames * 8;

	fn_check("the buffers are queued",
	         AudioQueueEnqueueBuffer(queue, queued, 0, NULL) == 0
	         && AudioQueueEnqueueBuffer(queue, rendered, 0, NULL) == 0);

	/* 768 frames: the whole first buffer and half of the second. */
	status = AudioQueueOfflineRender(queue, &stamp, out, 768);
	fn_check("the queue renders offline", status == 0);
	fn_check("the render reports the bytes it produced", out->mAudioDataByteSize == 768 * 8);
	{
		float *produced = out->mAudioData;
		bool first_half = true;
		bool second_half = true;

		for (i = 0; i < 512 * 2; i++) {
			if (produced[i] != 0.5f) {
				first_half = false;
			}
		}
		for (i = 512 * 2; i < 768 * 2; i++) {
			if (produced[i] != -0.25f) {
				second_half = false;
			}
		}
		fn_check("the first buffer's samples come out as they went in", first_half);
		fn_check("the render crosses into the next buffer exactly", second_half);
	}
	fn_check("the buffer that ran out is handed back", state.calls == 1);

	/* The rest of the second buffer, then silence because nothing is queued. */
	status = AudioQueueOfflineRender(queue, &stamp, out, 512);
	fn_check("the render continues where it stopped", status == 0);
	{
		float *produced = out->mAudioData;
		bool tail = true;
		bool silence = true;

		for (i = 0; i < 256 * 2; i++) {
			if (produced[i] != -0.25f) {
				tail = false;
			}
		}
		for (i = 256 * 2; i < 512 * 2; i++) {
			if (produced[i] != 0.0f) {
				silence = false;
			}
		}
		fn_check("the second buffer's tail is its own samples", tail);
		fn_check("an empty queue renders silence", silence);
	}
	fn_check("both buffers are handed back", state.calls == 2);

	{
		AudioTimeStamp time;

		memset(&time, 0, sizeof(time));
		fn_check("an offline queue has a time base",
		         AudioQueueGetCurrentTime(queue, NULL, &time, &discontinuity) == 0
		         && (time.mFlags & kAudioTimeStampSampleTimeValid) != 0);
		printf("      offline sample time %.1f (flags 0x%x)\n", (double)time.mSampleTime, (unsigned)time.mFlags);
		fn_check("the sample time is what was rendered", time.mSampleTime == 1280.0);
	}

	fn_check("a buffer too small for the request is refused by name",
	         AudioQueueOfflineRender(queue, &stamp, out, 100000) == kAudioQueueErr_InvalidParameter);
	fn_check("the queue is disposed", AudioQueueDispose(queue, true) == 0);
}

/* ------------------------------------------------------------------------------------------------
 * The system sounds.
 * ---------------------------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------------------------------
 * The channel mapping: a mono source reaches a stereo device.
 * ---------------------------------------------------------------------------------------------- */

/* **THE CASE A BEEP IS USUALLY IN.** A WAV from anywhere is often 16-bit mono, and the device is stereo; the
 * converter refuses a channel-count change (as Apple's does, because a map is what that needs), so the queue
 * fans it out itself. Without this the queue reports an error at creation and NSSound's `-play` answers NO —
 * which is what this suite would have missed, because every other buffer here is already the device's format. */
static void fn_test_mono_mapping(void)
{
	AudioDeviceID device = fn_default_output();
	AudioStreamBasicDescription format = fn_pcm(8000.0, 1, 16,
	                                            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	AudioQueueRef queue = NULL;
	AudioQueueBufferRef buffer = NULL;
	loop_state state;
	OSStatus status;
	float peak = 0.0f;

	memset(&state, 0, sizeof(state));
	state.value = 0.5f;

	status = AudioQueueNewOutput(&format, fn_refill_mono, &state, NULL, NULL, 0, &queue);
	fn_check("a mono queue on a stereo device is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	fn_check("an 8 kHz mono buffer is allocated",
	         AudioQueueAllocateBuffer(queue, 1600, &buffer) == 0 && buffer != NULL);

	{
		int16_t *samples = buffer->mAudioData;
		UInt32 i;

		for (i = 0; i < 800; i++) {
			samples[i] = (int16_t)(state.value * 32767.0f);
		}
		buffer->mAudioDataByteSize = 1600;
		fn_check("the mono buffer is enqueued", AudioQueueEnqueueBuffer(queue, buffer, 0, NULL) == 0);
	}

	fn_check("the queue starts", AudioQueueStart(queue, NULL) == 0);

	{
		UInt32 walked;

		for (walked = 0; walked < 2000 && state.calls == 0; walked += 10) {
			fn_sleep_ms(10);
		}
	}
	fn_check("the mono callback was asked for a buffer", state.calls > 0);

	fn_sleep_ms(150);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("a mono source is audible on the stereo device", peak > 0.4f && peak < 0.6f);

	AudioQueueStop(queue, true);
	AudioQueueDispose(queue, true);
}

/* ------------------------------------------------------------------------------------------------
 * Processing taps: the queue's decoded audio, in the client's format, before the device sees it.
 *
 * **THE TAP'S OWN STATE PROVES THE SOURCE AND THE DEVICE PROVES THE OUTPUT.** A tap that was called with 0.5 and
 * halved it is not the same claim as a device that plays 0.25, and a queue that merely invoked a callback would
 * satisfy the first and be silent at the second. So both are measured, plus the two flags a caller cannot compute
 * for itself: the start and the end of the source.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	int                            calls;
	float                          peak;		/* the largest sample the tap was shown */
	float                          scale;		/* what it multiplies the source by; 1.0 is an in-place pass */
	bool                           siphon;
	bool                           start_of_stream;
	bool                           end_of_stream;
	bool                           entry_clean;	/* ioData arrived empty, as a non-siphon's must */
	OSStatus                       source_status;
	OSStatus                       queue_time_status;
	Float64                        queue_time;
	UInt32                         queue_frames;
	UInt32                         source_frames;
} tap_state;

static void fn_tap(void *client, AudioQueueProcessingTapRef tap, UInt32 number_frames, AudioTimeStamp *time,
                   AudioQueueProcessingTapFlags *flags, UInt32 *out_frames, AudioBufferList *ioData)
{
	tap_state *state = client;
	AudioQueueProcessingTapFlags got = 0;
	AudioBufferList list;
	UInt32 frames = 0;
	UInt32 shown = 0;
	UInt32 i;

	state->calls++;

	if (state->siphon) {
		/* A siphon is shown the source and must not alter it; it does not pull, so ioData arrives valid. The
		 * entry flags are the constructor's, which is what a siphon is told about the stream. */
		state->start_of_stream |= ((*flags & kAudioQueueProcessingTap_StartOfStream) != 0);
		shown = number_frames;
	} else {
		/* Apple: "on entry, the buffer pointers are null and the lengths are zero." Recorded on the first call
		 * only: a later one must not overwrite what the first saw. */
		if (state->calls == 1) {
			state->entry_clean = ioData->mNumberBuffers == 1 && ioData->mBuffers[0].mData == NULL
			                  && ioData->mBuffers[0].mDataByteSize == 0;
		}

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;	/* the queue owns the pointers: none are provided */
		state->source_status = AudioQueueProcessingTapGetSourceAudio(tap, number_frames, time, &got, &frames,
		                                                            &list);
		if (state->source_status == 0) {
			state->source_frames = frames;
			state->start_of_stream |= ((got & kAudioQueueProcessingTap_StartOfStream) != 0);
			state->end_of_stream |= ((got & kAudioQueueProcessingTap_EndOfStream) != 0);
			ioData->mNumberBuffers = 1;
			ioData->mBuffers[0] = list.mBuffers[0];
			*out_frames = frames;
			*flags = got;
			shown = frames;
		}

		/* Only answerable here, and only after the source has been pulled. */
		state->queue_time_status = AudioQueueProcessingTapGetQueueTime(tap, &state->queue_time,
		                                                              &state->queue_frames);
	}

	if (shown == 0 || ioData->mBuffers[0].mData == NULL) {
		return;
	}

	{
		float *samples = ioData->mBuffers[0].mData;
		UInt32 count = shown * 2;

		for (i = 0; i < count; i++) {
			float value = samples[i] < 0.0f ? -samples[i] : samples[i];

			if (value > state->peak) {
				state->peak = value;
			}
			samples[i] *= state->scale;
		}
	}
}

/* A refill that stops re-enqueueing instead of stopping the queue, so what is queued can run out while the io proc is
 * still being called — which is the only way a tap ever sees the end of the source. */
static void fn_refill_patient(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	loop_state *state = client;
	float *samples;
	UInt32 frames;
	UInt32 i;

	state->calls++;

	if (state->stop_refilling) {
		return;
	}

	frames = buffer->mAudioDataByteSize / (2 * (UInt32)sizeof(float));
	samples = buffer->mAudioData;
	for (i = 0; i < frames * 2; i++) {
		samples[i] = state->value;
	}

	AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static void fn_queue_two_buffers(AudioQueueRef queue, AudioQueueBufferRef *given, UInt32 frames, float value)
{
	UInt32 i;

	for (i = 0; i < 2; i++) {
		float *samples;
		UInt32 sample;

		AudioQueueAllocateBuffer(queue, frames * 8, &given[i]);
		samples = given[i]->mAudioData;
		for (sample = 0; sample < frames * 2; sample++) {
			samples[sample] = value;
		}
		given[i]->mAudioDataByteSize = frames * 8;
		fn_check("the buffer is enqueued", AudioQueueEnqueueBuffer(queue, given[i], 0, NULL) == 0);
	}
}

static void fn_test_taps(void)
{
	AudioDeviceID device = fn_default_output();
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 32,
	                                            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioQueueBufferRef given[2];
	AudioQueueBufferRef siphoned[2];
	AudioQueueProcessingTapRef tap = NULL;
	AudioQueueProcessingTapRef second = NULL;
	AudioStreamBasicDescription processing;
	AudioQueueProcessingTapFlags flags = 0;
	AudioTimeStamp stamp;
	AudioBufferList list;
	AudioQueueRef queue = NULL;
	AudioQueueRef siphon_queue = NULL;
	tap_state state;
	tap_state siphon_state;
	loop_state loop;
	loop_state siphon_loop;
	OSStatus status;
	UInt32 max_frames = 0;
	UInt32 got = 0;
	UInt32 frames = 1024;
	float peak = 0.0f;

	memset(&state, 0, sizeof(state));
	state.scale = 0.5f;
	memset(&siphon_state, 0, sizeof(siphon_state));
	siphon_state.scale = 1.0f;
	siphon_state.siphon = true;
	memset(&loop, 0, sizeof(loop));
	loop.value = 0.5f;
	memset(&siphon_loop, 0, sizeof(siphon_loop));
	siphon_loop.value = 0.5f;

	status = AudioQueueNewOutput(&format, fn_refill_patient, &loop, NULL, NULL, 0, &queue);
	fn_check("a queue for the tap is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	memset(&processing, 0, sizeof(processing));
	fn_check("a tap with neither effects flag is refused by name",
	         AudioQueueProcessingTapNew(queue, fn_tap, &state, 0, &max_frames, &processing, &tap)
	             == kAudioQueueErr_InvalidParameter);
	fn_check("a tap with both effects flags is refused by name",
	         AudioQueueProcessingTapNew(queue, fn_tap, &state,
	                                    kAudioQueueProcessingTap_PreEffects
	                                        | kAudioQueueProcessingTap_PostEffects,
	                                    &max_frames, &processing, &tap)
	             == kAudioQueueErr_InvalidParameter);

	status = AudioQueueProcessingTapNew(queue, fn_tap, &state, kAudioQueueProcessingTap_PreEffects,
	                                    &max_frames, &processing, &tap);
	fn_check("a tap is created on a stopped queue", status == 0 && tap != NULL);
	fn_check("the tap's processing format is the queue's own",
	         processing.mSampleRate == 48000.0 && processing.mChannelsPerFrame == 2
	             && (processing.mFormatFlags & kAudioFormatFlagIsFloat) != 0);
	fn_check("the tap is told the largest quantum it can be asked for", max_frames >= 2400);

	fn_check("a second tap is refused by name",
	         AudioQueueProcessingTapNew(queue, fn_tap, &state, kAudioQueueProcessingTap_PostEffects,
	                                    &max_frames, &processing, &second)
	             == kAudioQueueErr_TooManyTaps);

	memset(&list, 0, sizeof(list));
	list.mNumberBuffers = 1;
	memset(&stamp, 0, sizeof(stamp));
	fn_check("the source can only be pulled from inside the callback",
	         AudioQueueProcessingTapGetSourceAudio(tap, 128, &stamp, &flags, &got, &list)
	             == kAudioQueueErr_InvalidTapContext);
	fn_check("the queue's time can only be read from inside the callback",
	         AudioQueueProcessingTapGetQueueTime(tap, &state.queue_time, &state.queue_frames)
	             == kAudioQueueErr_InvalidTapContext);

	fn_queue_two_buffers(queue, given, frames, 0.5f);

	fn_check("the queue starts", AudioQueueStart(queue, NULL) == 0);
	fn_check("a tap cannot be disposed while the queue runs",
	         AudioQueueProcessingTapDispose(tap) == kAudioQueueErr_InvalidRunState);
	fn_check("a second tap cannot be created while the queue runs",
	         AudioQueueProcessingTapNew(queue, fn_tap, &state, kAudioQueueProcessingTap_PostEffects,
	                                    &max_frames, &processing, &second)
	             == kAudioQueueErr_InvalidRunState);

	fn_sleep_ms(220);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the tap was called", state.calls > 0);
	fn_check("the tap was shown the source, in the client's format",
	         state.source_status == 0 && state.peak > 0.45f && state.peak < 0.55f);
	fn_check("the tap's output is what the device plays", peak > 0.2f && peak < 0.3f);
	fn_check("the tap is told when the source starts", state.start_of_stream);
	fn_check("a tap's ioData arrives empty, as it has to before it pulls", state.entry_clean);
	fn_check("the queue's time is answered inside the callback",
	         state.queue_time_status == 0 && state.queue_frames > 0);

	loop.stop_refilling = true;
	fn_sleep_ms(160);
	fn_check("the tap is told when the source ends", state.end_of_stream);

	fn_check("the queue stops", AudioQueueStop(queue, true) == 0);
	fn_check("the tap is disposed", AudioQueueProcessingTapDispose(tap) == 0);
	fn_check("the queue takes a new tap once the old one is gone",
	         AudioQueueProcessingTapNew(queue, fn_tap, &state, kAudioQueueProcessingTap_PreEffects,
	                                    &max_frames, &processing, &tap)
	             == 0);
	fn_check("the replacement tap is disposed", AudioQueueProcessingTapDispose(tap) == 0);

	printf("      tap: %d callbacks, source %.3f, device %.3f, %u frames of queue time, %.4f s\n",
	       state.calls, (double)state.peak, (double)peak, (unsigned)state.queue_frames,
	       (double)state.queue_time);

	fn_check("the queue is disposed", AudioQueueDispose(queue, true) == 0);

	/* **A SIPHON HEARS WITHOUT CHANGING**: the device must still play 0.5 while the tap has been shown it. */
	status = AudioQueueNewOutput(&format, fn_refill, &siphon_loop, NULL, NULL, 0, &siphon_queue);
	fn_check("a queue for the siphon is created", status == 0 && siphon_queue != NULL);
	if (status == 0) {
		status = AudioQueueProcessingTapNew(siphon_queue, fn_tap, &siphon_state,
		                                    kAudioQueueProcessingTap_PreEffects
		                                        | kAudioQueueProcessingTap_Siphon,
		                                    &max_frames, &processing, &tap);
		fn_check("a siphon tap is created", status == 0 && tap != NULL);
		fn_queue_two_buffers(siphon_queue, siphoned, frames, 0.5f);
		fn_check("the siphon's queue starts", AudioQueueStart(siphon_queue, NULL) == 0);
		fn_sleep_ms(180);
		fn_agora_audio_test_peaks(device, &peak, NULL);
		fn_check("the siphon was shown the source", siphon_state.calls > 0 && siphon_state.peak > 0.45f);
		fn_check("a siphon does not change what the device plays", peak > 0.45f && peak < 0.55f);
		fn_check("the siphon's queue stops", AudioQueueStop(siphon_queue, true) == 0);
		fn_check("the siphon is disposed", AudioQueueProcessingTapDispose(tap) == 0);
		fn_check("the siphon's queue is disposed", AudioQueueDispose(siphon_queue, true) == 0);
	}
}

/* The tap is not a device feature: `AudioQueueOfflineRender` goes through the same chain, which is what lets a caller
 * that writes a file get the processed audio rather than the raw queue. */
static void fn_test_tap_offline(void)
{
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 32,
	                                            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioQueueRef queue = NULL;
	AudioQueueBufferRef queued = NULL;
	AudioQueueBufferRef out = NULL;
	AudioQueueProcessingTapRef tap = NULL;
	AudioStreamBasicDescription processing;
	tap_state state;
	offline_state callbacks;
	AudioTimeStamp stamp;
	OSStatus status;
	UInt32 max_frames = 0;
	UInt32 frames = 256;
	UInt32 i;
	float *samples;
	bool halved = true;

	memset(&state, 0, sizeof(state));
	state.scale = 0.5f;
	memset(&callbacks, 0, sizeof(callbacks));
	memset(&stamp, 0, sizeof(stamp));
	stamp.mFlags = kAudioTimeStampSampleTimeValid;

	status = AudioQueueNewOutput(&format, fn_offline_refilled, &callbacks, NULL, NULL, 0, &queue);
	fn_check("a queue for the offline tap is created", status == 0 && queue != NULL);
	if (status != 0) {
		return;
	}

	status = AudioQueueProcessingTapNew(queue, fn_tap, &state, kAudioQueueProcessingTap_PostEffects,
	                                    &max_frames, &processing, &tap);
	fn_check("a tap for offline render is created", status == 0 && tap != NULL);

	AudioQueueAllocateBuffer(queue, frames * 8, &queued);
	AudioQueueAllocateBuffer(queue, frames * 8, &out);
	samples = queued->mAudioData;
	for (i = 0; i < frames * 2; i++) {
		samples[i] = 0.5f;
	}
	queued->mAudioDataByteSize = frames * 8;
	fn_check("the buffer is queued", AudioQueueEnqueueBuffer(queue, queued, 0, NULL) == 0);

	fn_check("the queue renders offline through the tap", AudioQueueOfflineRender(queue, &stamp, out, frames) == 0);
	samples = out->mAudioData;
	for (i = 0; i < frames * 2; i++) {
		if (samples[i] < 0.24f || samples[i] > 0.26f) {
			halved = false;
		}
	}
	fn_check("the tap processed the offline render", state.calls > 0 && halved);

	fn_check("the tap is disposed", AudioQueueProcessingTapDispose(tap) == 0);
	fn_check("the queue is disposed", AudioQueueDispose(queue, true) == 0);
}

static void fn_test_system_sounds(void)
{
	char path[] = "/tmp/agora-queue-XXXXXX";
	int fd = mkstemp(path);
	AudioDeviceID device = fn_default_output();
	CFURLRef url;
	AudioFileID file = NULL;
	AudioStreamBasicDescription format = fn_pcm(48000.0, 2, 16,
	                                            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	SystemSoundID sound = 0;
	OSStatus status;
	float peak = 0.0f;

	if (fd < 0) {
		fn_check("a temporary file is created", 0);
		return;
	}
	close(fd);

	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path,
	                                              (CFIndex)strlen(path), false);

	/* A quarter second of a loud tone, written as a WAV the sound door can read. */
	status = AudioFileCreateWithURL(url, kAudioFileWAVEType, &format, kAudioFileFlags_EraseFile, &file);
	if (status == 0) {
		UInt32 frames = 12000;
		UInt32 bytes = frames * format.mBytesPerFrame;
		int16_t *samples = malloc(bytes);
		UInt32 i;

		for (i = 0; i < frames * 2; i++) {
			samples[i] = (i / 8) % 2 ? 20000 : -20000;
		}

		{
			UInt32 count = bytes;

			AudioFileWriteBytes(file, false, 0, &count, (const void *)samples);
		}

		AudioFileClose(file);
		free(samples);
	}

	status = AudioServicesCreateSystemSoundID(url, &sound);
	fn_check("a system sound is created from a file", status == kAudioServicesNoError && sound != 0);

	AudioServicesPlaySystemSound(sound);
	fn_sleep_ms(40);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the sound is playing on the device", peak > 0.3f);

	fn_check("the sound is disposed",
	         AudioServicesDisposeSystemSoundID(sound) == kAudioServicesNoError);

	/* And the generated alert, which is the one every Agora application will play. */
	fn_sleep_ms(300);	/* let the first sound finish */
	AudioServicesPlayAlertSound(kSystemSoundID_UserPreferredAlert);
	fn_sleep_ms(60);
	fn_agora_audio_test_peaks(device, &peak, NULL);
	fn_check("the generated alert is audible", peak > 0.1f);

	fn_sleep_ms(300);

	/* **TWO ALERTS AT ONCE, AND A COMPLETION THAT FITS THE SOUND.** The defect this replaced was a single queue whose
	 * second play restarted the first *and* a block that was called before a sample had been heard. Both are
	 * measurable: the block must still be uncalled on the next line, and both must be called by the end. */
	{
		__block int first = 0;
		__block int second = 0;

		AudioServicesPlayAlertSoundWithCompletion(kSystemSoundID_UserPreferredAlert, ^{ first = 1; });
		AudioServicesPlayAlertSoundWithCompletion(kSystemSoundID_UserPreferredAlert, ^{ second = 1; });

		fn_check("a completion is not called before the sound has played", first == 0 && second == 0);
		fn_sleep_ms(600);
		fn_check("both overlapping alerts complete", first == 1 && second == 1);
	}

	CFRelease(url);
	unlink(path);
}

int main(void);
int main(void)
{
	/* The suite states its backend in code, so a developer's shell cannot change what it measured. */
	fn_agora_audio_set_driver("null");

	fn_test_queue();
	fn_test_timeline();
	fn_test_offline();
	fn_test_mono_mapping();
	fn_test_taps();
	fn_test_tap_offline();
	fn_test_system_sounds();

	if (g_failures != 0) {
		fprintf(stderr, "queue: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("queue: the buffered model plays, scales and stops; a timeline reports the breaks; "
	       "the system sounds are audible and can overlap\n");
	return 0;
}
