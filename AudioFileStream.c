/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioFileStream.c — the three containers, parsed as they arrive.
 *
 * THE STATE IS A BUFFER AND TWO ANSWERS. Everything the stream knows is either "not yet" or a fact it has
 * already told the listener about; the buffer holds the bytes that have arrived and not yet been delivered
 * as audio, and `buffer_start` is where those bytes sit in the stream as a whole, because a chunk header
 * names an absolute offset and the buffer is a window.
 *
 * PARTIAL HEADERS ARE THE NORMAL CASE, NOT AN EDGE CASE: a caller may hand over seven bytes and then the
 * rest, so every parser below returns "need more" rather than failing, and re-runs from the top when more
 * arrives. That is why the parsers are a loop over chunk headers rather than a `memcpy` of a struct: the
 * second call sees the same first chunk and must not re-notify it.
 *
 * THE LISTENER IS TOLD ONCE PER FACT, in the order a client needs them: the data format first (so it can
 * size its buffers), then where the audio starts, then that packets may be produced. The "ready" flag is
 * last because it is the promise that the packets callback will be called at all.
 */

#include <AudioToolbox/AudioFileStream.h>
#include "AudioToolbox_internal.h"

#include <stdlib.h>
#include <string.h>

struct OpaqueAudioFileStreamID {
	void *                              client;
	AudioFileStream_PropertyListenerProc listener;
	AudioFileStream_PacketsProc         packets;

	AudioFileTypeID                     type;	/* 0 until sniffed */
	uint8_t *                           buffer;
	UInt32                              length;
	UInt32                              capacity;
	SInt64                              buffer_start;

	bool                                header_done;
	AudioStreamBasicDescription         asbd;
	SInt64                              data_offset;
	SInt64                              data_bytes;	/* -1 when the container did not say */
	bool                                notified_format;
	bool                                notified_ready;
};

/* ------------------------------------------------------------------------------------------------
 * The parsers. 0 means "the header is complete", 1 means "need more bytes", anything else is an error.
 * ---------------------------------------------------------------------------------------------- */

static int fn_stream_wav(AudioFileStreamID stream)
{
	const uint8_t *buffer = stream->buffer;
	UInt32 length = stream->length;
	UInt32 position = 12;
	bool have_format = false;

	if (length < 12) {
		return 1;
	}
	if (memcmp(buffer, "RIFF", 4) != 0 || memcmp(buffer + 8, "WAVE", 4) != 0) {
		return kAudioFileStreamError_InvalidFile;
	}

	while (position + 8 <= length) {
		uint32_t size = fn_rd32le(buffer + position + 4);

		if (memcmp(buffer + position, "fmt ", 4) == 0) {
			uint16_t tag;
			UInt32 needed = size < 40 ? size : 40;

			if (size < 16) {
				return kAudioFileStreamError_InvalidFile;
			}
			if (position + 8 + needed > length) {
				return 1;
			}

			tag = fn_rd16le(buffer + position + 8);
			memset(&stream->asbd, 0, sizeof(stream->asbd));
			stream->asbd.mSampleRate = (Float64)fn_rd32le(buffer + position + 12);
			stream->asbd.mFormatID = kAudioFormatLinearPCM;
			stream->asbd.mChannelsPerFrame = fn_rd16le(buffer + position + 10);
			stream->asbd.mBitsPerChannel = fn_rd16le(buffer + position + 22);
			stream->asbd.mBytesPerFrame = fn_rd16le(buffer + position + 20);
			stream->asbd.mBytesPerPacket = stream->asbd.mBytesPerFrame;
			stream->asbd.mFramesPerPacket = 1;
			stream->asbd.mFormatFlags = kAudioFormatFlagIsPacked;

			if (tag == 0xFFFE) {
				if (needed < 40 || position + 48 > length) {
					return 1;
				}
				tag = fn_rd16le(buffer + position + 32);
				stream->asbd.mBitsPerChannel = fn_rd16le(buffer + position + 26);
			}

			if (tag == 3) {
				stream->asbd.mFormatFlags |= kAudioFormatFlagIsFloat;
			} else if (tag == 1) {
				if (stream->asbd.mBitsPerChannel != 8) {
					stream->asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
				}
			} else {
				return kAudioFileStreamError_UnsupportedDataFormat;
			}

			have_format = true;
		} else if (memcmp(buffer + position, "data", 4) == 0) {
			if (!have_format) {
				return kAudioFileStreamError_InvalidFile;
			}

			stream->data_offset = (SInt64)position + 8;
			/* A streamed WAV writes 0xFFFFFFFF for a size it does not know yet, and so does a writer that
			 * crashed: both mean "the rest of the file". */
			stream->data_bytes = (size == 0xFFFFFFFFU) ? -1 : (SInt64)size;
			return 0;
		}

		position += 8 + size + ((size & 1U) ? 1 : 0);
	}

	return 1;
}

