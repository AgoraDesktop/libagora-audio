/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * CoreAudio/CoreAudioTypes.h — the vocabulary every audio format in the system is described in.
 *
 * **THIS HEADER IS SPECIFIED BY macOS 10.12, NOT INSPIRED BY IT.** The structures below are field for
 * field, offset for offset, the ones in `MacOSX10.12.sdk`; the constants are its constants, down to the
 * four-char code. The measurements they were written from are committed at
 * `tests/api/manifest-10.12.txt`, and `tests/api/layout.c` fails the build if one of them drifts. See
 * `../AUDIO-PLAN.md` §1 for what "identical" covers and what it does not.
 *
 * WHAT IS HERE IS THE PART SOMETHING USES: the structures, the time types, the flag vocabulary, the
 * error codes, and the format IDs and channel layouts the file and device layers speak. The full
 * tables — every `AudioFormatID`, every `AudioChannelLayoutTag` — are in the manifest already and are
 * transcribed by the phase that first needs them, which is cheaper than carrying dead constants and a
 * lie that they are measured.
 */
#ifndef AGORA_COREAUDIO_COREAUDIOTYPES_H
#define AGORA_COREAUDIO_COREAUDIOTYPES_H

#include <CoreFoundation/CFBase.h>

/* Apple's header includes <string.h> for C++ only, because only the C++ inline helpers below need it
 * (they `memset` an `SMPTETime`). Kept exactly that way: a C client must not acquire <string.h> from
 * including an audio header. */
#if defined(__cplusplus)
    #include <string.h>
#endif

CF_ASSUME_NONNULL_BEGIN

/* Apple's own deprecation decoration for the "canonical" format flags: kept, marked, and not used by
 * anything here. */
#if defined(__cplusplus)
    #define CA_CANONICAL_DEPRECATED __attribute__((deprecated("The concept of canonical formats is deprecated")))
#else
    #define CA_CANONICAL_DEPRECATED
#endif

#pragma mark - OSStatus codes
/* The status values this framework returns. -4, -43, -54, -42, -50 and -108 are the aliases Core
 * Audio gives to the classic Mac OS errors; '!pth' is its own. */
CF_ENUM(OSStatus)
{
    kAudio_UnimplementedError     = -4,
    kAudio_FileNotFoundError      = -43,
    kAudio_FilePermissionError    = -54,
    kAudio_TooManyFilesOpenError  = -42,
    kAudio_BadFilePathError       = '!pth',
    kAudio_ParamError             = -50,
    kAudio_MemFullError           = -108
};

#pragma mark - Values
struct AudioValueRange
{
    Float64 mMinimum;
    Float64 mMaximum;
};
typedef struct AudioValueRange  AudioValueRange;

struct AudioValueTranslation
{
    void* _Nonnull  mInputData;
    UInt32          mInputDataSize;
    void* _Nonnull  mOutputData;
    UInt32          mOutputDataSize;
};
typedef struct AudioValueTranslation    AudioValueTranslation;

#pragma mark - Buffers
struct AudioBuffer
{
    UInt32              mNumberChannels;
    UInt32              mDataByteSize;
    void* _Nullable     mData;
};
typedef struct AudioBuffer  AudioBuffer;

/* A VARIABLE LENGTH ARRAY, AND THE `[1]` IS NOT A TYPO: Apple's structure has one buffer in it and a
 * caller with N buffers allocates `sizeof(AudioBufferList) + (N - 1) * sizeof(AudioBuffer)`. The
 * layout is checked by `tests/api/layout.c`; changing it breaks every client at once. */
struct AudioBufferList
{
    UInt32      mNumberBuffers;
    AudioBuffer mBuffers[1];
};
typedef struct AudioBufferList  AudioBufferList;

#pragma mark - Formats
/* The canonical sample types. Deprecated on Apple's side and kept here because they are part of the
 * typedef set a client compiled against 10.12 may name. */
typedef Float32 AudioSampleType;
typedef Float32 AudioUnitSampleType;

/* The two format typedefs are Apple's, and they are declared before the table grouped by them. */
typedef UInt32 AudioFormatID;
typedef UInt32 AudioFormatFlags;

/* Apple's fixed-point fraction width, named by the canonical flag combination below. */
#define kAudioUnitSampleFractionBits 24

