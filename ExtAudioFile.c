/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * ExtAudioFile.c — a file that speaks the caller's format.
 *
 * THE WHOLE POINT IS THAT THREE THINGS ARE CONVERTED AND THE CALLER THINKS ABOUT NONE OF THEM: sample
 * format, interleaving, and sample rate. The first two are the converter's ordinary work; the third is why
 * this file keeps an input buffer and a converter across calls, because a rate change carries state and a
 * caller asking for 512 frames must not be told "come back with a different number".
 *
 * THE READ LOOP IS THE INTERESTING ONE. `ExtAudioFileRead` is asked for N frames in the client's format;
 * the file holds frames in its own. So: pull converted frames while the converter has them, and when it does
 * not, read another block of the file and push it through. The loop ends at N frames, at end of file, or at
 * a read error — and it reports how many frames it produced, because a short read at the end of a file is
 * normal and not an error.
 *
 * WRITING IS THE SAME PIPELINE BACKWARDS, WITHOUT THE BUFFERING: each call converts its frames and writes
 * what came out. A rate change on the way out means the number of file frames is not the number of client
 * frames, which is exactly why the file's frame count is kept here rather than derived from the write size.
 */

#include <AudioToolbox/ExtendedAudioFile.h>
#include "AudioToolbox_internal.h"

#include <stdlib.h>
#include <string.h>

struct OpaqueExtAudioFile {
	AudioFileID                file;
	bool                       owns_file;
	bool                       for_writing;

	AudioStreamBasicDescription file_format;
	AudioStreamBasicDescription client_format;	/* zeroed until the caller sets one */
	bool                       has_client_format;

	AudioConverterRef          converter;

	/* The read side's staging: frames in the FILE's format, at the file's rate. */
	uint8_t *                  input_buffer;
	UInt32                     input_capacity;	/* bytes */
	UInt32                     input_bytes;		/* valid bytes */
	UInt32                     input_offset;	/* bytes already consumed */
	SInt64                     input_packet;	/* the file frame the buffer starts at */
	bool                       eof;

	SInt64                     client_position;	/* frames, in the client's rate */
	SInt64                     file_position;	/* frames, in the file's rate */
};

static UInt32 fn_bytes_per_packet(const AudioStreamBasicDescription *asbd)
{
	return fn_pcm_bytes_per_frame(asbd);
}

static OSStatus fn_ensure_converter(ExtAudioFileRef ext)
{
	OSStatus status;

	if (!ext->has_client_format) {
		/* No client format set: the file's own format is the client's, and no converter is needed. */
		ext->client_format = ext->file_format;
		ext->has_client_format = true;
	}

	if (ext->converter != NULL) {
		return 0;
	}

	if (memcmp(&ext->client_format, &ext->file_format, sizeof(AudioStreamBasicDescription)) == 0) {
		return 0;	/* identical descriptions: the pipeline is a memcpy */
	}

	status = (ext->for_writing)
	       ? AudioConverterNew(&ext->client_format, &ext->file_format, &ext->converter)
	       : AudioConverterNew(&ext->file_format, &ext->client_format, &ext->converter);

	return status;
}

static void fn_drop_converter(ExtAudioFileRef ext)
{
	if (ext->converter != NULL) {
		AudioConverterDispose(ext->converter);
		ext->converter = NULL;
	}
}

OSStatus ExtAudioFileOpenURL(CFURLRef inFileRef, ExtAudioFileRef *outExtAudioFile)
{
	ExtAudioFileRef ext;
	OSStatus status;
	UInt32 size;

	if (inFileRef == NULL || outExtAudioFile == NULL) {
		return kAudio_ParamError;
	}

	ext = calloc(1, sizeof(*ext));
	if (ext == NULL) {
		return kAudio_MemFullError;
	}

	status = AudioFileOpenURL(inFileRef, kAudioFileReadPermission, 0, &ext->file);
	if (status != 0) {
		free(ext);
		return status;
	}

	ext->owns_file = true;
	ext->for_writing = false;

	size = sizeof(ext->file_format);
	status = AudioFileGetProperty(ext->file, kAudioFilePropertyDataFormat, &size, &ext->file_format);
	if (status != 0) {
		AudioFileClose(ext->file);
		free(ext);
		return status;
	}

	*outExtAudioFile = ext;
	return 0;
}