static int fn_stream_aiff(AudioFileStreamID stream)
{
	const uint8_t *buffer = stream->buffer;
	UInt32 length = stream->length;
	UInt32 position = 12;
	bool have_format = false;

	if (length < 12) {
		return 1;
	}
	if (memcmp(buffer, "FORM", 4) != 0) {
		return kAudioFileStreamError_InvalidFile;
	}
	if (memcmp(buffer + 8, "AIFF", 4) != 0) {
		return kAudioFileStreamError_UnsupportedFileType;
	}

	while (position + 8 <= length) {
		uint32_t size = fn_rd32be(buffer + position + 4);

		if (memcmp(buffer + position, "COMM", 4) == 0) {
			if (size < 18) {
				return kAudioFileStreamError_InvalidFile;
			}
			if (position + 8 + 18 > length) {
				return 1;
			}

			memset(&stream->asbd, 0, sizeof(stream->asbd));
			stream->asbd.mSampleRate = fn_rd_extended(buffer + position + 16);
			stream->asbd.mFormatID = kAudioFormatLinearPCM;
			stream->asbd.mChannelsPerFrame = fn_rd16be(buffer + position + 8);
			stream->asbd.mBitsPerChannel = fn_rd16be(buffer + position + 14);
			stream->asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
			                          | kAudioFormatFlagIsBigEndian;
			stream->asbd.mBytesPerFrame = stream->asbd.mChannelsPerFrame
			                            * ((stream->asbd.mBitsPerChannel + 7) / 8);
			stream->asbd.mBytesPerPacket = stream->asbd.mBytesPerFrame;
			stream->asbd.mFramesPerPacket = 1;
			have_format = true;
		} else if (memcmp(buffer + position, "SSND", 4) == 0) {
			uint32_t offset;

			if (!have_format) {
				return kAudioFileStreamError_InvalidFile;
			}
			if (position + 8 + 8 > length) {
				return 1;
			}

			offset = fn_rd32be(buffer + position + 8);
			stream->data_offset = (SInt64)position + 16 + (SInt64)offset;
			stream->data_bytes = (SInt64)size - 8 - (SInt64)offset;
			return 0;
		}

		position += 8 + size + ((size & 1U) ? 1 : 0);
	}

	return 1;
}

static int fn_stream_caf(AudioFileStreamID stream)
{
	const uint8_t *buffer = stream->buffer;
	UInt32 length = stream->length;
	UInt32 position = 8;
	bool have_desc = false;

	if (length < 8) {
		return 1;
	}
	if (memcmp(buffer, "caff", 4) != 0) {
		return kAudioFileStreamError_InvalidFile;
	}

	while (position + 12 <= length) {
		uint64_t size = fn_rd64be(buffer + position + 4);

		if (memcmp(buffer + position, "desc", 4) == 0) {
			if (size < 32) {
				return kAudioFileStreamError_InvalidFile;
			}
			if (position + 12 + 32 > length) {
				return 1;
			}

			memset(&stream->asbd, 0, sizeof(stream->asbd));
			{
				union { uint64_t bits; double value; } rate;

				rate.bits = fn_rd64be(buffer + position + 12);
				stream->asbd.mSampleRate = rate.value;
			}
			stream->asbd.mFormatID = fn_rd32be(buffer + position + 20);
			stream->asbd.mFormatFlags = fn_rd32be(buffer + position + 24);
			stream->asbd.mBytesPerPacket = fn_rd32be(buffer + position + 28);
			stream->asbd.mFramesPerPacket = fn_rd32be(buffer + position + 32);
			stream->asbd.mChannelsPerFrame = fn_rd32be(buffer + position + 36);
			stream->asbd.mBitsPerChannel = fn_rd32be(buffer + position + 40);
			stream->asbd.mBytesPerFrame = (stream->asbd.mFramesPerPacket != 0)
			                            ? stream->asbd.mBytesPerPacket / stream->asbd.mFramesPerPacket : 0;
			have_desc = true;
		} else if (memcmp(buffer + position, "data", 4) == 0) {
			if (!have_desc) {
				return kAudioFileStreamError_InvalidFile;
			}
			if (position + 12 + 4 > length) {
				return 1;
			}

			stream->data_offset = (SInt64)position + 16;
			stream->data_bytes = (size == 0xFFFFFFFFFFFFFFFFULL) ? -1 : (SInt64)size - 4;
			return 0;
		}

		position += 12 + (SInt64)(size & 0x7FFFFFFFFFFFFFFFULL);
	}

	return 1;
}