struct AudioStreamBasicDescription
{
    Float64             mSampleRate;
    AudioFormatID       mFormatID;
    AudioFormatFlags    mFormatFlags;
    UInt32              mBytesPerPacket;
    UInt32              mFramesPerPacket;
    UInt32              mBytesPerFrame;
    UInt32              mChannelsPerFrame;
    UInt32              mBitsPerChannel;
    UInt32              mReserved;
};
typedef struct AudioStreamBasicDescription  AudioStreamBasicDescription;

/* 0.0 says "any rate" and only ever appears in a listing of what a device supports, never in one. */
static const Float64 kAudioStreamAnyRate = 0.0;

/* The format IDs this component reads, or refuses by name. Apple's table is much longer; the manifest
 * holds all of it. */
CF_ENUM(AudioFormatID)
{
    kAudioFormatLinearPCM               = 'lpcm',
    kAudioFormatMPEG4AAC                = 'aac ',
    kAudioFormatMPEGLayer3              = '.mp3',
    kAudioFormatAppleLossless           = 'alac'
};

CF_ENUM(AudioFormatFlags)
{
    kAudioFormatFlagIsFloat                     = (1U << 0),
    kAudioFormatFlagIsBigEndian                 = (1U << 1),
    kAudioFormatFlagIsSignedInteger             = (1U << 2),
    kAudioFormatFlagIsPacked                    = (1U << 3),
    kAudioFormatFlagIsAlignedHigh               = (1U << 4),
    kAudioFormatFlagIsNonInterleaved            = (1U << 5),
    kAudioFormatFlagIsNonMixable                = (1U << 6),
    kAudioFormatFlagsAreAllClear                = 0x80000000,
    kLinearPCMFormatFlagIsFloat                 = kAudioFormatFlagIsFloat,
    kLinearPCMFormatFlagIsBigEndian             = kAudioFormatFlagIsBigEndian,
    kLinearPCMFormatFlagIsSignedInteger         = kAudioFormatFlagIsSignedInteger,
    kLinearPCMFormatFlagIsPacked                = kAudioFormatFlagIsPacked,
    kLinearPCMFormatFlagIsAlignedHigh           = kAudioFormatFlagIsAlignedHigh,
    kLinearPCMFormatFlagIsNonInterleaved        = kAudioFormatFlagIsNonInterleaved,
    kLinearPCMFormatFlagIsNonMixable            = kAudioFormatFlagIsNonMixable,
    kLinearPCMFormatFlagsSampleFractionShift    = 7,
    kLinearPCMFormatFlagsSampleFractionMask     = (0x3F << kLinearPCMFormatFlagsSampleFractionShift),
    kLinearPCMFormatFlagsAreAllClear            = kAudioFormatFlagsAreAllClear,
    kAppleLosslessFormatFlag_16BitSourceData    = 1,
    kAppleLosslessFormatFlag_20BitSourceData    = 2,
    kAppleLosslessFormatFlag_24BitSourceData    = 3,
    kAppleLosslessFormatFlag_32BitSourceData    = 4
};

/* The commonly used combinations. `CA_PREFER_FIXED_POINT` is Apple's build switch and is off, which is
 * what makes `kAudioFormatFlagsCanonical` 9 — a float, native endian, packed — and not the 16-bit
 * integer form. BOTH branches are kept, because a client that defines the switch must get the same
 * answer it got on macOS. */
CF_ENUM(AudioFormatFlags)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    kAudioFormatFlagsNativeEndian       = kAudioFormatFlagIsBigEndian,
#else
    kAudioFormatFlagsNativeEndian       = 0,
#endif
#if !defined(CA_PREFER_FIXED_POINT) || !CA_PREFER_FIXED_POINT
    kAudioFormatFlagsCanonical CA_CANONICAL_DEPRECATED          = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
    kAudioFormatFlagsAudioUnitCanonical CA_CANONICAL_DEPRECATED = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
#else
    kAudioFormatFlagsCanonical CA_CANONICAL_DEPRECATED          = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
    kAudioFormatFlagsAudioUnitCanonical CA_CANONICAL_DEPRECATED = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved | (kAudioUnitSampleFractionBits << kLinearPCMFormatFlagsSampleFractionShift),
#endif
    kAudioFormatFlagsNativeFloatPacked  = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked
};

#define TestAudioFormatNativeEndian(f)  ((f.mFormatID == kAudioFormatLinearPCM) && ((f.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian))

