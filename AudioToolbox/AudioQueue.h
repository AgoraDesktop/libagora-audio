/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioQueue.h — the buffered playback model.
 *
 * THE MODEL IS A LOAN, NOT A STREAM. The caller allocates buffers, fills them, and hands them over; the queue
 * plays them and hands each one back through a callback so it can be filled again. That is why a queue is the
 * door a player uses: there is no read callback to get wrong, and the memory belongs to the caller, so a
 * dropped frame is a buffer that was never enqueued rather than a corruption inside a library.
 *
 * SPECIFIED BY macOS 10.12: the buffer structure (with its `const` members, which is why the implementation
 * keeps a mutable mirror and asserts the layouts agree), the callbacks, the parameters, the properties and
 * the error codes are the reference's.
 *
 * **VOLUME IS A PARAMETER HERE, NOT A PROPERTY** — `kAudioQueueParam_Volume`, set with
 * `AudioQueueSetParameter`. 10.12 has no `kAudioQueueProperty_Volume`, and this header does not invent one.
 *
 * WHAT IS IMPLEMENTED: output queues over the default output device, in linear PCM of any supported
 * description, resampled and converted to the device's format with `AudioConverter`. Input queues, timelines,
 * taps, offline rendering and parameter ramps are declared-and-refused by name: each is a real door on
 * macOS, and each needs machinery this phase does not have.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOQUEUE_H
#define AGORA_AUDIOTOOLBOX_AUDIOQUEUE_H

#include <AudioToolbox/AudioConverter.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFRunLoop.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef struct OpaqueAudioQueue *        AudioQueueRef;
typedef struct OpaqueAudioQueueTimeline * AudioQueueTimelineRef;
typedef struct OpaqueAudioQueueProcessingTap * AudioQueueProcessingTapRef;

typedef UInt32 AudioQueuePropertyID;
typedef UInt32 AudioQueueParameterID;
typedef Float32 AudioQueueParameterValue;
typedef UInt32 AudioQueueProcessingTapFlags;

struct AudioQueueBuffer
{
	const UInt32                    mAudioDataBytesCapacity;
	void * const _Nonnull           mAudioData;
	UInt32                          mAudioDataByteSize;
	void * _Nullable                mUserData;
	const UInt32                    mPacketDescriptionCapacity;
	AudioStreamPacketDescription * const _Nullable mPacketDescriptions;
	UInt32                          mPacketDescriptionCount;
};
typedef struct AudioQueueBuffer AudioQueueBuffer;
typedef AudioQueueBuffer *      AudioQueueBufferRef;

struct AudioQueueLevelMeterState
{
	Float32 mAveragePower;
	Float32 mPeakPower;
};
typedef struct AudioQueueLevelMeterState AudioQueueLevelMeterState;

struct AudioQueueParameterEvent
{
	AudioQueueParameterID    mID;
	AudioQueueParameterValue mValue;
};
typedef struct AudioQueueParameterEvent AudioQueueParameterEvent;

typedef void
(*AudioQueueOutputCallback)(void * _Nullable inUserData, AudioQueueRef _Nonnull inAQ,
                            AudioQueueBufferRef _Nonnull inBuffer);

typedef void
(^AudioQueueOutputCallbackBlock)(AudioQueueRef _Nonnull inAQ, AudioQueueBufferRef _Nonnull inBuffer);

typedef void
(*AudioQueueInputCallback)(void * _Nullable inUserData, AudioQueueRef _Nonnull inAQ,
                           AudioQueueBufferRef _Nonnull inBuffer, const AudioTimeStamp * _Nonnull inStartTime,
                           UInt32 inNumberPacketDescriptions,
                           const AudioStreamPacketDescription * _Nullable inPacketDescs);

typedef void
(^AudioQueueInputCallbackBlock)(AudioQueueRef _Nonnull inAQ, AudioQueueBufferRef _Nonnull inBuffer,
                                const AudioTimeStamp * _Nonnull inStartTime,
                                UInt32 inNumberPacketDescriptions,
                                const AudioStreamPacketDescription * _Nullable inPacketDescs);

