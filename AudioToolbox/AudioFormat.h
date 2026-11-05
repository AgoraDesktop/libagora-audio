/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioFormat.h — the questions a format can be asked, without a converter.
 *
 * SPECIFIED BY macOS 10.12: the property selectors, the two structures and the two functions are the
 * reference header's, checked by `tests/api/layout.c` and `tests/api/prototypes.c`. What is here is the
 * subset something implements; the rest of Apple's selector table is in the manifest and arrives with the
 * phase that answers it, returning `kAudioFormatUnsupportedPropertyError` until then.
 *
 * WHY THIS EXISTS AT ALL, given a converter: a caller needs to know the byte layout of a format before it
 * has any data — how many bytes a frame is, how many channels a layout tag names — and those are
 * questions about a description, not about a stream.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOFORMAT_H
#define AGORA_AUDIOTOOLBOX_AUDIOFORMAT_H

#include <CoreAudio/CoreAudioTypes.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

/* The errors this door returns, in Apple's four-char spelling so a log line is readable. */
CF_ENUM(OSStatus)
{
    kAudioFormatUnsupportedPropertyError  = 'prop',
    kAudioFormatBadPropertySizeError      = '!siz',
    kAudioFormatBadSpecifierSizeError     = '!spc',
    kAudioFormatUnsupportedDataFormatError = 'fmt?',
    kAudioFormatUnknownFormatError        = '!fmt'
};

typedef UInt32 AudioFormatPropertyID;

/* One format, and the magic cookie that explains it. */
struct AudioFormatInfo
{
    AudioStreamBasicDescription     mASBD;
    const void*                     mMagicCookie;
    UInt32                          mMagicCookieSize;
};
typedef struct AudioFormatInfo  AudioFormatInfo;

/* A format plus the channel layout it is meant to be played through — what a file's format list is made
 * of, and what a device's supported-format list is made of. */
struct AudioFormatListItem
{
    AudioStreamBasicDescription     mASBD;
    AudioChannelLayoutTag           mChannelLayoutTag;
};
typedef struct AudioFormatListItem  AudioFormatListItem;

/* The selectors this component answers. `kAudioFormatProperty_FormatList` is Apple's next door step: it
 * lists the formats something else can produce, which is a question about a file, so it belongs to the
 * phase that has one. */
CF_ENUM(AudioFormatPropertyID)
{
    kAudioFormatProperty_FormatInfo             = 'fmti',
    kAudioFormatProperty_FormatList             = 'flst',
    kAudioFormatProperty_ChannelLayoutForTag    = 'cmpl',
    kAudioFormatProperty_NumberOfChannelsForLayout = 'nchm',
    kAudioFormatProperty_ValidateChannelLayout  = 'vacl'
};

/* The size a property's answer will take, so a caller can allocate it. */
extern OSStatus
AudioFormatGetPropertyInfo(AudioFormatPropertyID    inPropertyID,
                           UInt32                   inSpecifierSize,
                           const void * _Nullable   inSpecifier,
                           UInt32 *                 outPropertyDataSize);

/* The answer itself. The specifier is the question: a format description, a layout tag, a layout. */
extern OSStatus
AudioFormatGetProperty(AudioFormatPropertyID    inPropertyID,
                       UInt32                   inSpecifierSize,
                       const void * _Nullable   inSpecifier,
                       UInt32 * _Nullable       ioPropertyDataSize,
                       void * _Nullable         outPropertyData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOFORMAT_H */