#if defined(__cplusplus)
static inline bool IsAudioFormatNativeEndian(const AudioStreamBasicDescription& f) { return (f.mFormatID == kAudioFormatLinearPCM) && ((f.mFormatFlags & kAudioFormatFlagIsBigEndian) == kAudioFormatFlagsNativeEndian); }

static inline AudioFormatFlags    CalculateLPCMFlags(UInt32 inValidBitsPerChannel, UInt32 inTotalBitsPerChannel, bool inIsFloat, bool inIsBigEndian, bool inIsNonInterleaved = false) { return (inIsFloat ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | (inIsBigEndian ? ((UInt32)kAudioFormatFlagIsBigEndian) : 0) | ((inValidBitsPerChannel == inTotalBitsPerChannel) ? kAudioFormatFlagIsPacked : kAudioFormatFlagIsAlignedHigh) | (inIsNonInterleaved ? ((UInt32)kAudioFormatFlagIsNonInterleaved) : 0); }
#endif

struct AudioStreamPacketDescription
{
    SInt64  mStartOffset;
    UInt32  mVariableFramesInPacket;
    UInt32  mDataByteSize;
};
typedef struct AudioStreamPacketDescription AudioStreamPacketDescription;

#pragma mark - Time stamps
typedef CF_ENUM(UInt32, SMPTETimeType)
{
    kSMPTETimeType24        = 0,
    kSMPTETimeType25        = 1,
    kSMPTETimeType30Drop    = 2,
    kSMPTETimeType30        = 3,
    kSMPTETimeType2997      = 4,
    kSMPTETimeType2997Drop  = 5,
    kSMPTETimeType60        = 6,
    kSMPTETimeType5994      = 7,
    kSMPTETimeType60Drop    = 8,
    kSMPTETimeType5994Drop  = 9,
    kSMPTETimeType50        = 10,
    kSMPTETimeType2398      = 11
};

typedef CF_OPTIONS(UInt32, SMPTETimeFlags)
{
    kSMPTETimeUnknown   = 0,
    kSMPTETimeValid     = (1U << 0)
};

struct SMPTETime
{
    SInt16          mSubframes;
    SInt16          mSubframeDivisor;
    UInt32          mCounter;
    SMPTETimeType   mType;
    SMPTETimeFlags  mFlags;
    SInt16          mHours;
    SInt16          mMinutes;
    SInt16          mSeconds;
    SInt16          mFrames;
};
typedef struct SMPTETime    SMPTETime;

typedef CF_OPTIONS(UInt32, AudioTimeStampFlags)
{
    kAudioTimeStampNothingValid         = 0,
    kAudioTimeStampSampleTimeValid      = (1U << 0),
    kAudioTimeStampHostTimeValid        = (1U << 1),
    kAudioTimeStampRateScalarValid      = (1U << 2),
    kAudioTimeStampWordClockTimeValid   = (1U << 3),
    kAudioTimeStampSMPTETimeValid       = (1U << 4),
    kAudioTimeStampSampleHostTimeValid  = (kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid)
};

/* FOUR CLOCKS AND A FLAG WORD SAYING WHICH OF THEM THE CALLER MAY BELIEVE. The flags are the whole
 * contract: a field whose bit is clear is not zero, it is meaningless. */
struct AudioTimeStamp
{
    Float64             mSampleTime;
    UInt64              mHostTime;
    Float64             mRateScalar;
    UInt64              mWordClockTime;
    SMPTETime           mSMPTETime;
    AudioTimeStampFlags mFlags;
    UInt32              mReserved;
};
typedef struct AudioTimeStamp   AudioTimeStamp;

#if defined(__cplusplus)
static inline void    FillOutAudioTimeStampWithSampleTime(AudioTimeStamp& outATS, Float64 inSampleTime)    { outATS.mSampleTime = inSampleTime; outATS.mHostTime = 0; outATS.mRateScalar = 0; outATS.mWordClockTime = 0; memset(&outATS.mSMPTETime, 0, sizeof(SMPTETime)); outATS.mFlags = kAudioTimeStampSampleTimeValid; }
static inline void    FillOutAudioTimeStampWithHostTime(AudioTimeStamp& outATS, UInt64 inHostTime) { outATS.mSampleTime = 0; outATS.mHostTime = inHostTime; outATS.mRateScalar = 0; outATS.mWordClockTime = 0; memset(&outATS.mSMPTETime, 0, sizeof(SMPTETime)); outATS.mFlags = kAudioTimeStampHostTimeValid; }
static inline void    FillOutAudioTimeStampWithSampleAndHostTime(AudioTimeStamp& outATS, Float64 inSampleTime, UInt64 inHostTime) { outATS.mSampleTime = inSampleTime; outATS.mHostTime = inHostTime; outATS.mRateScalar = 0; outATS.mWordClockTime = 0; memset(&outATS.mSMPTETime, 0, sizeof(SMPTETime)); outATS.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid; }
#endif

#pragma mark - Codecs
struct AudioClassDescription {
    OSType  mType;
    OSType  mSubType;
    OSType  mManufacturer;
};
typedef struct AudioClassDescription    AudioClassDescription;

typedef CF_ENUM(long, MPEG4ObjectID)
{
    kMPEG4Object_AAC_Main   = 1,
    kMPEG4Object_AAC_LC     = 2,
    kMPEG4Object_AAC_SSR    = 3,
    kMPEG4Object_AAC_LTP    = 4,
    kMPEG4Object_AAC_SBR    = 5,
    kMPEG4Object_AAC_Scalable = 6,
    kMPEG4Object_TwinVQ     = 7,
    kMPEG4Object_CELP       = 8,
    kMPEG4Object_HVXC       = 9
};

#pragma mark - Channel layouts
typedef CF_ENUM(UInt32, AudioChannelLabel)
{
    kAudioChannelLabel_UseCoordinates   = 100,
    kAudioChannelLabel_Left             = 1,
    kAudioChannelLabel_Right            = 2,
    kAudioChannelLabel_Center           = 3,
    kAudioChannelLabel_LFEScreen        = 4
};

typedef CF_OPTIONS(UInt32, AudioChannelBitmap)
{
    kAudioChannelBit_Left           = (1U << 0),
    kAudioChannelBit_Right          = (1U << 1),
    kAudioChannelBit_Center         = (1U << 2),
    kAudioChannelBit_LFEScreen      = (1U << 3)
};

typedef CF_OPTIONS(UInt32, AudioChannelFlags)
{
    kAudioChannelFlags_AllOff   = 0,
    kAudioChannelFlags_Meters   = (1U << 2)
};

typedef CF_ENUM(UInt32, AudioChannelCoordinateIndex)
{
    kAudioChannelCoordinates_LeftRight  = 0,
    kAudioChannelCoordinates_BackFront  = 1,
    kAudioChannelCoordinates_DownUp     = 2,
    kAudioChannelCoordinates_Azimuth    = 0,
    kAudioChannelCoordinates_Elevation  = 1,
    kAudioChannelCoordinates_Distance   = 2
};

/* A layout tag is either a named layout — `(label << 16) | channelCount`, which is why Stereo is
 * 6619138 — or one of two sentinels saying "read the bitmap" or "read the descriptions". */
typedef CF_ENUM(UInt32, AudioChannelLayoutTag)
{
    kAudioChannelLayoutTag_UseChannelDescriptions   = (0U<<16) | 0,
    kAudioChannelLayoutTag_UseChannelBitmap         = (1U<<16) | 0,
    kAudioChannelLayoutTag_Mono                     = (100U<<16) | 1,
    kAudioChannelLayoutTag_Stereo                   = (101U<<16) | 2,
    kAudioChannelLayoutTag_StereoHeadphones         = (102U<<16) | 2,
    kAudioChannelLayoutTag_MPEG_5_1_A               = (121U<<16) | 6,
    kAudioChannelLayoutTag_MPEG_7_1_A               = (126U<<16) | 8
};

struct AudioChannelDescription
{
    AudioChannelLabel           mChannelLabel;
    AudioChannelFlags           mChannelFlags;
    Float32                     mCoordinates[3];
};
typedef struct AudioChannelDescription AudioChannelDescription;

/* ALSO A VARIABLE LENGTH STRUCTURE: `mNumberChannelDescriptions` decides how many of the one declared
 * description are really there, and a caller with N allocates `offsetof(...) + N * sizeof(...)`. */
struct AudioChannelLayout
{
    AudioChannelLayoutTag       mChannelLayoutTag;
    AudioChannelBitmap          mChannelBitmap;
    UInt32                      mNumberChannelDescriptions;
    AudioChannelDescription     mChannelDescriptions[1];
};
typedef struct AudioChannelLayout AudioChannelLayout;

CF_ASSUME_NONNULL_END

#endif /* AGORA_COREAUDIO_COREAUDIOTYPES_H */