OSStatus ExtAudioFileCreateWithURL(CFURLRef inFileRef, AudioFileTypeID inFileType,
                                   const AudioStreamBasicDescription *inFormat,
                                   const AudioChannelLayout *inChannelLayout, UInt32 inFlags,
                                   ExtAudioFileRef *outExtAudioFile)
{
	ExtAudioFileRef ext;
	OSStatus status;

	(void)inChannelLayout;

	if (inFileRef == NULL || inFormat == NULL || outExtAudioFile == NULL) {
		return kAudio_ParamError;
	}

	ext = calloc(1, sizeof(*ext));
	if (ext == NULL) {
		return kAudio_MemFullError;
	}

	status = AudioFileCreateWithURL(inFileRef, inFileType, inFormat, (AudioFileFlags)inFlags, &ext->file);
	if (status != 0) {
		free(ext);
		return status;
	}

	ext->owns_file = true;
	ext->for_writing = true;
	ext->file_format = *inFormat;

	/* A CREATED FILE'S CLIENT FORMAT STARTS AS ITS OWN, which is Apple's behaviour and the only one that
	 * makes the first `ExtAudioFileWrite` work without a `SetProperty` first. */
	ext->client_format = *inFormat;
	ext->has_client_format = true;

	*outExtAudioFile = ext;
	return 0;
}

OSStatus ExtAudioFileWrapAudioFileID(AudioFileID inAudioFile, Boolean inForWriting,
                                     ExtAudioFileRef *outExtAudioFile)
{
	ExtAudioFileRef ext;
	OSStatus status;
	UInt32 size;

	if (inAudioFile == NULL || outExtAudioFile == NULL) {
		return kAudio_ParamError;
	}

	ext = calloc(1, sizeof(*ext));
	if (ext == NULL) {
		return kAudio_MemFullError;
	}

	ext->file = inAudioFile;
	ext->owns_file = false;
	ext->for_writing = inForWriting != 0;

	size = sizeof(ext->file_format);
	status = AudioFileGetProperty(ext->file, kAudioFilePropertyDataFormat, &size, &ext->file_format);
	if (status != 0) {
		free(ext);
		return status;
	}

	*outExtAudioFile = ext;
	return 0;
}

OSStatus ExtAudioFileDispose(ExtAudioFileRef inExtAudioFile)
{
	if (inExtAudioFile == NULL) {
		return kAudio_ParamError;
	}

	fn_drop_converter(inExtAudioFile);
	free(inExtAudioFile->input_buffer);

	if (inExtAudioFile->owns_file && inExtAudioFile->file != NULL) {
		AudioFileClose(inExtAudioFile->file);
	}

	free(inExtAudioFile);
	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Properties.
 * ---------------------------------------------------------------------------------------------- */

OSStatus ExtAudioFileGetPropertyInfo(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                                     UInt32 *outSize, Boolean *outWritable)
{
	if (inExtAudioFile == NULL) {
		return kAudio_ParamError;
	}

	if (outWritable != NULL) {
		*outWritable = 0;
	}

	switch (inPropertyID) {
	case kExtAudioFileProperty_FileDataFormat:
		if (outSize) { *outSize = sizeof(AudioStreamBasicDescription); }
		return 0;

	case kExtAudioFileProperty_ClientDataFormat:
		if (outSize) { *outSize = sizeof(AudioStreamBasicDescription); }
		if (outWritable) { *outWritable = 1; }
		return 0;

	case kExtAudioFileProperty_FileLengthFrames:
		if (outSize) { *outSize = sizeof(SInt64); }
		return 0;

	default:
		return kAudioFileUnsupportedPropertyError;
	}
}

OSStatus ExtAudioFileGetProperty(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                                 UInt32 *ioPropertyDataSize, void *outPropertyData)
{
	if (inExtAudioFile == NULL || ioPropertyDataSize == NULL || outPropertyData == NULL) {
		return kAudio_ParamError;
	}

	switch (inPropertyID) {
	case kExtAudioFileProperty_FileDataFormat:
		if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioFileBadPropertySizeError;
		}
		*(AudioStreamBasicDescription *)outPropertyData = inExtAudioFile->file_format;
		*ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kExtAudioFileProperty_ClientDataFormat:
		if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioFileBadPropertySizeError;
		}
		*(AudioStreamBasicDescription *)outPropertyData = inExtAudioFile->client_format;
		*ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kExtAudioFileProperty_FileLengthFrames: {
		SInt64 frames = 0;
		UInt32 size = sizeof(frames);

		if (*ioPropertyDataSize < sizeof(SInt64)) {
			return kAudioFileBadPropertySizeError;
		}

		/* There is no frame-count property on an audio file in 10.12: for linear PCM a packet is a
		 * frame, so the packet count IS the frame count. */
		{
			UInt64 packets = 0;

			size = sizeof(packets);
			if (AudioFileGetProperty(inExtAudioFile->file, kAudioFilePropertyAudioDataPacketCount, &size,
			                         &packets) != 0) {
				packets = 0;
			}
			frames = (SInt64)packets;
		}

		*(SInt64 *)outPropertyData = frames;
		*ioPropertyDataSize = sizeof(SInt64);
		return 0;
	}

	default:
		return kAudioFileUnsupportedPropertyError;
	}
}

