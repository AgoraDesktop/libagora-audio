/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioServices.c — the system sounds, over the queue.
 *
 * A SYSTEM SOUND IS A FILE AND A NUMBER. `AudioServicesCreateSystemSoundID` reads the whole file at creation
 * time — a buzzer is short, and reading it once is what makes playing it a memory copy rather than a parse on
 * the audio thread — and `AudioServicesPlaySystemSound` hands a buffer holding it to a queue, which plays and
 * forgets it.
 *
 * THE ALERT IS GENERATED, NOT VENDORED. `kSystemSoundID_UserPreferredAlert` is a short tone built from a sine
 * with a decaying envelope: 0.25 s at 44.1 kHz, stereo, float. That is a sound a person hears as a beep, it
 * needs no asset and no licence, and `tests/queue.c` can measure it rather than trusting that it plays.
 *
 * ONE QUEUE, REUSED, BECAUSE A BUZZER IS NOT A MIXER. Two alerts in a row restart the queue — the second one
 * is what the user asked to hear — and the buffer of the first is freed by the completion path rather than
 * leaked.
 */

#include <AudioToolbox/AudioServices.h>
#include <AudioToolbox/AudioQueue.h>
#include "AudioToolbox_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define AGORA_MAX_SOUNDS 32

typedef struct {
	bool                        active;
	SystemSoundID               id;
	uint8_t *                   bytes;
	UInt32                      length;
	AudioStreamBasicDescription format;
	AudioServicesSystemSoundCompletionProc completion;
	void *                      completion_client;
} agora_system_sound;

typedef struct {
	pthread_mutex_t    lock;
	agora_system_sound sounds[AGORA_MAX_SOUNDS];
	SystemSoundID       next_id;
	AudioQueueRef       queue;
	AudioStreamBasicDescription queue_format;
	AudioQueueBufferRef playing;
} agora_services;

static agora_services g_services = { .lock = PTHREAD_MUTEX_INITIALIZER, .next_id = 0x1001 };

static agora_system_sound *fn_sound(SystemSoundID id)
{
	UInt32 i;

	for (i = 0; i < AGORA_MAX_SOUNDS; i++) {
		if (g_services.sounds[i].active && g_services.sounds[i].id == id) {
			return &g_services.sounds[i];
		}
	}

	return NULL;
}

/* The buffer is ours, so it is freed here and the queue is reset for the next sound. */
static void fn_played(void *client, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	(void)client;
	(void)queue;

	if (buffer != NULL) {
		AudioQueueFreeBuffer(queue, buffer);
	}
}

