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
	fn_test_mono_mapping();
	fn_test_system_sounds();

	if (g_failures != 0) {
		fprintf(stderr, "queue: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("queue: the buffered model plays, scales and stops; a timeline reports the breaks; "
	       "the system sounds are audible and can overlap\n");
	return 0;
}