OSStatus ExtAudioFileSetProperty(ExtAudioFileRef inExtAudioFile, ExtAudioFilePropertyID inPropertyID,
                                 UInt32 inPropertyDataSize, const void *inPropertyData)
{
	if (inExtAudioFile == NULL || inPropertyData == NULL) {
		return kAudio_ParamError;
	}

	if (inPropertyID != kExtAudioFileProperty_ClientDataFormat) {
		return kAudioFileUnsupportedPropertyError;
	}

	if (inPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
		return kAudioFileBadPropertySizeError;
	}

	/* SETTING THE CLIENT FORMAT REBUILDS THE PIPELINE, and drops whatever the old converter had buffered:
	 * a half-converted block in the previous format has no meaning in the new one. */
	fn_drop_converter(inExtAudioFile);
	inExtAudioFile->client_format = *(const AudioStreamBasicDescription *)inPropertyData;
	inExtAudioFile->has_client_format = true;
	inExtAudioFile->input_bytes = 0;
	inExtAudioFile->input_offset = 0;

	return fn_ensure_converter(inExtAudioFile);
}

/* ------------------------------------------------------------------------------------------------
 * Reading.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_refill(ExtAudioFileRef ext, UInt32 frames)
{
	UInt32 bytes_per_packet = fn_bytes_per_packet(&ext->file_format);
	UInt32 wanted;
	OSStatus status;

	if (bytes_per_packet == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	wanted = frames * bytes_per_packet;

	if (wanted > ext->input_capacity) {
		uint8_t *grown = realloc(ext->input_buffer, wanted);

		if (grown == NULL) {
			return kAudio_MemFullError;
		}
		ext->input_buffer = grown;
		ext->input_capacity = wanted;
	}

	if (ext->input_offset < ext->input_bytes) {
		/* Unconsumed frames are kept: the file's position moves in packets, not in converted frames. */
		memmove(ext->input_buffer, ext->input_buffer + ext->input_offset,
		        ext->input_bytes - ext->input_offset);
		ext->input_bytes -= ext->input_offset;
		ext->input_offset = 0;
	} else {
		ext->input_bytes = 0;
		ext->input_offset = 0;
	}

	{
		UInt32 packets = frames;
		UInt32 capacity = wanted - ext->input_bytes;

		status = AudioFileReadPacketData(ext->file, false, &capacity, NULL,
		                                 ext->input_packet + ext->input_bytes / bytes_per_packet,
		                                 &packets,
		                                 ext->input_buffer + ext->input_bytes);
		if (status != 0) {
			return status;
		}

		ext->input_bytes += capacity;
		ext->eof = (packets == 0);
	}

	return 0;
}

typedef struct {
	ExtAudioFileRef ext;
	const uint8_t * bytes;
	UInt32          frames;
	UInt32          delivered;
	UInt32          bytes_per_packet;
} read_source;

static OSStatus fn_read_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets,
                              AudioBufferList *ioData, AudioStreamPacketDescription **outDescription,
                              void *user)
{
	read_source *source = user;
	UInt32 wanted;
	(void)converter;
	(void)outDescription;

	if (source->delivered >= source->frames) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	wanted = *ioNumberDataPackets;
	if (wanted > source->frames - source->delivered) {
		wanted = source->frames - source->delivered;
	}

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = source->ext->file_format.mChannelsPerFrame;
	ioData->mBuffers[0].mDataByteSize = wanted * source->bytes_per_packet;
	ioData->mBuffers[0].mData = (void *)(source->bytes + (size_t)source->delivered * source->bytes_per_packet);

	source->delivered += wanted;
	*ioNumberDataPackets = wanted;
	return 0;
}