/* ------------------------------------------------------------------------------------------------
 * Notifying, and handing over audio.
 * ---------------------------------------------------------------------------------------------- */

static void fn_notify(AudioFileStreamID stream, AudioFileStreamPropertyID property)
{
	AudioFileStreamPropertyFlags flags = 0;

	if (stream->listener != NULL) {
		stream->listener(stream->client, stream, property, &flags);
	}
}

static void fn_emit(AudioFileStreamID stream)
{
	UInt32 bytes_per_frame = fn_pcm_bytes_per_frame(&stream->asbd);
	SInt64 audio_start = stream->data_offset - stream->buffer_start;

	if (bytes_per_frame == 0) {
		return;
	}

	/* NEGATIVE MEANS THE HEADER IS ALREADY BEHIND US: once audio has been delivered and the buffer trimmed,
	 * the buffer starts inside the audio, so the whole of it is available and not "before the data chunk". */
	if (audio_start < 0) {
		audio_start = 0;
	}

	if ((SInt64)stream->length <= audio_start) {
		return;
	}

	{
		UInt32 available = stream->length - (UInt32)audio_start;
		UInt32 packets = available / bytes_per_frame;

		if (packets == 0) {
			return;
		}

		{
			UInt32 consumed = packets * bytes_per_frame;

			if (stream->packets != NULL) {
				/* A CONSTANT-BIT-RATE STREAM HAS NO PACKET DESCRIPTIONS, and Apple's own annotation says
				 * `_Nonnull` anyway: the value is NULL because there is nothing to describe, which is what
				 * every macOS caller relies on. Held in a variable so the annotation does not warn about a
				 * literal. */
				AudioStreamPacketDescription *descriptions = NULL;

				stream->packets(stream->client, consumed, packets,
				                stream->buffer + (UInt32)audio_start, descriptions);
			}

			memmove(stream->buffer, stream->buffer + (UInt32)audio_start + consumed,
			        stream->length - (UInt32)audio_start - consumed);
			stream->length -= (UInt32)audio_start + consumed;
			stream->buffer_start += (SInt64)audio_start + consumed;
		}
	}
}

