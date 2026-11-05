/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * CoreAudio/AudioHardware.h — the device layer.
 *
 * FIVE FUNCTIONS DESCRIBE EVERY OBJECT, AND FOUR DRIVE A DEVICE. That is the whole shape: ask an object for
 * a property, set one, ask whether it has one, ask how big the answer is, and listen for changes; then
 * attach an io callback to a device, start it, stop it, and let it tell you what time it is.
 *
 * SPECIFIED BY macOS 10.12: every prototype below is the reference's, including the `_Nullable` io data
 * pointers (a device with no input has no input data) and the `_Nullable` io proc id (`NULL` on Start means
 * "every proc attached to this device"). `tests/api/prototypes.c` is what keeps them that way.
 *
 * THE DEVICE'S DATA THREAD IS THE DRIVER'S, NOT OURS: an io proc is called on the thread that produces the
 * audio, which for the null backend is a thread this library owns and for a real backend will be the
 * sound server's. Nothing in the callback may allocate or take a lock, for the same reason it may not on
 * macOS.
 */
#ifndef AGORA_COREAUDIO_AUDIOHARDWARE_H
#define AGORA_COREAUDIO_AUDIOHARDWARE_H

#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudio/HostTime.h>
#include <dispatch/dispatch.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

/* --- every object ---------------------------------------------------------- */

extern OSStatus
AudioObjectGetPropertyData(AudioObjectID                inObjectID,
                           const AudioObjectPropertyAddress * inAddress,
                           UInt32                      inQualifierDataSize,
                           const void * _Nullable      inQualifierData,
                           UInt32 *                    ioDataSize,
                           void *                      outData);

extern OSStatus
AudioObjectSetPropertyData(AudioObjectID                inObjectID,
                           const AudioObjectPropertyAddress * inAddress,
                           UInt32                      inQualifierDataSize,
                           const void * _Nullable      inQualifierData,
                           UInt32                      inDataSize,
                           const void *                inData);

extern Boolean
AudioObjectHasProperty(AudioObjectID                       inObjectID,
                       const AudioObjectPropertyAddress *  inAddress);

extern OSStatus
AudioObjectIsPropertySettable(AudioObjectID                       inObjectID,
                              const AudioObjectPropertyAddress *  inAddress,
                              Boolean *                           outIsSettable);

extern OSStatus
AudioObjectGetPropertyDataSize(AudioObjectID                       inObjectID,
                               const AudioObjectPropertyAddress *  inAddress,
                               UInt32                              inQualifierDataSize,
                               const void * _Nullable              inQualifierData,
                               UInt32 *                            outDataSize);

extern OSStatus
AudioObjectAddPropertyListener(AudioObjectID                     inObjectID,
                               const AudioObjectPropertyAddress * inAddress,
                               AudioObjectPropertyListenerProc    inListener,
                               void * _Nullable                  inClientData);

extern OSStatus
AudioObjectRemovePropertyListener(AudioObjectID                     inObjectID,
                                  const AudioObjectPropertyAddress * inAddress,
                                  AudioObjectPropertyListenerProc    inListener,
                                  void * _Nullable                  inClientData);

extern OSStatus
AudioObjectAddPropertyListenerBlock(AudioObjectID                     inObjectID,
                                    const AudioObjectPropertyAddress * inAddress,
                                    dispatch_queue_t _Nullable         inDispatchQueue,
                                    AudioObjectPropertyListenerBlock   inListener);

extern OSStatus
AudioObjectRemovePropertyListenerBlock(AudioObjectID                     inObjectID,
                                       const AudioObjectPropertyAddress * inAddress,
                                       dispatch_queue_t _Nullable        inDispatchQueue,
                                       AudioObjectPropertyListenerBlock  inListener);

/* --- a device -------------------------------------------------------------- */

extern OSStatus
AudioDeviceCreateIOProcID(AudioObjectID              inDevice,
                          AudioDeviceIOProc          inProc,
                          void * _Nullable           inClientData,
                          AudioDeviceIOProcID _Nullable * _Nonnull outIOProcID);

extern OSStatus
AudioDeviceCreateIOProcIDWithBlock(AudioDeviceIOProcID _Nullable * _Nonnull outIOProcID,
                                   AudioObjectID                   inDevice,
                                   dispatch_queue_t _Nullable      inDispatchQueue,
                                   AudioDeviceIOBlock              inIOBlock);

extern OSStatus
AudioDeviceDestroyIOProcID(AudioObjectID inDevice, AudioDeviceIOProcID inIOProcID);

extern OSStatus
AudioDeviceStart(AudioObjectID inDevice, AudioDeviceIOProcID _Nullable inProcID);

extern OSStatus
AudioDeviceStop(AudioObjectID inDevice, AudioDeviceIOProcID _Nullable inProcID);

extern OSStatus
AudioDeviceGetCurrentTime(AudioObjectID inDevice, AudioTimeStamp * outTime);

extern OSStatus
AudioDeviceTranslateTime(AudioObjectID inDevice, const AudioTimeStamp * inTime,
                         AudioTimeStamp * outTime);

extern OSStatus
AudioDeviceGetNearestStartTime(AudioObjectID inDevice, AudioTimeStamp * ioRequestedStartTime,
                               UInt32 inFlags);

/* --- the deprecated device doors ------------------------------------------- */
/* Declared, and refused: the modern equivalents are the five object functions above. Keeping the names is
 * what lets a client compiled against 10.12 link and be told which door to use. */

extern OSStatus
AudioDeviceAddIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc, void * _Nullable inClientData);

extern OSStatus
AudioDeviceRemoveIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc);

extern OSStatus
AudioDeviceGetProperty(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput,
                       AudioDevicePropertyID inPropertyID, UInt32 * ioPropertyDataSize,
                       void * outPropertyData);

extern OSStatus
AudioDeviceSetProperty(AudioDeviceID inDevice, const AudioTimeStamp * _Nullable inWhen,
                       UInt32 inChannel, Boolean isInput, AudioDevicePropertyID inPropertyID,
                       UInt32 inPropertyDataSize, const void * inPropertyData);

extern OSStatus
AudioDeviceGetPropertyInfo(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput,
                           AudioDevicePropertyID inPropertyID, UInt32 * _Nullable outSize,
                           Boolean * _Nullable outWritable);

extern OSStatus
AudioStreamGetProperty(AudioStreamID inStream, UInt32 inChannel, AudioDevicePropertyID inPropertyID,
                       UInt32 * ioPropertyDataSize, void * outPropertyData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_COREAUDIO_AUDIOHARDWARE_H */
