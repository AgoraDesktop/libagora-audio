/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioFile.h — a sound file as a stream of bytes and packets.
 *
 * SPECIFIED BY macOS 10.12: the three permissions, the flag, the callback types, the type IDs, the property
 * selectors, the errors and every prototype below are the reference header's, checked by
 * `tests/api/prototypes.c` and `tests/api/layout.c`.
 *
 * WHAT IT OPENS, THIS PHASE: **WAV (RIFF), AIFF and CAF**, linear PCM only — the three containers a Cocoa
 * application actually hands around, each written and read here rather than delegated to a library. A
 * compressed format is refused with `kAudioFileUnsupportedDataFormatError`, and a container this does not
 * know with `kAudioFileUnsupportedFileTypeError`, both by name.
 *
 * NOT DECLARED, AND WHY: Apple's 10.12 header also carries `AudioFileOpen`, `AudioFileCreate`,
 * `AudioFileInitialize` and `ExtAudioFileCreateNew`, whose first argument is a Carbon `FSRef`. FSRef is a
 * pre-10.0 file reference that Agora does not have and will not grow; the URL and callbacks doors are the
 * modern shapes of the same questions, and a client that needs FSRef needs Carbon, which is a larger
 * absence than this header's.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOFILE_H
#define AGORA_AUDIOTOOLBOX_AUDIOFILE_H

#include <CoreAudio/CoreAudioTypes.h>
#include <CoreFoundation/CFURL.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef struct OpaqueAudioFileID *   AudioFileID;
typedef UInt32                       AudioFileTypeID;
typedef SInt8                        AudioFilePermissions;
typedef UInt32                       AudioFileFlags;
typedef UInt32                       AudioFilePropertyID;

/* What a callbacks-based file is made of: read and write at an absolute offset, and the size. */
typedef OSStatus
(*AudioFile_ReadProc)(void * _Nonnull inClientData, SInt64 inPosition, UInt32 requestCount,
                      void * _Nonnull buffer, UInt32 * _Nonnull actualCount);

typedef OSStatus
(*AudioFile_WriteProc)(void * _Nonnull inClientData, SInt64 inPosition, UInt32 requestCount,
                       const void * _Nonnull buffer, UInt32 * _Nonnull actualCount);

typedef SInt64
(*AudioFile_GetSizeProc)(void * _Nonnull inClientData);

typedef OSStatus
(*AudioFile_SetSizeProc)(void * _Nonnull inClientData, SInt64 inSize);

CF_ENUM(AudioFilePermissions)
{
    kAudioFileReadPermission        = 0x01,
    kAudioFileWritePermission       = 0x02,
    kAudioFileReadWritePermission   = 0x03
};

CF_ENUM(AudioFileFlags)
{
    kAudioFileFlags_EraseFile       = 1,
    kAudioFileFlags_DontPageAlignAudioData = 2
};

CF_ENUM(OSStatus)
{
    kAudioFileUnsupportedFileTypeError  = 'typ?',
    kAudioFileUnsupportedDataFormatError = 'fmt?',
    kAudioFileUnsupportedPropertyError  = 'pty?',
    kAudioFileBadPropertySizeError      = '!siz',
    kAudioFilePermissionsError          = 'prm?',
    kAudioFileNotOpenError              = -38,
    kAudioFileInvalidFileError          = 'dta?',
    kAudioFileInvalidChunkError         = 'chk?'
};

/* The three containers, in Apple's four-char spelling. */
CF_ENUM(AudioFileTypeID)
{
    kAudioFileAIFFType  = 'AIFF',
    kAudioFileWAVEType  = 'WAVE',
    kAudioFileCAFType   = 'caff'
};

/* The property selectors this door answers. Everything else in Apple's table — album artwork, ID3, chunk
 * lists, the packet tables — belongs to the phase that reads or writes the thing it names. */
CF_ENUM(AudioFilePropertyID)
{
    kAudioFilePropertyFileFormat            = 'ffmt',
    kAudioFilePropertyDataFormat            = 'dfmt',
    kAudioFilePropertyDataOffset            = 'doff',
    kAudioFilePropertyAudioDataByteCount    = 'bcnt',
    kAudioFilePropertyAudioDataPacketCount  = 'pcnt',
    kAudioFilePropertyEstimatedDuration     = 'edur',
    kAudioFilePropertyIsOptimized           = 'optm'
};