OSStatus ExtAudioFileRead(ExtAudioFileRef inExtAudioFile, UInt32 *ioNumberFrames, AudioBufferList *ioData)
{
	ExtAudioFileRef ext = inExtAudioFile;
	UInt32 wanted;
	UInt32 produced = 0;
	UInt32 file_bytes_per_packet;
	OSStatus status;

	if (ext == NULL || ioNumberFrames == NULL || ioData == NULL) {
		return kAudio_ParamError;
	}

	status = fn_ensure_converter(ext);
	if (status != 0) {
		return status;
	}

	file_bytes_per_packet = fn_bytes_per_packet(&ext->file_format);
	if (file_bytes_per_packet == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	wanted = *ioNumberFrames;

	if (ext->converter == NULL) {
		/* The client wants the file's own format: read straight into its buffers. */
		UInt32 packets = wanted;
		UInt32 bytes = wanted * file_bytes_per_packet;

		status = AudioFileReadPacketData(ext->file, false, &bytes, NULL, ext->file_position, &packets,
		                                 ioData->mBuffers[0].mData);
		if (status != 0) {
			return status;
		}

		ioData->mBuffers[0].mDataByteSize = bytes;
		ext->client_position += packets;
		ext->file_position += packets;
		*ioNumberFrames = packets;
		return 0;
	}

	/* Otherwise: pull through the converter, refilling the file side whenever it runs dry. */
	while (produced < wanted) {
		UInt32 converted;
		AudioBufferList chunk;
		read_source source;
		UInt32 available;

		available = (ext->input_bytes - ext->input_offset) / file_bytes_per_packet;
		if (available == 0) {
			if (ext->eof) {
				break;
			}

			status = fn_refill(ext, 4096);
			if (status != 0) {
				return status;
			}

			if ((ext->input_bytes - ext->input_offset) == 0) {
				/* Nothing came back and the file is not at end: avoid spinning. */
				break;
			}
			continue;
		}

		/* One converted block at a time, into the caller's buffer at the right offset. */
		chunk = *ioData;
		chunk.mBuffers[0].mNumberChannels = ext->client_format.mChannelsPerFrame;
		chunk.mBuffers[0].mData = (uint8_t *)ioData->mBuffers[0].mData
		                        + (size_t)produced * fn_bytes_per_packet(&ext->client_format);
		chunk.mBuffers[0].mDataByteSize = (wanted - produced) * fn_bytes_per_packet(&ext->client_format);

		source.ext = ext;
		source.bytes = ext->input_buffer + ext->input_offset;
		source.frames = available;
		source.delivered = 0;
		source.bytes_per_packet = file_bytes_per_packet;

		converted = wanted - produced;
		status = AudioConverterFillComplexBuffer(ext->converter, fn_read_input, &source, &converted, &chunk,
		                                         NULL);
		if (status != 0) {
			return status;
		}

		ext->input_offset += source.delivered * file_bytes_per_packet;
		ext->file_position += source.delivered;

		if (converted == 0) {
			if (ext->eof && available == source.delivered) {
				break;
			}
			continue;
		}

		produced += converted;
	}

	ext->client_position += produced;
	*ioNumberFrames = produced;
	return 0;
}

typedef struct {
	const AudioBufferList * list;
	AudioStreamBasicDescription * format;
	UInt32 frames;
	UInt32 delivered;
} write_source;

/* The client's frames, handed to the converter once, in the order it asks for them. */
static OSStatus fn_write_input(AudioConverterRef converter, UInt32 *ioNumberDataPackets,
                               AudioBufferList *ioData, AudioStreamPacketDescription **outDescription,
                               void *user)
{
	write_source *source = user;
	UInt32 wanted;
	UInt32 buffer;
	UInt32 bytes_per_sample;
	(void)converter;
	(void)outDescription;

	if (source->delivered >= source->frames) {
		*ioNumberDataPackets = 0;
		return 0;
	}

	wanted = *ioNumberDataPackets;
	if (wanted > source->frames - source->delivered) {
		wanted = source->frames - source->delivered;
	}

	bytes_per_sample = fn_pcm_bytes_per_sample(source->format);

	ioData->mNumberBuffers = source->list->mNumberBuffers;
	for (buffer = 0; buffer < source->list->mNumberBuffers; buffer++) {
		UInt32 stride = (source->format->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
		              ? bytes_per_sample : fn_pcm_bytes_per_frame(source->format);

		ioData->mBuffers[buffer].mNumberChannels = source->list->mBuffers[buffer].mNumberChannels;
		ioData->mBuffers[buffer].mData = (uint8_t *)source->list->mBuffers[buffer].mData
		                               + (size_t)source->delivered * stride;
		ioData->mBuffers[buffer].mDataByteSize = wanted * stride;
	}

	source->delivered += wanted;
	*ioNumberDataPackets = wanted;
	return 0;
}

OSStatus ExtAudioFileWrite(ExtAudioFileRef inExtAudioFile, UInt32 inNumberFrames,
                           const AudioBufferList *ioData)
{
	ExtAudioFileRef ext = inExtAudioFile;
	OSStatus status;

	if (ext == NULL || ioData == NULL) {
		return kAudio_ParamError;
	}

	status = fn_ensure_converter(ext);
	if (status != 0) {
		return status;
	}

	if (ext->converter == NULL) {
		UInt32 bytes = inNumberFrames * fn_bytes_per_packet(&ext->file_format);
		UInt32 packets = inNumberFrames;

		status = AudioFileWritePackets(ext->file, false, bytes, NULL, ext->file_position, &packets,
		                               ioData->mBuffers[0].mData);
		if (status != 0) {
			return status;
		}

		ext->file_position += packets;
		ext->client_position += inNumberFrames;
		return 0;
	}

	/* A rate change on the way out: convert, then write what came out. The client's frame count and the
	 * file's are different numbers, and only the second one is the file's position. */
	{
		UInt32 file_bytes_per_packet = fn_bytes_per_packet(&ext->file_format);
		UInt32 capacity = (UInt32)((double)inNumberFrames * ext->file_format.mSampleRate
		                           / (ext->client_format.mSampleRate > 0.0 ? ext->client_format.mSampleRate : 1.0))
		                + 4096;
		uint8_t *scratch = malloc((size_t)capacity * file_bytes_per_packet);
		UInt32 produced = capacity;
		AudioBufferList list;
		write_source source;

		if (scratch == NULL) {
			return kAudio_MemFullError;
		}

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = ext->file_format.mChannelsPerFrame;
		list.mBuffers[0].mDataByteSize = capacity * file_bytes_per_packet;
		list.mBuffers[0].mData = scratch;

		source.list = ioData;
		source.format = &ext->client_format;
		source.frames = inNumberFrames;
		source.delivered = 0;

		produced = capacity;
		status = AudioConverterFillComplexBuffer(ext->converter, fn_write_input, &source, &produced, &list,
		                                         NULL);
		if (status != 0) {
			free(scratch);
			return status;
		}

		if (produced > 0) {
			UInt32 packets = produced;

			status = AudioFileWritePackets(ext->file, false, produced * file_bytes_per_packet, NULL,
			                               ext->file_position, &packets, scratch);
			if (status != 0) {
				free(scratch);
				return status;
			}

			ext->file_position += packets;
		}

		ext->client_position += inNumberFrames;
		free(scratch);
		return 0;
	}
}

OSStatus ExtAudioFileWriteAsync(ExtAudioFileRef inExtAudioFile, UInt32 inNumberFrames,
                                const AudioBufferList *ioData)
{
	/* There is no asynchronous writer here: the file is a file, and the write has happened by the time this
	 * returns. Apple's contract allows that — the promise is only that the caller need not wait for a disk
	 * flush, which is `fsync`'s business and not this door's. */
	return ExtAudioFileWrite(inExtAudioFile, inNumberFrames, ioData);
}

/* ------------------------------------------------------------------------------------------------
 * Position.
 * ---------------------------------------------------------------------------------------------- */

OSStatus ExtAudioFileSeek(ExtAudioFileRef inExtAudioFile, SInt64 inFrameOffset)
{
	ExtAudioFileRef ext = inExtAudioFile;

	if (ext == NULL) {
		return kAudio_ParamError;
	}

	if (inFrameOffset < 0) {
		return kAudio_ParamError;
	}

	ext->client_position = inFrameOffset;
	ext->input_bytes = 0;
	ext->input_offset = 0;
	ext->eof = false;

	/* THE CONVERTER'S HISTORY IS STALE AFTER A SEEK, which is what `AudioConverterReset` is for: a filter
	 * that still holds the audio from before the jump turns the first block into a click. */
	if (ext->converter != NULL) {
		AudioConverterReset(ext->converter);
	}

	return 0;
}

OSStatus ExtAudioFileTell(ExtAudioFileRef inExtAudioFile, SInt64 *outFrameOffset)
{
	if (inExtAudioFile == NULL || outFrameOffset == NULL) {
		return kAudio_ParamError;
	}

	*outFrameOffset = inExtAudioFile->client_position;
	return 0;
}