static OSStatus fn_play_locked(const agora_system_sound *sound, void (^completion)(void))
{
	AudioQueueBufferRef buffer = NULL;
	OSStatus status;

	if (g_services.queue != NULL
	    && memcmp(&g_services.queue_format, &sound->format, sizeof(AudioStreamBasicDescription)) != 0) {
		AudioQueueDispose(g_services.queue, true);
		g_services.queue = NULL;
	}

	if (g_services.queue == NULL) {
		status = AudioQueueNewOutput(&sound->format, fn_played, NULL, NULL, NULL, 0, &g_services.queue);
		if (status != 0) {
			return kAudioServicesSystemSoundUnspecifiedError;
		}
		g_services.queue_format = sound->format;
	}

	status = AudioQueueAllocateBuffer(g_services.queue, sound->length, &buffer);
	if (status != 0) {
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	memcpy(buffer->mAudioData, sound->bytes, sound->length);
	buffer->mAudioDataByteSize = sound->length;

	status = AudioQueueEnqueueBuffer(g_services.queue, buffer, 0, NULL);
	if (status != 0) {
		AudioQueueFreeBuffer(g_services.queue, buffer);
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	status = AudioQueueStart(g_services.queue, NULL);

	if (status != 0) {
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	if (completion != NULL) {
		completion();
	}

	return kAudioServicesNoError;
}

/* The alert tone: a 1.5 kHz sine over 0.22 s with a fast attack and an exponential decay. Generated into a
 * buffer of float32 stereo at 44.1 kHz, which is the format the sound system uses for its own sounds. */
static void fn_build_alert(agora_system_sound *sound, uint8_t *bytes)
{
	const double rate = 44100.0;
	const double frequency = 1500.0;
	const double seconds = 0.22;
	UInt32 frames = (UInt32)(rate * seconds);
	float *samples = (float *)bytes;
	UInt32 i;

	for (i = 0; i < frames; i++) {
		double t = (double)i / rate;
		double attack = (t < 0.004) ? (t / 0.004) : 1.0;
		double decay = exp(-t * 18.0);
		float value = (float)(0.35 * attack * decay * sin(2.0 * 3.14159265358979323846 * frequency * t));

		samples[i * 2 + 0] = value;
		samples[i * 2 + 1] = value;
	}

	sound->format.mSampleRate = rate;
	sound->format.mFormatID = kAudioFormatLinearPCM;
	sound->format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeFloatPacked;
	sound->format.mChannelsPerFrame = 2;
	sound->format.mBitsPerChannel = 32;
	sound->format.mBytesPerFrame = 8;
	sound->format.mBytesPerPacket = 8;
	sound->format.mFramesPerPacket = 1;
	sound->length = frames * 8;
}

OSStatus AudioServicesCreateSystemSoundID(CFURLRef inFileURL, SystemSoundID *outSystemSoundID)
{
	AudioFileID file = NULL;
	AudioStreamBasicDescription format;
	UInt64 byte_count = 0;
	UInt32 size;
	OSStatus status;
	agora_system_sound *slot = NULL;
	UInt32 i;
	uint8_t *bytes;

	if (inFileURL == NULL || outSystemSoundID == NULL) {
		return kAudio_ParamError;
	}

	status = AudioFileOpenURL(inFileURL, kAudioFileReadPermission, 0, &file);
	if (status != 0) {
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	size = sizeof(format);
	status = AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format);
	if (status != 0) {
		AudioFileClose(file);
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	size = sizeof(byte_count);
	status = AudioFileGetProperty(file, kAudioFilePropertyAudioDataByteCount, &size, &byte_count);
	if (status != 0 || byte_count == 0 || byte_count > (64u * 1024u * 1024u)) {
		/* A system sound is short; anything else is not one. */
		AudioFileClose(file);
		return kAudioServicesSystemSoundExceededMaximumDurationError;
	}

	bytes = malloc((size_t)byte_count);
	if (bytes == NULL) {
		AudioFileClose(file);
		return kAudio_MemFullError;
	}

	{
		UInt32 wanted = (UInt32)byte_count;

		status = AudioFileReadBytes(file, false, 0, &wanted, bytes);
		if (status != 0 || wanted != byte_count) {
			free(bytes);
			AudioFileClose(file);
			return kAudioServicesSystemSoundUnspecifiedError;
		}
	}

	AudioFileClose(file);

	pthread_mutex_lock(&g_services.lock);

	for (i = 0; i < AGORA_MAX_SOUNDS; i++) {
		if (!g_services.sounds[i].active) {
			slot = &g_services.sounds[i];
			break;
		}
	}

	if (slot == NULL) {
		pthread_mutex_unlock(&g_services.lock);
		free(bytes);
		return kAudioServicesSystemSoundUnspecifiedError;
	}

	memset(slot, 0, sizeof(*slot));
	slot->active = true;
	slot->id = g_services.next_id++;
	slot->bytes = bytes;
	slot->length = (UInt32)byte_count;
	slot->format = format;

	*outSystemSoundID = slot->id;
	pthread_mutex_unlock(&g_services.lock);

	return kAudioServicesNoError;
}

OSStatus AudioServicesDisposeSystemSoundID(SystemSoundID inSystemSoundID)
{
	agora_system_sound *sound;

	pthread_mutex_lock(&g_services.lock);
	sound = fn_sound(inSystemSoundID);
	if (sound != NULL) {
		free(sound->bytes);
		memset(sound, 0, sizeof(*sound));
	}
	pthread_mutex_unlock(&g_services.lock);

	return sound != NULL ? kAudioServicesNoError : kAudioServicesSystemSoundUnspecifiedError;
}

static OSStatus fn_play_id(SystemSoundID inSystemSoundID, bool alert, void (^completion)(void))
{
	agora_system_sound *sound;
	OSStatus status;
	agora_system_sound generated;

	if (inSystemSoundID == kSystemSoundID_UserPreferredAlert || inSystemSoundID == kSystemSoundID_FlashScreen) {
		UInt32 frames = (UInt32)(44100.0 * 0.22);

		memset(&generated, 0, sizeof(generated));
		generated.active = true;
		generated.bytes = malloc((size_t)frames * 8);
		if (generated.bytes == NULL) {
			return kAudioServicesSystemSoundUnspecifiedError;
		}
		fn_build_alert(&generated, generated.bytes);

		pthread_mutex_lock(&g_services.lock);
		status = fn_play_locked(&generated, completion);
		pthread_mutex_unlock(&g_services.lock);
		free(generated.bytes);
		return status;
	}

	pthread_mutex_lock(&g_services.lock);
	sound = fn_sound(inSystemSoundID);
	status = (sound != NULL) ? fn_play_locked(sound, completion) : kAudioServicesSystemSoundUnspecifiedError;
	pthread_mutex_unlock(&g_services.lock);

	(void)alert;
	return status;
}

void AudioServicesPlaySystemSound(SystemSoundID inSystemSoundID)
{
	(void)fn_play_id(inSystemSoundID, false, NULL);
}

void AudioServicesPlayAlertSound(SystemSoundID inSystemSoundID)
{
	(void)fn_play_id(inSystemSoundID, true, NULL);
}

void AudioServicesPlaySystemSoundWithCompletion(SystemSoundID inSystemSoundID, void (^inCompletionBlock)(void))
{
	(void)fn_play_id(inSystemSoundID, false, inCompletionBlock);
}

void AudioServicesPlayAlertSoundWithCompletion(SystemSoundID inSystemSoundID, void (^inCompletionBlock)(void))
{
	(void)fn_play_id(inSystemSoundID, true, inCompletionBlock);
}

OSStatus AudioServicesAddSystemSoundCompletion(SystemSoundID inSystemSoundID, CFRunLoopRef inRunLoop,
                                               CFStringRef inRunLoopMode,
                                               AudioServicesSystemSoundCompletionProc inCompletionRoutine,
                                               void *inClientData)
{
	agora_system_sound *sound;

	(void)inRunLoop;
	(void)inRunLoopMode;

	if (inCompletionRoutine == NULL) {
		return kAudio_ParamError;
	}

	pthread_mutex_lock(&g_services.lock);
	sound = fn_sound(inSystemSoundID);
	if (sound != NULL) {
		sound->completion = inCompletionRoutine;
		sound->completion_client = inClientData;
	}
	pthread_mutex_unlock(&g_services.lock);

	return sound != NULL ? kAudioServicesNoError : kAudioServicesSystemSoundUnspecifiedError;
}

void AudioServicesRemoveSystemSoundCompletion(SystemSoundID inSystemSoundID)
{
	agora_system_sound *sound;

	pthread_mutex_lock(&g_services.lock);
	sound = fn_sound(inSystemSoundID);
	if (sound != NULL) {
		sound->completion = NULL;
		sound->completion_client = NULL;
	}
	pthread_mutex_unlock(&g_services.lock);
}

OSStatus AudioServicesGetPropertyInfo(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                                      const void *inSpecifier, UInt32 *outPropertyDataSize,
                                      Boolean *outWritable)
{
	(void)inPropertyID;
	(void)inSpecifierSize;
	(void)inSpecifier;
	(void)outPropertyDataSize;
	(void)outWritable;

	return kAudioServicesUnsupportedPropertyError;
}

OSStatus AudioServicesGetProperty(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                                  const void *inSpecifier, UInt32 *ioPropertyDataSize, void *outPropertyData)
{
	(void)inPropertyID;
	(void)inSpecifierSize;
	(void)inSpecifier;
	(void)ioPropertyDataSize;
	(void)outPropertyData;

	return kAudioServicesUnsupportedPropertyError;
}

OSStatus AudioServicesSetProperty(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                                  const void *inSpecifier, UInt32 inPropertyDataSize, const void *inPropertyData)
{
	(void)inPropertyID;
	(void)inSpecifierSize;
	(void)inSpecifier;
	(void)inPropertyDataSize;
	(void)inPropertyData;

	return kAudioServicesUnsupportedPropertyError;
}
