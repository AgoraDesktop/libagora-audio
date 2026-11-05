/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioFileStream.h — a file being parsed as it arrives.
 *
 * THE DIFFERENCE FROM `AudioFile.h` IS WHO OWNS THE BYTES. A file door seeks and reads; this one is fed, a
 * buffer at a time, in the order the bytes happen to arrive — from a socket, a pipe, or a download that has
 * not finished. It answers the two questions that can be answered early (what format is this, and where
 * does the audio start) through a listener, and then hands over audio as packets through another.
 *
 * SPECIFIED BY macOS 10.12: the two callbacks, the three flags, the property selectors, the error codes and
 * the prototypes are the reference's. What is implemented here is the same three containers `AudioFile.h`
 * reads, in their streaming form, linear PCM only.
 *
 * `AudioFileStreamSeek` is declared and refuses: seeking a stream needs the packet table that a compressed
 * file carries, and there is none here.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOFILESTREAM_H
#define AGORA_AUDIOTOOLBOX_AUDIOFILESTREAM_H

#include <AudioToolbox/AudioFile.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef struct OpaqueAudioFileStreamID *   AudioFileStreamID;
typedef UInt32                             AudioFileStreamPropertyID;
typedef UInt32                             AudioFileStreamPropertyFlags;
typedef UInt32                             AudioFileStreamParseFlags;
typedef UInt32                             AudioFileStreamSeekFlags;

typedef void
(*AudioFileStream_PropertyListenerProc)(void * _Nonnull           inClientData,
                                        AudioFileStreamID _Nonnull inAudioFileStream,
                                        AudioFileStreamPropertyID  inPropertyID,
                                        AudioFileStreamPropertyFlags * _Nonnull ioFlags);

typedef void
(*AudioFileStream_PacketsProc)(void * _Nonnull                          inClientData,
                               UInt32                                  inNumberBytes,
                               UInt32                                  inNumberPackets,
                               const void * _Nonnull                   inInputData,
                               AudioStreamPacketDescription * _Nonnull inPacketDescriptions);

CF_ENUM(AudioFileStreamPropertyID)
{
    kAudioFileStreamProperty_FileFormat             = 'ffmt',
    kAudioFileStreamProperty_DataFormat             = 'dfmt',
    kAudioFileStreamProperty_DataOffset             = 'doff',
    kAudioFileStreamProperty_AudioDataByteCount     = 'bcnt',
    kAudioFileStreamProperty_AudioDataPacketCount   = 'pcnt',
    kAudioFileStreamProperty_PacketSizeUpperBound   = 'pkub',
    kAudioFileStreamProperty_ReadyToProducePackets  = 'redy'
};

CF_ENUM(AudioFileStreamPropertyFlags)
{
    kAudioFileStreamPropertyFlag_PropertyIsCached  = 1,
    kAudioFileStreamPropertyFlag_CacheProperty     = 2
};

CF_ENUM(AudioFileStreamParseFlags)
{
    kAudioFileStreamParseFlag_Discontinuity        = 1
};

CF_ENUM(AudioFileStreamSeekFlags)
{
    kAudioFileStreamSeekFlag_OffsetIsEstimated     = 1
};

CF_ENUM(OSStatus)
{
    kAudioFileStreamError_UnsupportedFileType       = 'typ?',
    kAudioFileStreamError_UnsupportedDataFormat     = 'fmt?',
    kAudioFileStreamError_UnsupportedProperty       = 'pty?',
    kAudioFileStreamError_BadPropertySize           = '!siz',
    kAudioFileStreamError_NotOptimized              = 'optm',
    kAudioFileStreamError_InvalidPacketOffset       = 'pck?',
    kAudioFileStreamError_InvalidFile               = 'dta?',
    kAudioFileStreamError_ValueUnknown              = 'unk?',
    kAudioFileStreamError_DataUnavailable           = 'more',
    kAudioFileStreamError_IllegalOperation          = 'nope',
    kAudioFileStreamError_UnspecifiedError          = 'wht?',
    kAudioFileStreamError_DiscontinuityCantRecover  = 'dsc!'
};

extern OSStatus
AudioFileStreamOpen(void * _Nullable                            inClientData,
                    AudioFileStream_PropertyListenerProc _Nonnull inPropertyListenerProc,
                    AudioFileStream_PacketsProc _Nonnull        inPacketsProc,
                    AudioFileTypeID                            inFileTypeHint,
                    AudioFileStreamID _Nullable * _Nonnull      outAudioFileStream);

extern OSStatus
AudioFileStreamParseBytes(AudioFileStreamID     inAudioFileStream,
                          UInt32                inDataByteSize,
                          const void * _Nonnull inData,
                          AudioFileStreamParseFlags inFlags);

extern OSStatus
AudioFileStreamGetPropertyInfo(AudioFileStreamID   inAudioFileStream,
                               AudioFileStreamPropertyID inPropertyID,
                               UInt32 * _Nullable  outPropertyDataSize,
                               Boolean * _Nullable outWritable);

extern OSStatus
AudioFileStreamGetProperty(AudioFileStreamID   inAudioFileStream,
                           AudioFileStreamPropertyID inPropertyID,
                           UInt32 *            ioPropertyDataSize,
                           void *              outPropertyData);

extern OSStatus
AudioFileStreamSetProperty(AudioFileStreamID   inAudioFileStream,
                           AudioFileStreamPropertyID inPropertyID,
                           UInt32              inPropertyDataSize,
                           const void *        inPropertyData);

extern OSStatus
AudioFileStreamSeek(AudioFileStreamID   inAudioFileStream,
                    SInt64              inPacketOffset,
                    SInt64 *            outDataByteOffset,
                    AudioFileStreamSeekFlags * outFlags);

extern OSStatus
AudioFileStreamClose(AudioFileStreamID inAudioFileStream);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOFILESTREAM_H */
