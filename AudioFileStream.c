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

	/* **AN MPEG STREAM HAS NO HEADER, SO "READY" MEANS "THE FIRST FRAME HEADER PARSED".** Everything after it is
	 * frames, and their sizes differ, so each one is reported with a packet description of its own — the one case
	 * where a caller cannot compute the boundaries for itself. */
	bool                                mpeg;
	AudioStreamPacketDescription *      descriptions;	/* RETAINED: one per frame in the buffer, grown as needed */
	UInt32                              description_capacity;

	/* **THE XING/INFO TAG IS THE STREAM'S ONLY DECLARED LENGTH, AND IT ARRIVES WITH THE FIRST FRAME.** Until that
	 * frame is complete the door cannot answer a packet count; after it, the count is exact without reading the
	 * stream to its end. `has_toc` is what makes a seek to an estimated byte offset possible at all. */
	fn_mpeg_frame                       first_frame;
	bool                                xing_checked;
	SInt64                              packet_count;	/* -1 until the tag says, and for a stream with none */
	bool                                has_toc;
	uint8_t                             toc[100];
	SInt64                              toc_bytes;	/* the audio's length, the tag's own frame included */
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

/* **THE HEADER IS THE FIRST FRAME'S HEADER, AND REACHING IT MEANS SKIPPING WHAT IS NOT AUDIO.** An ID3v2 tag is
 * skipped by its own size (it may contain sync words), and any other bytes before the first real frame are resynced
 * one at a time. The return is the same as the container parsers': 0 done, 1 need more bytes, else an error. */
static int fn_stream_mpeg(AudioFileStreamID stream)
{
	for (;;) {
		UInt32 tag = 0;
		int has_tag;
		UInt32 position;
		fn_mpeg_frame frame;

		has_tag = fn_mpeg_id3v2_length(stream->buffer, stream->length, &tag);
		if (has_tag < 0) {
			return 1;		/* the tag's size is still to come */
		}
		if (has_tag == 1) {
			if (stream->length < tag) {
				return 1;	/* wait for the whole tag rather than walking inside it */
			}
			/* **EVERY BYTE DROPPED MOVES THE STREAM, AND `buffer_start` IS THE ONLY RECORD OF THAT.** Counting
			 * the tag separately as well as moving the buffer would double-count it, and the reported data
			 * offset is the one number a caller uses to find the audio again. */
			memmove(stream->buffer, stream->buffer + tag, stream->length - tag);
			stream->length -= tag;
			stream->buffer_start += (SInt64)tag;
			continue;
		}

		for (position = 0; position + 4 <= stream->length; position++) {
			if (fn_mpeg_frame_header(stream->buffer, stream->length, position, &frame)) {
				break;
			}
		}

		if (position + 4 > stream->length) {
			/* Nothing in the buffer parses. Keep the last three bytes — a header can straddle the boundary — and
			 * wait for more, but only up to a point: an unbounded buffer of garbage is a caller's bug, not a
			 * stream to resynchronise. */
			UInt32 keep = (stream->length < 3) ? stream->length : 3;

			if (stream->length > (1u << 20)) {
				return kAudioFileStreamError_InvalidFile;
			}
			stream->buffer_start += (SInt64)(stream->length - keep);
			memmove(stream->buffer, stream->buffer + stream->length - keep, keep);
			stream->length = keep;
			return 1;
		}

		memset(&stream->asbd, 0, sizeof(stream->asbd));
		stream->asbd.mSampleRate = (Float64)frame.rate;
		stream->asbd.mFormatID = kAudioFormatMPEGLayer3;
		stream->asbd.mChannelsPerFrame = frame.channels;
		stream->asbd.mFramesPerPacket = frame.samples;
		/* Bytes per packet and per frame stay ZERO: a frame's length is in its own header. */

		stream->data_offset = stream->buffer_start + (SInt64)position;
		stream->data_bytes = -1;	/* until the tag says; then it is exact */
		stream->packet_count = -1;
		stream->first_frame = frame;
		stream->xing_checked = false;

		/* The bytes before the first frame are not audio and never will be. */
		if (position > 0) {
			memmove(stream->buffer, stream->buffer + position, stream->length - position);
			stream->length -= position;
		}
		return 0;
	}
}

static void fn_notify(AudioFileStreamID stream, AudioFileStreamPropertyID property);

/* **A COMPRESSED STREAM IS EMITTED BY THE FRAME, BECAUSE THAT IS THE ONLY UNIT IT HAS.** Every complete frame in the
 * buffer becomes one packet, with a description saying where it starts and how many samples it holds; a frame that
 * straddles the end of the buffer waits for the next call, and a byte that starts no frame is skipped one at a time
 * until audio is found again. */