typedef void
(*AudioQueueProcessingTapCallback)(void * _Nonnull inClientData, AudioQueueProcessingTapRef _Nonnull inAQTap,
                                   UInt32 inNumberFrames, AudioTimeStamp * _Nonnull ioTimeStamp,
                                   AudioQueueProcessingTapFlags * _Nonnull outFlags,
                                   UInt32 * _Nonnull outNumberFrames, AudioBufferList * _Nonnull ioData);

typedef void
(*AudioQueuePropertyListenerProc)(void * _Nullable inUserData, AudioQueueRef _Nonnull inAQ,
                                  AudioQueuePropertyID inID);

CF_ENUM(AudioQueuePropertyID)
{
	kAudioQueueProperty_IsRunning          = 'aqrn',
	kAudioQueueProperty_StreamDescription  = 'aqft',
	kAudioQueueProperty_CurrentDevice      = 'aqcd',
	kAudioQueueProperty_EnableLevelMetering = 'aqme',
	kAudioQueueProperty_CurrentLevelMeter  = 'aqmv',
	kAudioQueueProperty_CurrentLevelMeterDB = 'aqmd'
};

CF_ENUM(AudioQueueParameterID)
{
	kAudioQueueParam_Volume          = 1,
	kAudioQueueParam_VolumeRampTime  = 4
};

CF_ENUM(OSStatus)
{
	kAudioQueueErr_InvalidBuffer        = -66687,
	kAudioQueueErr_BufferEmpty          = -66686,
	kAudioQueueErr_DisposalPending      = -66685,
	kAudioQueueErr_InvalidProperty      = -66684,
	kAudioQueueErr_InvalidPropertySize  = -66683,
	kAudioQueueErr_InvalidPropertyValue = -66675,
	kAudioQueueErr_InvalidParameter     = -66682,
	kAudioQueueErr_CannotStart          = -66681,
	kAudioQueueErr_InvalidDevice        = -66680,
	kAudioQueueErr_BufferInQueue        = -66679,
	kAudioQueueErr_InvalidRunState      = -66678,
	kAudioQueueErr_BufferEnqueuedTwice  = -66666,
	kAudioQueueErr_EnqueueDuringReset   = -66632,
	kAudioQueueErr_InvalidOfflineMode   = -66626,
	kAudioQueueErr_InvalidTapContext    = -66669,
	kAudioQueueErr_InvalidTapType       = -66667
};

/* Output queues: the buffered model. */
extern OSStatus
AudioQueueNewOutput(const AudioStreamBasicDescription * inFormat,
                    AudioQueueOutputCallback          inCallbackProc,
                    void * _Nullable                  inUserData,
                    CFRunLoopRef _Nullable            inCallbackRunLoop,
                    CFStringRef _Nullable             inCallbackRunLoopMode,
                    UInt32                            inFlags,
                    AudioQueueRef _Nullable * _Nonnull outAQ);

extern OSStatus
AudioQueueNewOutputWithDispatchQueue(AudioQueueRef _Nullable * _Nonnull outAQ,
                                     const AudioStreamBasicDescription * inFormat,
                                     UInt32                            inFlags,
                                     dispatch_queue_t _Nonnull         inCallbackRunLoop,
                                     AudioQueueOutputCallbackBlock _Nonnull inCallbackProc);

/* Input queues: declared, refused (see the header comment). */
extern OSStatus
AudioQueueNewInput(const AudioStreamBasicDescription * inFormat,
                   AudioQueueInputCallback            inCallbackProc,
                   void * _Nullable                   inUserData,
                   CFRunLoopRef _Nullable             inCallbackRunLoop,
                   CFStringRef _Nullable              inCallbackRunLoopMode,
                   UInt32                             inFlags,
                   AudioQueueRef _Nullable * _Nonnull outAQ);

extern OSStatus
AudioQueueNewInputWithDispatchQueue(AudioQueueRef _Nullable * _Nonnull outAQ,
                                    const AudioStreamBasicDescription * inFormat,
                                    UInt32                            inFlags,
                                    dispatch_queue_t _Nonnull         inCallbackRunLoop,
                                    AudioQueueInputCallbackBlock _Nonnull inCallbackProc);

extern OSStatus
AudioQueueAllocateBuffer(AudioQueueRef inAQ, UInt32 inBufferByteSize,
                         AudioQueueBufferRef _Nullable * _Nonnull outBuffer);

