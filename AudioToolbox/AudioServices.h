/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioServices.h — the alert sound.
 *
 * ONE SOUND, PLAYED AT A SYSTEM EVENT, AND NO APPARATUS AROUND IT. A "system sound" is a short file loaded by
 * URL and played by number; the alert sound is one the system already knows. Everything here is fire-and-
 * forget, which is the point: a buzzer that could fail loudly would be a buzzer nobody calls.
 *
 * `kSystemSoundID_UserPreferredAlert` IS ANSWERED FROM A GENERATED TONE, NOT A VENDORED FILE. It is a short
 * decaying sine, built in memory at 44.1 kHz — so the installation carries no unlicensed audio, the tone is
 * testable by value, and a user who wants a different alert sets one with `AudioServicesCreateSystemSoundID`.
 *
 * SPECIFIED BY macOS 10.12: `SystemSoundID`, the ids, the completion callbacks and the error codes are the
 * reference's.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOSERVICES_H
#define AGORA_AUDIOTOOLBOX_AUDIOSERVICES_H

#include <AudioToolbox/AudioFile.h>
#include <CoreFoundation/CFRunLoop.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef UInt32 SystemSoundID;
typedef UInt32 AudioServicesPropertyID;

typedef void
(*AudioServicesSystemSoundCompletionProc)(SystemSoundID inSystemSoundID, void * _Nullable inClientData);

CF_ENUM(UInt32)
{
	kSystemSoundID_UserPreferredAlert  = 0x1000,
	kSystemSoundID_FlashScreen         = 0x0FFE
};

CF_ENUM(OSStatus)
{
	kAudioServicesNoError                            = 0,
	kAudioServicesUnsupportedPropertyError           = 'pty?',
	kAudioServicesBadPropertySizeError               = '!siz',
	kAudioServicesBadSpecifierSizeError              = '!spc',
	kAudioServicesSystemSoundUnspecifiedError        = -1500,
	kAudioServicesSystemSoundClientTimedOutError     = -1501,
	kAudioServicesSystemSoundExceededMaximumDurationError = -1502
};

/* Load a sound from a file, and forget it. */
extern OSStatus
AudioServicesCreateSystemSoundID(CFURLRef inFileURL, SystemSoundID * outSystemSoundID);

extern OSStatus
AudioServicesDisposeSystemSoundID(SystemSoundID inSystemSoundID);

/* Play it. Returns immediately; the sound is mixed over whatever else is playing. */
extern void
AudioServicesPlaySystemSound(SystemSoundID inSystemSoundID);

extern void
AudioServicesPlayAlertSound(SystemSoundID inSystemSoundID);

extern void
AudioServicesPlaySystemSoundWithCompletion(SystemSoundID inSystemSoundID, void (^ _Nullable inCompletionBlock)(void));

extern void
AudioServicesPlayAlertSoundWithCompletion(SystemSoundID inSystemSoundID, void (^ _Nullable inCompletionBlock)(void));

extern OSStatus
AudioServicesAddSystemSoundCompletion(SystemSoundID inSystemSoundID, CFRunLoopRef _Nullable inRunLoop,
                                      CFStringRef _Nullable inRunLoopMode,
                                      AudioServicesSystemSoundCompletionProc inCompletionRoutine,
                                      void * _Nullable inClientData);

extern void
AudioServicesRemoveSystemSoundCompletion(SystemSoundID inSystemSoundID);

extern OSStatus
AudioServicesGetPropertyInfo(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                             const void * _Nullable inSpecifier, UInt32 * _Nullable outPropertyDataSize,
                             Boolean * _Nullable outWritable);

extern OSStatus
AudioServicesGetProperty(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                         const void * _Nullable inSpecifier, UInt32 * ioPropertyDataSize,
                         void * _Nullable outPropertyData);

extern OSStatus
AudioServicesSetProperty(AudioServicesPropertyID inPropertyID, UInt32 inSpecifierSize,
                         const void * _Nullable inSpecifier, UInt32 inPropertyDataSize,
                         const void * inPropertyData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOSERVICES_H */
