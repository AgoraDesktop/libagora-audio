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

#include <stdio.h>
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

	/* **A COMPRESSED FILE IS DECODED ON THE WAY IN, AND EVERYTHING ABOVE IT IS UNCHANGED.** MPEG has no fixed
	 * packet size and a decoder that must see frames in order, so the compressed bytes are staged and decoded into
	 * `staged_format` (PCM16 at the file's own rate and channel count) before the ordinary pipeline looks at them.
	 * The converter above therefore only ever sees the linear PCM it already knew how to handle. */
	AudioCodec                *decoder;
	AudioStreamBasicDescription staged_format;
	uint8_t                   *coded;		/* RETAINED: compressed bytes on their way to the decoder */
	UInt32                     coded_capacity;
	UInt32                     coded_bytes;
	SInt64                     coded_packet;	/* the packet index the next read of compressed bytes comes from */
	AudioStreamPacketDescription *coded_desc;	/* RETAINED: one per staged compressed packet */
	UInt32                     coded_desc_capacity;
	float                     *decoded;		/* RETAINED: interleaved float32 out of the decoder */
	UInt32                     decoded_capacity;	/* frames */
	UInt32                     skip_frames;	/* samples to drop from the next decoded frame (a seek inside one) */

	/* The read side's staging: frames in the FILE's format, at the file's rate. */
	uint8_t *                  input_buffer;
	UInt32                     input_capacity;	/* bytes */
	UInt32                     input_bytes;		/* valid bytes */
	UInt32                     input_offset;	/* bytes already consumed */
	bool                       eof;

	SInt64                     client_position;	/* frames, in the client's rate */
	SInt64                     file_position;	/* frames, in the file's rate */
};

static UInt32 fn_bytes_per_packet(const AudioStreamBasicDescription *asbd)
{
	return fn_pcm_bytes_per_frame(asbd);
}

/* **THE FORMAT THE STAGING BUFFER HOLDS**, which is the file's for linear PCM and the decoder's for everything else.
 * Every byte count in this file is computed from this one answer, so a compressed file cannot half-change the
 * pipeline. */
static const AudioStreamBasicDescription *fn_staged_format(ExtAudioFileRef ext)
{
	return (ext->decoder != NULL) ? &ext->staged_format : &ext->file_format;
}

static AudioStreamBasicDescription fn_pcm16(double rate, UInt32 channels)
{
	AudioStreamBasicDescription format;

	memset(&format, 0, sizeof(format));
	format.mSampleRate = rate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	format.mChannelsPerFrame = channels;
	format.mBitsPerChannel = 16;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = channels * sizeof(SInt16);
	format.mBytesPerPacket = format.mBytesPerFrame;
	return format;
}

/* **A DECODER IS BUILT WHEN THE CONTAINER NAMES A FORMAT THIS BUILD CAN DECODE.** The description the decoder
 * produces is PCM16 at the file's rate and channel count — the container states both, so the pipeline can be built
 * before a single frame has been decoded. */
static OSStatus fn_ensure_decoder(ExtAudioFileRef ext)
{
	if (ext->decoder != NULL || ext->file_format.mFormatID == kAudioFormatLinearPCM) {
		return 0;
	}

	/* **A COMPRESSED FILE THIS BUILD CANNOT DECODE IS REFUSED HERE, BY NAME.** `-DAGORA_AUDIO_WITH_MPG123=OFF`
	 * compiles and links, and the container is still walked by `AudioFile` — but a door that opens a file and then
	 * returns silence is worse than one that says why at open time. */
	if (!fn_codec_is_decodable(&ext->file_format)) {
		return kAudioFileUnsupportedDataFormatError;
	}

	ext->decoder = fn_codec_open(&ext->file_format);
	if (ext->decoder == NULL) {
		return kAudioFileUnsupportedDataFormatError;
	}
	ext->staged_format = fn_pcm16(ext->file_format.mSampleRate, ext->file_format.mChannelsPerFrame);
	return 0;
}

static void fn_drop_decoder(ExtAudioFileRef ext)
{
	if (ext->decoder != NULL) {
		fn_codec_close(ext->decoder);
		ext->decoder = NULL;
	}
	ext->coded_bytes = 0;
	ext->coded_packet = 0;
}

