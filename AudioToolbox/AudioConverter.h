/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioConverter.h — format conversion between two linear PCM descriptions.
 *
 * SPECIFIED BY macOS 10.12: the prototypes, the callbacks, `AudioConverterPrimeInfo`, and the selector,
 * error, quality, complexity and priming vocabularies are the reference header's, checked by
 * `tests/api/prototypes.c` and `tests/api/layout.c`.
 *
 * WHAT IT CONVERTS, THIS PHASE: linear PCM to linear PCM — sample format (signed and unsigned integers of
 * 8/16/24/32 bits, float 32 and 64), interleaving, channel count (identity map), and sample rate, with the
 * resampler in `AudioResampler.c`. A non-PCM format is refused with `kAudioConverterErr_FormatNotSupported`
 * because there is no codec here to hand it to: compressed formats arrive with the component registry
 * (phase 5), and a refusal that names the reason is the honest answer until then.
 *
 * THE TWO CALLING MODELS ARE BOTH HERE, because they are different questions. `AudioConverterConvertBuffer`
 * is one shot and requires no sample-rate change (Apple's own restriction: it has nowhere to keep the
 * resampler's state between calls). `AudioConverterFillComplexBuffer` pulls through a callback and keeps
 * that state, so it is the door a real-time chain uses.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOCONVERTER_H
#define AGORA_AUDIOTOOLBOX_AUDIOCONVERTER_H

#include <AudioToolbox/AudioFormat.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef UInt32 AudioConverterPropertyID;
typedef UInt32 AudioConverterPrimeMethod;
typedef struct OpaqueAudioConverter *   AudioConverterRef;

/* Fill `ioNumberDataPackets` frames from wherever the caller's audio comes from. Apple's contract: the
 * buffer list may point into the caller's own memory, and `outDataPacketDescription` may be NULL when the
 * input is constant-bit-rate. */
typedef OSStatus
(*AudioConverterComplexInputDataProc)(AudioConverterRef               inAudioConverter,
                                      UInt32 *                        ioNumberDataPackets,
                                      AudioBufferList *               ioData,
                                      AudioStreamPacketDescription * _Nullable * _Nullable outDataPacketDescription,
                                      void * _Nullable                inUserData);

/* The pre-10.4 shape of the same callback: raw bytes, no buffer list. Kept because
 * `AudioConverterFillBuffer` is still a real door in 10.12 and a client may be using it. */
typedef OSStatus
(*AudioConverterInputDataProc)(AudioConverterRef           inAudioConverter,
                               UInt32 *                    ioDataSize,
                               void * _Nonnull * _Nonnull  outData,
                               void * _Nullable            inUserData);

/* How much of the input the resampler needs before its first output frame is trustworthy. */
struct AudioConverterPrimeInfo {
    UInt32      leadingFrames;
    UInt32      trailingFrames;
};
typedef struct AudioConverterPrimeInfo  AudioConverterPrimeInfo;

CF_ENUM(OSStatus)
{
    kAudioConverterErr_FormatNotSupported           = 'fmt?',
    /* Apple writes this one as the four-char literal 'op??', whose trailing `??'` is a C trigraph. It is
     * spelled as the arithmetic for the same four characters: same value, no compiler extension. */
    kAudioConverterErr_OperationNotSupported        = (('o' << 24) | ('p' << 16) | ('?' << 8) | '?'),
    kAudioConverterErr_PropertyNotSupported         = 'prop',
    kAudioConverterErr_InvalidInputSize             = 'insz',
    kAudioConverterErr_InvalidOutputSize            = 'otsz',
    kAudioConverterErr_UnspecifiedError             = 'what',
    kAudioConverterErr_BadPropertySizeError         = '!siz',
    kAudioConverterErr_RequiresPacketDescriptionsError = '!pkd',
    kAudioConverterErr_InputSampleRateOutOfRange    = '!isr',
    kAudioConverterErr_OutputSampleRateOutOfRange   = '!osr'
};

/* The selectors this component answers. `kAudioConverterPropertyFormatList`, the codec parameters and the
 * encoder bit-rate tables belong to the codec and unit phases and are not declared as implemented here. */
CF_ENUM(AudioConverterPropertyID)
{
    kAudioConverterCurrentInputStreamDescription    = 'acid',
    kAudioConverterCurrentOutputStreamDescription   = 'acod',
    kAudioConverterPrimeMethod                      = 'prmm',
    kAudioConverterPrimeInfo                        = 'prim',
    kAudioConverterChannelMap                       = 'chmp',
    kAudioConverterSampleRateConverterQuality       = 'srcq',
    kAudioConverterSampleRateConverterComplexity    = 'srca',
    kAudioConverterSampleRateConverterInitialPhase  = 'srcp',
    kAudioConverterPropertyCalculateInputBufferSize = 'cibs',
    kAudioConverterPropertyCalculateOutputBufferSize = 'cobs',
    kAudioConverterPropertyMinimumInputBufferSize   = 'mibs',
    kAudioConverterPropertyMinimumOutputBufferSize  = 'mobs',
    kAudioConverterPropertyMaximumInputBufferSize   = 'xmib',
    kAudioConverterPropertyMaximumOutputPacketSize  = 'xmop'
};

/* Quality, as a number rather than a name, so a caller can ask for 73. */
CF_ENUM(UInt32)
{
    kAudioConverterQuality_Max      = 0x7F,
    kAudioConverterQuality_High     = 0x60,
    kAudioConverterQuality_Medium   = 0x40,
    kAudioConverterQuality_Low      = 0x20,
    kAudioConverterQuality_Min      = 0
};

CF_ENUM(UInt32)
{
    kAudioConverterSampleRateConverterComplexity_Linear      = 'line',
    kAudioConverterSampleRateConverterComplexity_Normal      = 'norm',
    kAudioConverterSampleRateConverterComplexity_Mastering   = 'bats',
    kAudioConverterSampleRateConverterComplexity_MinimumPhase = 'minp'
};

CF_ENUM(UInt32)
{
    kConverterPrimeMethod_Pre       = 0,
    kConverterPrimeMethod_Normal    = 1,
    kConverterPrimeMethod_None      = 2
};

/* Create a converter between two formats. The output description is the one the client will be handed
 * frames in; the input description is what it will be given. */
extern OSStatus
AudioConverterNew(const AudioStreamBasicDescription *  inSourceFormat,
                  const AudioStreamBasicDescription *  inDestinationFormat,
                  AudioConverterRef  _Nullable * _Nonnull outAudioConverter);

/* The same, restricted to the codecs named in `inClassDescriptions` — which here means "linear PCM or
 * nothing", because no other codec exists yet. */
extern OSStatus
AudioConverterNewSpecific(const AudioStreamBasicDescription *  inSourceFormat,
                          const AudioStreamBasicDescription *  inDestinationFormat,
                          UInt32                               inNumberClassDescriptions,
                          const AudioClassDescription *        inClassDescriptions,
                          AudioConverterRef  _Nullable * _Nonnull outAudioConverter);

extern OSStatus
AudioConverterDispose(AudioConverterRef inAudioConverter);

/* Drop everything buffered: the resampler's history and phase, and the prime. A seek calls this. */
extern OSStatus
AudioConverterReset(AudioConverterRef inAudioConverter);

extern OSStatus
AudioConverterGetPropertyInfo(AudioConverterRef       inAudioConverter,
                              AudioConverterPropertyID inPropertyID,
                              UInt32 * _Nullable      outSize,
                              Boolean * _Nullable     outWritable);

extern OSStatus
AudioConverterGetProperty(AudioConverterRef       inAudioConverter,
                          AudioConverterPropertyID inPropertyID,
                          UInt32 *                ioPropertyDataSize,
                          void *                  outPropertyData);

extern OSStatus
AudioConverterSetProperty(AudioConverterRef       inAudioConverter,
                          AudioConverterPropertyID inPropertyID,
                          UInt32                   inPropertyDataSize,
                          const void *             inPropertyData);

/* One shot. Frames in, frames out, no sample-rate change — a rate change needs state the caller does not
 * have anywhere to put. */
extern OSStatus
AudioConverterConvertBuffer(AudioConverterRef   inAudioConverter,
                            UInt32              inInputDataSize,
                            const void *        inInputData,
                            UInt32 *            ioOutputDataSize,
                            void *              outOutputData);

/* The buffered model: `ioOutputData` says how many frames are wanted and is updated with how many came
 * back; the input callback is called as often as the converter needs to fill it. */
extern OSStatus
AudioConverterFillComplexBuffer(AudioConverterRef                   inAudioConverter,
                                AudioConverterComplexInputDataProc  inInputDataProc,
                                void * _Nullable                    inInputDataProcUserData,
                                UInt32 *                            ioOutputDataPacketSize,
                                AudioBufferList *                   outOutputData,
                                AudioStreamPacketDescription * _Nullable outPacketDescription);

/* The pre-10.4 door: bytes rather than frames. Same conversion, a buffer the converter allocates. */
extern OSStatus
AudioConverterFillBuffer(AudioConverterRef           inAudioConverter,
                         AudioConverterInputDataProc inInputDataProc,
                         void * _Nullable            inInputDataProcUserData,
                         UInt32 *                    ioOutputDataSize,
                         void *                      outOutputData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOCONVERTER_H */