extern OSStatus
AudioQueueAllocateBufferWithPacketDescriptions(AudioQueueRef inAQ, UInt32 inBufferByteSize,
                                               UInt32 inNumberPacketDescriptions,
                                               AudioQueueBufferRef _Nullable * _Nonnull outBuffer);

extern OSStatus
AudioQueueFreeBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);

extern OSStatus
AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                        UInt32 inNumPacketDescs, const AudioStreamPacketDescription * _Nullable inPacketDescs);

extern OSStatus
AudioQueueEnqueueBufferWithParameters(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                                      UInt32 inNumPacketDescs,
                                      const AudioStreamPacketDescription * _Nullable inPacketDescs,
                                      UInt32 inTrimFramesAtStart, UInt32 inTrimFramesAtEnd,
                                      UInt32 inNumParamEvents,
                                      const AudioQueueParameterEvent * _Nullable inParamEvents,
                                      const AudioTimeStamp * _Nullable inStartTime,
                                      AudioTimeStamp * _Nullable outActualStartTime);

extern OSStatus
AudioQueueStart(AudioQueueRef inAQ, const AudioTimeStamp * _Nullable inStartTime);

extern OSStatus
AudioQueuePrime(AudioQueueRef inAQ, UInt32 inNumberOfFramesToPrepare,
                UInt32 * _Nullable outNumberOfFramesPrepared);

extern OSStatus
AudioQueueStop(AudioQueueRef inAQ, Boolean inImmediate);

extern OSStatus
AudioQueuePause(AudioQueueRef inAQ);

extern OSStatus
AudioQueueFlush(AudioQueueRef inAQ);

extern OSStatus
AudioQueueReset(AudioQueueRef inAQ);

extern OSStatus
AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate);

/* Parameters (volume lives here) and properties. */
extern OSStatus
AudioQueueSetParameter(AudioQueueRef inAQ, AudioQueueParameterID inParamID, AudioQueueParameterValue inValue);

extern OSStatus
AudioQueueGetParameter(AudioQueueRef inAQ, AudioQueueParameterID inParamID,
                       AudioQueueParameterValue * outValue);

extern OSStatus
AudioQueueGetProperty(AudioQueueRef inAQ, AudioQueuePropertyID inID, void * outData, UInt32 * ioDataSize);

extern OSStatus
AudioQueueSetProperty(AudioQueueRef inAQ, AudioQueuePropertyID inID, const void * inData, UInt32 inDataSize);

extern OSStatus
AudioQueueGetPropertySize(AudioQueueRef inAQ, AudioQueuePropertyID inID, UInt32 * outDataSize);

extern OSStatus
AudioQueueAddPropertyListener(AudioQueueRef inAQ, AudioQueuePropertyID inID,
                              AudioQueuePropertyListenerProc inProc, void * _Nullable inUserData);

extern OSStatus
AudioQueueRemovePropertyListener(AudioQueueRef inAQ, AudioQueuePropertyID inID,
                                 AudioQueuePropertyListenerProc inProc, void * _Nullable inUserData);

/* The queue's timeline, against the device's clock. */
extern OSStatus
AudioQueueGetCurrentTime(AudioQueueRef inAQ, AudioQueueTimelineRef _Nullable inTimeline,
                         AudioTimeStamp * _Nullable outTimeStamp, Boolean * _Nullable outDiscontinuity);

/* The doors this phase declares and refuses, by name. */
extern OSStatus
AudioQueueOfflineRender(AudioQueueRef inAQ, const AudioTimeStamp * inTimestamp,
                        AudioQueueBufferRef ioBuffer, UInt32 inNumberFrames);

extern OSStatus
AudioQueueCreateTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef _Nullable * _Nonnull outTimeline);

extern OSStatus
AudioQueueDisposeTimeline(AudioQueueRef inAQ, AudioQueueTimelineRef inTimeline);

extern OSStatus
AudioQueueProcessingTapNew(AudioQueueRef inAQ, AudioQueueProcessingTapCallback inCallback,
                           void * _Nullable inClientData, AudioQueueProcessingTapFlags inFlags,
                           UInt32 * outMaxFrames, AudioStreamBasicDescription * outProcessingFormat,
                           AudioQueueProcessingTapRef _Nullable * _Nonnull outAQTap);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOQUEUE_H */