extern OSStatus
AudioFileOpenURL(CFURLRef                  inFileRef,
                 AudioFilePermissions      inPermissions,
                 AudioFileTypeID           inFileTypeHint,
                 AudioFileID  _Nullable * _Nonnull outAudioFile);

extern OSStatus
AudioFileOpenWithCallbacks(void * _Nonnull                inClientData,
                           AudioFile_ReadProc _Nonnull   inReadFunc,
                           AudioFile_WriteProc _Nullable inWriteFunc,
                           AudioFile_GetSizeProc _Nonnull inGetSizeFunc,
                           AudioFile_SetSizeProc _Nullable inSetSizeFunc,
                           AudioFileTypeID               inFileTypeHint,
                           AudioFileID  _Nullable * _Nonnull outAudioFile);

extern OSStatus
AudioFileCreateWithURL(CFURLRef                            inFileRef,
                       AudioFileTypeID                     inFileType,
                       const AudioStreamBasicDescription * inFormat,
                       AudioFileFlags                      inFlags,
                       AudioFileID  _Nullable * _Nonnull   outAudioFile);

extern OSStatus
AudioFileInitializeWithCallbacks(void * _Nonnull                      inClientData,
                                 AudioFile_ReadProc _Nonnull        inReadFunc,
                                 AudioFile_WriteProc _Nonnull       inWriteFunc,
                                 AudioFile_GetSizeProc _Nonnull     inGetSizeFunc,
                                 AudioFile_SetSizeProc _Nonnull     inSetSizeFunc,
                                 AudioFileTypeID                    inFileType,
                                 const AudioStreamBasicDescription * inFormat,
                                 AudioFileFlags                     inFlags,
                                 AudioFileID  _Nullable * _Nonnull  outAudioFile);

extern OSStatus
AudioFileClose(AudioFileID inAudioFile);

/* Patch the sizes a streaming write left behind. For a file opened for writing this is also what `Close`
 * does, so a caller that wants the file readable while it is still open can ask for it early. */
extern OSStatus
AudioFileOptimize(AudioFileID inAudioFile);

extern OSStatus
AudioFileGetPropertyInfo(AudioFileID        inAudioFile,
                         AudioFilePropertyID inPropertyID,
                         UInt32 * _Nullable outDataSize,
                         UInt32 * _Nullable outWritable);

extern OSStatus
AudioFileGetProperty(AudioFileID        inAudioFile,
                     AudioFilePropertyID inPropertyID,
                     UInt32 *           ioDataSize,
                     void *             outPropertyData);

extern OSStatus
AudioFileSetProperty(AudioFileID        inAudioFile,
                     AudioFilePropertyID inPropertyID,
                     UInt32             inPropertyDataSize,
                     const void *       inPropertyData);

/* The raw doors: bytes at an absolute offset, whatever the container thinks a packet is. */
extern OSStatus
AudioFileReadBytes(AudioFileID inAudioFile,
                   Boolean    inUseCache,
                   SInt64     inStartingByte,
                   UInt32 *   ioNumBytes,
                   void *     outBuffer);

extern OSStatus
AudioFileWriteBytes(AudioFileID     inAudioFile,
                    Boolean        inUseCache,
                    SInt64         inStartingByte,
                    UInt32 *       ioNumBytes,
                    const void *   inBuffer);

/* The packet doors. For linear PCM a packet is one frame, so these are frames. */
extern OSStatus
AudioFileReadPacketData(AudioFileID                     inAudioFile,
                        Boolean                        inUseCache,
                        UInt32 *                       ioNumBytes,
                        AudioStreamPacketDescription * _Nullable outPacketDescriptions,
                        SInt64                         inStartingPacket,
                        UInt32 *                       ioNumPackets,
                        void * _Nullable               outBuffer);

extern OSStatus
AudioFileReadPackets(AudioFileID                     inAudioFile,
                     Boolean                        inUseCache,
                     UInt32 *                       outNumBytes,
                     AudioStreamPacketDescription * _Nullable outPacketDescriptions,
                     SInt64                         inStartingPacket,
                     UInt32 *                       ioNumPackets,
                     void * _Nullable               outBuffer);

extern OSStatus
AudioFileWritePackets(AudioFileID                       inAudioFile,
                      Boolean                          inUseCache,
                      UInt32                           inNumBytes,
                      const AudioStreamPacketDescription * _Nullable inPacketDescriptions,
                      SInt64                           inStartingPacket,
                      UInt32 *                         ioNumPackets,
                      const void *                     inBuffer);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUDIOFILE_H */