static void fn_emit_mpeg(AudioFileStreamID stream)
{
	UInt32 available = stream->length;
	UInt32 position = 0;
	UInt32 count = 0;

	while (position + 4 <= available) {
		fn_mpeg_frame frame;

		if (!fn_mpeg_frame_header(stream->buffer, available, position, &frame)) {
			break;
		}
		if ((Float64)frame.rate != stream->asbd.mSampleRate
		    || frame.channels != stream->asbd.mChannelsPerFrame
		    || frame.samples != stream->asbd.mFramesPerPacket) {
			break;		/* a frame that disagrees with the description is not part of this stream */
		}
		if (position + frame.size > available) {
			break;		/* incomplete: the rest of it arrives in the next call */
		}

		if (!stream->xing_checked && available >= stream->first_frame.size) {
			fn_mpeg_xing tag;

			stream->xing_checked = true;
			if (fn_mpeg_xing_parse(stream->buffer, available, &stream->first_frame, &tag)) {
				if (tag.has_frames) {
					/* **THE TAG'S COUNT EXCLUDES THE FRAME THAT CARRIES IT**, which this door reports as a packet
					 * like any other — so the packet count is one more than the tag says. Measured against the
					 * file door on both fixtures: 116 + 1 = 117 and 79 + 1 = 80. */
					stream->packet_count = (SInt64)tag.frames + 1;
				}
				if (tag.has_bytes) {
					stream->data_bytes = (SInt64)tag.bytes;
				}
				if (tag.has_toc) {
					memcpy(stream->toc, tag.toc, sizeof(stream->toc));
					stream->has_toc = true;
					stream->toc_bytes = tag.has_bytes ? (SInt64)tag.bytes : -1;
				}

				if (tag.has_bytes) {
					fn_notify(stream, kAudioFileStreamProperty_AudioDataByteCount);
				}
				if (tag.has_frames) {
					fn_notify(stream, kAudioFileStreamProperty_AudioDataPacketCount);
				}
			}
		}

		if (count == stream->description_capacity) {
			UInt32 grown = (stream->description_capacity == 0) ? 64 : stream->description_capacity * 2;
			AudioStreamPacketDescription *grown_descriptions =
			    realloc(stream->descriptions, (size_t)grown * sizeof(*grown_descriptions));

			if (grown_descriptions == NULL) {
				break;	/* emit what is described so far rather than losing it */
			}
			stream->descriptions = grown_descriptions;
			stream->description_capacity = grown;
		}

		stream->descriptions[count].mStartOffset = (SInt64)position;
		stream->descriptions[count].mDataByteSize = frame.size;
		stream->descriptions[count].mVariableFramesInPacket = frame.samples;
		count++;
		position += frame.size;
	}

	if (count == 0) {
		/* No complete frame yet. If the head cannot even start one, it is not audio: resynchronise by one byte. */
		if (available >= 4 && !fn_mpeg_frame_header(stream->buffer, available, 0, NULL)) {
			memmove(stream->buffer, stream->buffer + 1, available - 1);
			stream->length = available - 1;
			stream->buffer_start += 1;
		}
		return;
	}

	stream->packets(stream->client, position, count, stream->buffer, stream->descriptions);

	memmove(stream->buffer, stream->buffer + position, available - position);
	stream->length = available - position;
	stream->buffer_start += position;
}

static void fn_notify(AudioFileStreamID stream, AudioFileStreamPropertyID property)
{
	AudioFileStreamPropertyFlags flags = 0;

	if (stream->listener != NULL) {
		stream->listener(stream->client, stream, property, &flags);
	}
}