static OSStatus fn_ensure_converter(ExtAudioFileRef ext)
{
	const AudioStreamBasicDescription *staged = fn_staged_format(ext);
	OSStatus status;

	if (!ext->has_client_format) {
		/* No client format set: what the pipeline produces is the client's, and no converter is needed. That is
		 * the file's own description for linear PCM, and the decoder's for a compressed file — never the
		 * compressed one, which no ring of the pipeline above this point could consume. */
		ext->client_format = *staged;
		ext->has_client_format = true;
	}

	if (ext->converter != NULL) {
		return 0;
	}

	if (memcmp(&ext->client_format, staged, sizeof(AudioStreamBasicDescription)) == 0) {
		return 0;	/* identical descriptions: the pipeline is a memcpy */
	}

	status = (ext->for_writing)
	       ? AudioConverterNew(&ext->client_format, staged, &ext->converter)
	       : AudioConverterNew(staged, &ext->client_format, &ext->converter);

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

	/* **THE DECODER IS BUILT HERE RATHER THAN AT THE FIRST READ**, so that a file this build cannot play fails at
	 * `Open` — where a caller can still say why — rather than a minute into a song. */
	status = fn_ensure_decoder(ext);
	if (status != 0) {
		fn_drop_decoder(ext);
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
	fn_drop_decoder(inExtAudioFile);
	free(inExtAudioFile->input_buffer);
	free(inExtAudioFile->coded);
	free(inExtAudioFile->coded_desc);
	free(inExtAudioFile->decoded);

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
			/* **A PACKET IS ONE FRAME ONLY FOR LINEAR PCM.** An MPEG frame holds 1152 samples for MPEG-1 and 576
			 * for MPEG-2, so the count has to be scaled by the format's own samples-per-packet or the duration
			 * comes out a thousand times too short. */
			frames = (SInt64)packets;
			if (inExtAudioFile->file_format.mFormatID != kAudioFormatLinearPCM
			    && inExtAudioFile->file_format.mFramesPerPacket > 1) {
				frames *= (SInt64)inExtAudioFile->file_format.mFramesPerPacket;
			}
		}

		/* **A FILE THAT DECLARED ITS GAPLESS TRIM REPORTS THE AUDIO, NOT THE PADDING.** Without this the length of a
		 * 3 s MP3 is 3.06 s and a seek bar ends before the music does; the packet table is where the encoder wrote
		 * the difference, and `mNumberValidFrames` is the same number the decoder produces (measured equal on both
		 * fixtures). A file with no declaration keeps the container's count, which is the honest answer for a file
		 * cut out of a longer one. */
		{
			AudioFilePacketTableInfo table;
			UInt32 table_size = sizeof(table);

			if (AudioFileGetProperty(inExtAudioFile->file, kAudioFilePropertyPacketTableInfo, &table_size,
			                         &table) == 0) {
				frames = table.mNumberValidFrames;
			}
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

/* **ONE DECODE BEHIND THE CONTAINER, AND NOTHING IS THROWN AWAY.** A decode call yields as many frames as mpg123 has
 * buffered, which is not the number the caller asked for: so everything the decoder produces is appended to the
 * staging buffer, and the read above consumes from it in its own sizes. Asking for exactly `frames` and dropping the
 * rest is the bug this shape exists to prevent — MEASURED, it played 12 288 frames of a three-second file. */
static OSStatus fn_refill_decoded(ExtAudioFileRef ext, UInt32 frames)
{
	const AudioStreamBasicDescription *staged = &ext->staged_format;
	UInt32 bytes_per_frame = fn_pcm_bytes_per_frame(staged);
	UInt32 channels = staged->mChannelsPerFrame;
	UInt32 wanted_bytes = frames * bytes_per_frame;
	int guard = 0;

	if (bytes_per_frame == 0 || channels == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	/* Room for one burst of decoded audio. The decoder is asked for this much, and whatever it gives is kept. */
	if (ext->decoded_capacity < 8192) {
		float *buffer = realloc(ext->decoded, (size_t)8192 * channels * sizeof(float));

		if (buffer == NULL) {
			return kAudio_MemFullError;
		}
		ext->decoded = buffer;
		ext->decoded_capacity = 8192;
	}

	while (ext->input_bytes < wanted_bytes && guard++ < 4096) {
		UInt32 stale;
		UInt32 got;
		UInt32 decoded_channels = 0;
		UInt32 f;
		OSStatus status;

		if (ext->coded_bytes == 0 && !ext->eof) {
			UInt32 capacity;
			UInt32 packets = 64;

			if (ext->coded_capacity == 0) {
				uint8_t *coded = malloc(64 * 1024);
				AudioStreamPacketDescription *desc =
				    calloc(packets, sizeof(AudioStreamPacketDescription));

				if (coded == NULL || desc == NULL) {
					free(coded);
					free(desc);
					return kAudio_MemFullError;
				}
				ext->coded = coded;
				ext->coded_capacity = 64 * 1024;
				ext->coded_desc = desc;
				ext->coded_desc_capacity = packets;
			}
			capacity = ext->coded_capacity;

			status = AudioFileReadPacketData(ext->file, false, &capacity, ext->coded_desc,
			                                 ext->coded_packet, &packets, ext->coded);
			if (status != 0) {
				return status;
			}
			ext->coded_bytes = capacity;
			ext->coded_packet += packets;
			ext->eof = (packets == 0);
		}

		stale = ext->coded_bytes;
		got = fn_codec_decode(ext->decoder, ext->coded, ext->coded_bytes, ext->decoded,
		                      ext->decoded_capacity, &decoded_channels);
		/* **WHAT THE DECODER DID NOT USE IS NOT LOST: mpg123 KEEPS IT.** Every byte fed above is either
		 * consumed or buffered inside the handle, so the staging buffer is empty after the call whatever it
		 * returned. */
		ext->coded_bytes = 0;

		if (got == 0) {
			if (ext->eof || stale == 0) {
				break;
			}
			continue;	/* the decoder wants more bytes than it had */
		}

		/* **A STREAM THAT CONTRADICTS ITS OWN DESCRIPTION IS REFUSED, NOT RESHAPED.** The frame walk rejects frames
		 * whose headers disagree with the first, but the decoder is the final authority on what it is producing: a
		 * frame stride that disagrees with the container's channel count turns interleaved audio into noise, so the
		 * two must agree before a sample is copied. */
		if (decoded_channels != 0 && decoded_channels != channels) {
			return kAudioFileUnsupportedDataFormatError;
		}

		{
			size_t needed = (size_t)ext->input_bytes + (size_t)got * bytes_per_frame;

			if (needed > ext->input_capacity) {
				uint8_t *buffer = realloc(ext->input_buffer, needed + 64 * 1024);

				if (buffer == NULL) {
					return kAudio_MemFullError;
				}
				ext->input_buffer = buffer;
				ext->input_capacity = (UInt32)(needed + 64 * 1024);
			}
		}

		/* **A SEEK LANDS BETWEEN FRAMES AND THE CONTAINER CANNOT.** The stream is repositioned to the frame that
		 * contains the sample asked for, and the samples before it are dropped here, which is the only place that
		 * knows which frame was just decoded. */
		{
			UInt32 first = 0;
			UInt32 kept;

			if (ext->skip_frames > 0) {
				first = (ext->skip_frames < got) ? ext->skip_frames : got;
				ext->skip_frames -= first;
			}
			for (f = first; f < got; f++) {
				fn_pcm_frame_from_float(staged, ext->decoded + (size_t)f * channels,
				                        ext->input_buffer + ext->input_bytes + (size_t)(f - first) * bytes_per_frame);
			}
			kept = got - first;
			ext->input_bytes += kept * bytes_per_frame;
		}
	}

	return 0;
}

static OSStatus fn_refill(ExtAudioFileRef ext, UInt32 frames)
{
	UInt32 bytes_per_packet = fn_bytes_per_packet(fn_staged_format(ext));
	UInt32 wanted;
	OSStatus status;

	if (bytes_per_packet == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	/* Keep whatever the last read did not consume: the file's position moves in packets, not in converted frames. */
	if (ext->input_offset < ext->input_bytes) {
		memmove(ext->input_buffer, ext->input_buffer + ext->input_offset,
		        ext->input_bytes - ext->input_offset);
		ext->input_bytes -= ext->input_offset;
		ext->input_offset = 0;
	} else {
		ext->input_bytes = 0;
		ext->input_offset = 0;
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

	/* **A COMPRESSED FILE FILLS THE SAME BUFFER, ONE DECODE BEHIND THE CONTAINER.** The loop ends when enough frames
	 * are staged, the container is exhausted, or the decoder can make no more of what it has; the last case is not an
	 * error, it is what asking for more bytes means. */
	if (ext->decoder != NULL) {
		return fn_refill_decoded(ext, wanted / bytes_per_packet);
	}
	{
		UInt32 packets = frames;
		UInt32 capacity = wanted - ext->input_bytes;
		/* **THE STAGING BUFFER STARTS WHERE THE FILE'S POSITION SAYS IT DOES.** It is the END of the staged audio that
		 * `file_position` tracks, so the beginning is the staged frames counted back from it. Carrying a packet index
		 * of its own meant the converter path restarted at packet 0 on every refill — a read that never advanced. */
		SInt64 start = ext->file_position - (SInt64)(ext->input_bytes / bytes_per_packet);

		if (start < 0) {
			start = 0;
		}
		status = AudioFileReadPacketData(ext->file, false, &capacity, NULL, start, &packets,
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
	ioData->mBuffers[0].mNumberChannels = fn_staged_format(source->ext)->mChannelsPerFrame;
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

	file_bytes_per_packet = fn_bytes_per_packet(fn_staged_format(ext));
	if (file_bytes_per_packet == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	wanted = *ioNumberFrames;

	if (ext->converter == NULL && ext->decoder == NULL) {
		/* **THE FAST PATH IS FOR A FILE WHOSE BYTES ARE ITS FRAMES.** With no converter and no decoder the staged
		 * buffer and the file are the same audio, so a read is a seek and a copy. A DECODED file has no such
		 * shortcut: its bytes are compressed, and they must go through the staging loop below even when the
		 * client's format is exactly what the decoder produces. */
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
			/* **"THE CONTAINER IS EXHAUSTED" IS NOT "THE AUDIO IS".** A decoder still holds what it has
			 * already buffered — MEASURED, 100 KB of a three-second file — so the end of the container is
			 * only the end of the stream when the refill comes back with nothing. */
			if (ext->eof && ext->decoder == NULL) {
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

		/* **DECODED AND ALREADY IN THE CLIENT'S FORMAT: THE STAGED FRAMES ARE THE ANSWER.** No converter exists to
		 * ask, so the copy is the whole conversion. */
		if (ext->converter == NULL) {
			UInt32 take = available;
			UInt32 bytes_per_frame = fn_bytes_per_packet(&ext->client_format);

			if (take > wanted - produced) {
				take = wanted - produced;
			}
			memcpy((uint8_t *)ioData->mBuffers[0].mData + (size_t)produced * bytes_per_frame,
			       ext->input_buffer + ext->input_offset, (size_t)take * bytes_per_frame);
			ext->input_offset += take * file_bytes_per_packet;
			ext->file_position += take;
			produced += take;
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

	/* **A COMPRESSED FILE SEEKS TO A FRAME, BECAUSE A FRAME IS THE ONLY PLACE THE STREAM CAN BE ENTERED.** MPEG's bit
	 * reservoir makes frame N depend on the frames before it, so the decoder is restarted at the frame that contains
	 * the sample asked for (`fn_codec_reset`) and the samples before it inside that frame are dropped on the way out
	 * (`skip_frames`). A linear-PCM file is addressed directly: its bytes are its frames. */
	if (ext->decoder != NULL) {
		SInt64 per_packet = (SInt64)ext->file_format.mFramesPerPacket;
		SInt64 packet = (per_packet > 0) ? inFrameOffset / per_packet : 0;

		ext->coded_packet = packet;
		ext->coded_bytes = 0;
		ext->skip_frames = (per_packet > 0) ? (UInt32)(inFrameOffset - packet * per_packet) : 0;
		ext->file_position = packet;
		fn_codec_reset(ext->decoder);
	} else {
		SInt64 file_rate = (SInt64)ext->file_format.mSampleRate;
		SInt64 client_rate = (SInt64)ext->client_format.mSampleRate;

		/* The two positions are different numbers when the client's rate is not the file's. */
		ext->file_position = (client_rate > 0 && file_rate > 0)
		                    ? inFrameOffset * file_rate / client_rate
		                    : inFrameOffset;
	}

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