/* ------------------------------------------------------------------------------------------------
 * The doors.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioFileStreamOpen(void *inClientData, AudioFileStream_PropertyListenerProc inPropertyListenerProc,
                             AudioFileStream_PacketsProc inPacketsProc, AudioFileTypeID inFileTypeHint,
                             AudioFileStreamID *outAudioFileStream)
{
	AudioFileStreamID stream;

	if (inPropertyListenerProc == NULL || inPacketsProc == NULL || outAudioFileStream == NULL) {
		return kAudio_ParamError;
	}

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		return kAudio_MemFullError;
	}

	stream->client = inClientData;
	stream->listener = inPropertyListenerProc;
	stream->packets = inPacketsProc;
	stream->type = inFileTypeHint;
	stream->data_bytes = -1;

	stream->capacity = 4096;
	stream->buffer = malloc(stream->capacity);
	if (stream->buffer == NULL) {
		free(stream);
		return kAudio_MemFullError;
	}

	*outAudioFileStream = stream;
	return 0;
}

OSStatus AudioFileStreamClose(AudioFileStreamID inAudioFileStream)
{
	if (inAudioFileStream == NULL) {
		return kAudio_ParamError;
	}

	free(inAudioFileStream->buffer);
	free(inAudioFileStream);
	return 0;
}

OSStatus AudioFileStreamParseBytes(AudioFileStreamID inAudioFileStream, UInt32 inDataByteSize,
                                   const void *inData, AudioFileStreamParseFlags inFlags)
{
	AudioFileStreamID stream = inAudioFileStream;
	int result = 0;

	(void)inFlags;

	if (stream == NULL || inData == NULL) {
		return kAudio_ParamError;
	}

	if (inDataByteSize > 0) {
		if (stream->length + inDataByteSize > stream->capacity) {
			UInt32 capacity = stream->capacity;
			uint8_t *grown;

			while (capacity < stream->length + inDataByteSize) {
				capacity *= 2;
			}

			grown = realloc(stream->buffer, capacity);
			if (grown == NULL) {
				return kAudio_MemFullError;
			}
			stream->buffer = grown;
			stream->capacity = capacity;
		}

		memcpy(stream->buffer + stream->length, inData, inDataByteSize);
		stream->length += inDataByteSize;
	}

	if (!stream->header_done) {
		if (stream->type == 0) {
			/* The magic is the only honest source for "which container is this", and it fits in eight
			 * bytes, but a caller may have handed over fewer. */
			if (stream->length < 4) {
				return kAudioFileStreamError_DataUnavailable;
			}

			if (memcmp(stream->buffer, "RIFF", 4) == 0) {
				stream->type = kAudioFileWAVEType;
			} else if (memcmp(stream->buffer, "FORM", 4) == 0) {
				stream->type = kAudioFileAIFFType;
			} else if (memcmp(stream->buffer, "caff", 4) == 0) {
				stream->type = kAudioFileCAFType;
			} else {
				return kAudioFileStreamError_UnsupportedFileType;
			}
		}

		switch (stream->type) {
		case kAudioFileWAVEType:  result = fn_stream_wav(stream); break;
		case kAudioFileAIFFType:  result = fn_stream_aiff(stream); break;
		case kAudioFileCAFType:   result = fn_stream_caf(stream); break;
		default:                  return kAudioFileStreamError_UnsupportedFileType;
		}

		if (result == 1) {
			return 0;	/* more bytes will finish the header; not an error */
		}
		if (result != 0) {
			return (OSStatus)result;
		}

		if (stream->asbd.mFormatID != kAudioFormatLinearPCM) {
			return kAudioFileStreamError_UnsupportedDataFormat;
		}

		stream->header_done = true;

		if (!stream->notified_format) {
			stream->notified_format = true;
			fn_notify(stream, kAudioFileStreamProperty_DataFormat);
			fn_notify(stream, kAudioFileStreamProperty_FileFormat);
			fn_notify(stream, kAudioFileStreamProperty_DataOffset);
			if (stream->data_bytes >= 0) {
				fn_notify(stream, kAudioFileStreamProperty_AudioDataByteCount);
			}
			fn_notify(stream, kAudioFileStreamProperty_PacketSizeUpperBound);
		}

		if (!stream->notified_ready) {
			stream->notified_ready = true;
			fn_notify(stream, kAudioFileStreamProperty_ReadyToProducePackets);
		}
	}

	fn_emit(stream);
	return 0;
}

OSStatus AudioFileStreamGetPropertyInfo(AudioFileStreamID inAudioFileStream,
                                        AudioFileStreamPropertyID inPropertyID, UInt32 *outPropertyDataSize,
                                        Boolean *outWritable)
{
	(void)inAudioFileStream;

	if (outWritable != NULL) {
		*outWritable = 0;
	}

	switch (inPropertyID) {
	case kAudioFileStreamProperty_FileFormat:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(AudioFileTypeID); }
		return 0;
	case kAudioFileStreamProperty_DataFormat:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(AudioStreamBasicDescription); }
		return 0;
	case kAudioFileStreamProperty_DataOffset:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(SInt64); }
		return 0;
	case kAudioFileStreamProperty_AudioDataByteCount:
	case kAudioFileStreamProperty_AudioDataPacketCount:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(UInt64); }
		return 0;
	case kAudioFileStreamProperty_PacketSizeUpperBound:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(UInt32); }
		return 0;
	case kAudioFileStreamProperty_ReadyToProducePackets:
		if (outPropertyDataSize) { *outPropertyDataSize = sizeof(UInt32); }
		return 0;
	default:
		return kAudioFileStreamError_UnsupportedProperty;
	}
}