static void fn_emit(AudioFileStreamID stream)
{
	UInt32 bytes_per_frame;
	SInt64 audio_start;

	/* **A FRAME IS NOT A FIXED NUMBER OF BYTES, SO A COMPRESSED STREAM HAS ITS OWN EMITTER.** Everything below is
	 * arithmetic on `mBytesPerFrame`, which an MPEG frame does not have. */
	if (stream->mpeg) {
		fn_emit_mpeg(stream);
		return;
	}

	bytes_per_frame = fn_pcm_bytes_per_frame(&stream->asbd);
	audio_start = stream->data_offset - stream->buffer_start;

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
	free(inAudioFileStream->descriptions);
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
			 * bytes, but a caller may have handed over fewer. **"NOT ENOUGH YET" IS NOT AN ERROR**: a caller
			 * feeding a stream one byte at a time must keep being able to, and `DataUnavailable` would stop
			 * every one of them. It is a property's answer, not the parser's. */
			if (stream->length < 4) {
				return 0;
			}

			if (memcmp(stream->buffer, "RIFF", 4) == 0) {
				stream->type = kAudioFileWAVEType;
			} else if (memcmp(stream->buffer, "FORM", 4) == 0) {
				stream->type = kAudioFileAIFFType;
			} else if (memcmp(stream->buffer, "caff", 4) == 0) {
				stream->type = kAudioFileCAFType;
			} else if ((stream->buffer[0] == 'I' && stream->buffer[1] == 'D' && stream->buffer[2] == '3')
			           || (stream->buffer[0] == 0xFF && (stream->buffer[1] & 0xE0) == 0xE0)) {
				/* **A TAG, OR A SYNC WORD: THOSE ARE AN MPEG STREAM'S ONLY TWO OPENINGS**, and neither is a
				 * four-character magic. The full check is `fn_stream_mpeg`'s; this only decides whose. */
				stream->type = kAudioFileMP3Type;
			} else {
				return kAudioFileStreamError_UnsupportedFileType;
			}
		}

		switch (stream->type) {
		case kAudioFileWAVEType:  result = fn_stream_wav(stream); break;
		case kAudioFileAIFFType:  result = fn_stream_aiff(stream); break;
		case kAudioFileCAFType:   result = fn_stream_caf(stream); break;
		case kAudioFileMP3Type:   result = fn_stream_mpeg(stream); break;
		default:                  return kAudioFileStreamError_UnsupportedFileType;
		}

		if (result == 1) {
			return 0;	/* more bytes will finish the header; not an error */
		}
		if (result != 0) {
			return (OSStatus)result;
		}

		/* **MPEG IS THE ONE COMPRESSED FORMAT WITH A DECODER BEHIND IT**, so it is the one that is not refused
		 * here: `ExtAudioFile` decodes it, and a streamed MP3 is the shape a player wants most. */
		if (stream->asbd.mFormatID != kAudioFormatLinearPCM
		    && stream->asbd.mFormatID != kAudioFormatMPEGLayer3) {
			return kAudioFileStreamError_UnsupportedDataFormat;
		}
		stream->mpeg = (stream->asbd.mFormatID == kAudioFormatMPEGLayer3);

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
		SInt64 packets = stream->packet_count;

		if (!stream->mpeg) {
			UInt32 bytes_per_frame = fn_pcm_bytes_per_frame(&stream->asbd);

			packets = (stream->data_bytes >= 0 && bytes_per_frame > 0)
			        ? (stream->data_bytes / bytes_per_frame)
			        : -1;
		}
		if (packets < 0) {
			return kAudioFileStreamError_ValueUnknown;
		}
		if (*ioPropertyDataSize < sizeof(UInt64)) { return kAudioFileStreamError_BadPropertySize; }
		*(UInt64 *)outPropertyData = (UInt64)packets;
		*ioPropertyDataSize = sizeof(UInt64);
		return 0;
	}

	case kAudioFileStreamProperty_PacketSizeUpperBound:
		if (*ioPropertyDataSize < sizeof(UInt32)) { return kAudioFileStreamError_BadPropertySize; }
		/* An MPEG frame's largest possible size is a fact about the rate; a PCM frame's is its own size. */
		*(UInt32 *)outPropertyData = stream->mpeg ? fn_mpeg_packet_size_upper_bound(&stream->asbd)
		                                          : fn_pcm_bytes_per_frame(&stream->asbd);
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
	AudioFileStreamID stream = inAudioFileStream;
	UInt32 index;
	SInt64 offset;

	if (stream == NULL || outDataByteOffset == NULL || outFlags == NULL) {
		return kAudio_ParamError;
	}

	*outFlags = 0;

	/* **PACKET ZERO IS A POSITION THIS DOOR KNOWS EXACTLY**, because it is where the audio starts. */
	if (inPacketOffset == 0) {
		*outDataByteOffset = stream->data_offset;
		return 0;
	}

	/* A seek in a stream needs an index of what has not been read yet, and the tag's 100-entry table is the only
	 * one an MPEG stream carries. Linear PCM has no such table — every packet is the same size — and its byte
	 * offset is arithmetic the caller can do; a file with no tag is a stream nobody has indexed. */
	if (!stream->has_toc || stream->packet_count <= 0 || stream->toc_bytes <= 0) {
		return kAudioFileStreamError_IllegalOperation;
	}
	if (inPacketOffset < 0 || inPacketOffset >= stream->packet_count) {
		return kAudioFileStreamError_InvalidPacketOffset;
	}

	/* **THE TABLE IS A PERCENTAGE OF THE AUDIO, NOT A PACKET INDEX**, so the answer is interpolated: packet N of P
	 * is at N/P of the way, the table's entry for that hundredth is at `toc[i]/256` of the audio's bytes. It is an
	 * estimate — that is what the flag is for — and the caller re-feeds from the offset it names, where this door
	 * resynchronises on the first frame it finds. */
	index = (UInt32)((inPacketOffset * 100) / stream->packet_count);
	if (index > 99) {
		index = 99;
	}
	offset = stream->toc_bytes * (SInt64)stream->toc[index] / 256;

	*outDataByteOffset = stream->data_offset + offset;
	*outFlags = kAudioFileStreamSeekFlag_OffsetIsEstimated;
	return 0;
}
