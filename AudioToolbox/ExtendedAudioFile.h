/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/ExtendedAudioFile.h — a file that speaks the caller's format.
 *
 * THE DIFFERENCE FROM `AudioFile.h` IS WHO CONVERTS. A plain audio file hands over its own bytes, so every
 * client that wants floats has to own a converter and get the packet arithmetic right. This door does it:
 * the file's format and the client's format are two properties, and the conversion between them — sample
 * format, interleaving, and sample rate — happens inside, with the converter's history kept between calls.
 *
 * SPECIFIED BY macOS 10.12: the prototypes, the property selectors and the flags are the reference's.
 * `ExtAudioFileCreateNew` is the Carbon `FSRef` door and is not declared here, for the reason `AudioFile.h`
 * gives.
 */
#ifndef AGORA_AUDIOTOOLBOX_EXTENDEDAUDIOFILE_H
#define AGORA_AUDIOTOOLBOX_EXTENDEDAUDIOFILE_H

#include <AudioToolbox/AudioFile.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef struct OpaqueExtAudioFile *  ExtAudioFileRef;
typedef UInt32                      ExtAudioFilePropertyID;

CF_ENUM(ExtAudioFilePropertyID)
{
    kExtAudioFileProperty_FileDataFormat    = 'ffmt',
    kExtAudioFileProperty_FileLengthFrames  = '#frm',
    kExtAudioFileProperty_ClientDataFormat  = 'cfmt'
};

extern OSStatus
ExtAudioFileOpenURL(CFURLRef inFileRef, ExtAudioFileRef _Nullable * _Nonnull outExtAudioFile);

extern OSStatus
ExtAudioFileCreateWithURL(CFURLRef                            inFileRef,
                          AudioFileTypeID                     inFileType,
                          const AudioStreamBasicDescription * inFormat,
                          const AudioChannelLayout * _Nullable inChannelLayout,
                          UInt32                              inFlags,
                          ExtAudioFileRef _Nullable * _Nonnull outExtAudioFile);

/* Wrap an already-open audio file. `inCloseFile` says whether disposing the wrapper closes it. */
extern OSStatus
ExtAudioFileWrapAudioFileID(AudioFileID inAudioFile, Boolean inForWriting,
                            ExtAudioFileRef _Nullable * _Nonnull outExtAudioFile);

extern OSStatus
ExtAudioFileDispose(ExtAudioFileRef inExtAudioFile);

/* Frames read or written: what the door exists for. */
extern OSStatus
ExtAudioFileRead(ExtAudioFileRef inExtAudioFile, UInt32 * ioNumberFrames, AudioBufferList * ioData);

extern OSStatus
ExtAudioFileWrite(ExtAudioFileRef inExtAudioFile, UInt32 inNumberFrames, const AudioBufferList * ioData);

/* The same, without the promise that it has happened yet. Buffered through the converter and flushed by
 * the next call or by `Dispose`. */
extern OSStatus
ExtAudioFileWriteAsync(ExtAudioFileRef inExtAudioFile, UInt32 inNumberFrames,
                       const AudioBufferList * _Nullable ioData);

extern OSStatus
ExtAudioFileSeek(ExtAudioFileRef inExtAudioFile, SInt64 inFrameOffset);

extern OSStatus
ExtAudioFileTell(ExtAudioFileRef inExtAudioFile, SInt64 * outFrameOffset);

extern OSStatus
ExtAudioFileGetPropertyInfo(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                            UInt32 * _Nullable outSize, Boolean * _Nullable outWritable);

extern OSStatus
ExtAudioFileGetProperty(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                        UInt32 * ioPropertyDataSize, void * outPropertyData);

extern OSStatus
ExtAudioFileSetProperty(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                        UInt32 inPropertyDataSize, const void * inPropertyData);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_EXTENDEDAUDIOFILE_H */