OSStatus AudioFileStreamGetProperty(AudioFileStreamID inAudioFileStream,
                                    AudioFileStreamPropertyID inPropertyID, UInt32 *ioPropertyDataSize,
                                    void *outPropertyData)
{
	AudioFileStreamID stream = inAudioFileStream;

	if (stream == NULL || ioPropertyDataSize == NULL || outPropertyData == NULL) {
		return kAudio_ParamError;
	}

	if (!stream->header_done) {
		return kAudioFileStreamError_DataUnavailable;
	}

	switch (inPropertyID) {
	case kAudioFileStreamProperty_FileFormat:
		if (*ioPropertyDataSize < sizeof(AudioFileTypeID)) { return kAudioFileStreamError_BadPropertySize; }
		*(AudioFileTypeID *)outPropertyData = stream->type;
		*ioPropertyDataSize = sizeof(AudioFileTypeID);
		return 0;

	case kAudioFileStreamProperty_DataFormat:
		if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioFileStreamError_BadPropertySize;
		}
		*(AudioStreamBasicDescription *)outPropertyData = stream->asbd;
		*ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioFileStreamProperty_DataOffset:
		if (*ioPropertyDataSize < sizeof(SInt64)) { return kAudioFileStreamError_BadPropertySize; }
		*(SInt64 *)outPropertyData = stream->data_offset;
		*ioPropertyDataSize = sizeof(SInt64);
		return 0;

	case kAudioFileStreamProperty_AudioDataByteCount:
		if (stream->data_bytes < 0) {
			return kAudioFileStreamError_ValueUnknown;
		}
		if (*ioPropertyDataSize < sizeof(UInt64)) { return kAudioFileStreamError_BadPropertySize; }
		*(UInt64 *)outPropertyData = (UInt64)stream->data_bytes;
		*ioPropertyDataSize = sizeof(UInt64);
		return 0;

	case kAudioFileStreamProperty_AudioDataPacketCount: {
		UInt32 bytes_per_frame = fn_pcm_bytes_per_frame(&stream->asbd);

		if (stream->data_bytes < 0 || bytes_per_frame == 0) {
			return kAudioFileStreamError_ValueUnknown;
		}
		if (*ioPropertyDataSize < sizeof(UInt64)) { return kAudioFileStreamError_BadPropertySize; }
		*(UInt64 *)outPropertyData = (UInt64)(stream->data_bytes / bytes_per_frame);
		*ioPropertyDataSize = sizeof(UInt64);
		return 0;
	}

	case kAudioFileStreamProperty_PacketSizeUpperBound:
		if (*ioPropertyDataSize < sizeof(UInt32)) { return kAudioFileStreamError_BadPropertySize; }
		*(UInt32 *)outPropertyData = fn_pcm_bytes_per_frame(&stream->asbd);
		*ioPropertyDataSize = sizeof(UInt32);
		return 0;

	case kAudioFileStreamProperty_ReadyToProducePackets:
		if (*ioPropertyDataSize < sizeof(UInt32)) { return kAudioFileStreamError_BadPropertySize; }
		*(UInt32 *)outPropertyData = stream->header_done ? 1U : 0U;
		*ioPropertyDataSize = sizeof(UInt32);
		return 0;

	default:
		return kAudioFileStreamError_UnsupportedProperty;
	}
}

OSStatus AudioFileStreamSetProperty(AudioFileStreamID inAudioFileStream,
                                    AudioFileStreamPropertyID inPropertyID, UInt32 inPropertyDataSize,
                                    const void *inPropertyData)
{
	(void)inAudioFileStream;
	(void)inPropertyID;
	(void)inPropertyDataSize;
	(void)inPropertyData;

	return kAudioFileStreamError_UnsupportedProperty;
}

OSStatus AudioFileStreamSeek(AudioFileStreamID inAudioFileStream, SInt64 inPacketOffset,
                             SInt64 *outDataByteOffset, AudioFileStreamSeekFlags *outFlags)
{
	(void)inPacketOffset;
	(void)outDataByteOffset;
	(void)outFlags;

	if (inAudioFileStream == NULL) {
		return kAudio_ParamError;
	}

	/* A seek in a stream needs the packet table, which is what makes the byte offset of packet N knowable
	 * without reading everything before it. Linear PCM has no such table — every packet is the same size —
	 * and the byte offset is arithmetic the caller can do. */
	return kAudioFileStreamError_IllegalOperation;
}
