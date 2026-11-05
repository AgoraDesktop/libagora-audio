/*
 * AudioMpeg.c — one MPEG Layer III frame header, and the two doors that walk it.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * **AN MPEG AUDIO STREAM HAS NO HEADER, SO EVERYTHING BEGINS WITH A SYNC WORD AND A FOUR-BYTE FRAME HEADER.** That is
 * the whole of the format's self-description: version, layer, bitrate, sample rate, padding and channel mode, with the
 * frame's length derived from the first four and its sample count from the version. Two doors need exactly this —
 * `AudioFile.c`, which indexes a whole file, and `AudioFileStream.c`, which reports packets as they arrive — and they
 * need it to agree, because a file the first door indexes and the second door emits are the same frames.
 *
 * So the tables and the parse live here, once, and neither door owns them. What is NOT here is anything with state: a
 * decoder is a stream (see `AudioCodec.c`) and a walk is a loop. This file answers a question about four bytes.
 *
 * **A SYNTHETIC SYNC WORD IS CHEAP AND COMMON.** 0xFF 0xFB appears by chance in ID3 artwork and in the tail of any
 * compressed payload at roughly one in 65 536 positions, which is why the parse rejects every reserved field value
 * rather than the first one it notices, and why both callers verify a frame's length against the bytes they actually
 * have before treating it as a packet.
 */

#include "AudioToolbox_internal.h"

/* kbit/s by index, for MPEG-1 and for MPEG-2/2.5. Index 0 is "free" and 15 is "bad", and both are refused. */
static const uint32_t fn_mpeg_bitrates_mpeg1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const uint32_t fn_mpeg_bitrates_mpeg2[16] = { 0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0 };

/* Hz by version and index. Version 1 is reserved, and index 3 is reserved at every version. */
static const uint32_t fn_mpeg_rates[4][3] = {
	{ 11025, 12000, 8000 },		/* MPEG 2.5 */
	{ 0, 0, 0 },			/* reserved */
	{ 22050, 24000, 16000 },	/* MPEG 2 */
	{ 44100, 48000, 32000 }		/* MPEG 1 */
};

bool fn_mpeg_frame_header(const uint8_t *data, UInt32 available, UInt32 at, fn_mpeg_frame *out)
{
	fn_mpeg_frame scratch;
	uint32_t b1, b2, b3;
	uint32_t version, layer, bitrate_index, rate_index, padding, mode, bitrate, rate, samples;
	uint64_t length;

	if (data == NULL) {
		return false;
	}
	/* **`out` MAY BE NULL, AND THAT IS A REAL USE: "DOES A FRAME START HERE?"** A resynchroniser asks exactly that
	 * and has no use for the fields; answering false because it passed NULL would make it skip every byte. */
	if (out == NULL) {
		out = &scratch;
	}
	if (at > available || available - at < 4) {
		return false;
	}
	if (data[at] != 0xFF || (data[at + 1] & 0xE0) != 0xE0) {
		return false;
	}

	b1 = data[at + 1];
	b2 = data[at + 2];
	b3 = data[at + 3];

	version = (b1 >> 3) & 0x03;		/* 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5, 1 = reserved */
	layer = (b1 >> 1) & 0x03;		/* 1 = Layer III */
	if (version == 1 || layer != 1) {
		return false;
	}

	bitrate_index = (b2 >> 4) & 0x0F;
	rate_index = (b2 >> 2) & 0x03;
	padding = (b2 >> 1) & 0x01;
	if (bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
		return false;
	}

	bitrate = (version == 3) ? fn_mpeg_bitrates_mpeg1[bitrate_index] : fn_mpeg_bitrates_mpeg2[bitrate_index];
	rate = fn_mpeg_rates[version][rate_index];
	if (bitrate == 0 || rate == 0) {
		return false;
	}

	mode = (b3 >> 6) & 0x03;
	samples = (version == 3) ? 1152 : 576;

	/* The frame length every MPEG Layer III table gives: floor(144 x bitrate / rate) + padding, and half of the
	 * coefficient for MPEG-2/2.5 because its frames hold half the samples. */
	length = ((version == 3) ? (144u * (uint64_t)bitrate * 1000u / (uint64_t)rate)
	                         : ( 72u * (uint64_t)bitrate * 1000u / (uint64_t)rate)) + padding;
	if (length < 4 || length > 0xFFFFu) {
		return false;
	}

	out->version = version;
	out->bitrate = bitrate;
	out->rate = rate;
	out->channels = (mode == 3) ? 1 : 2;	/* mode 3 is single-channel; every other mode is two */
	out->samples = samples;
	out->size = (UInt32)length;
	return true;
}

/* **THE TAG'S SIZE IS THE ONLY WAY PAST IT.** An ID3v2 header is ten bytes — `ID3`, a version, a flags byte and a
 * 28-bit *syncsafe* size — and the tag is allowed to contain 0xFF bytes, so a walk that started inside one would index
 * artwork as audio. The size is stored seven bits per byte precisely so that no size byte can look like a sync word. */
int fn_mpeg_id3v2_length(const uint8_t *data, UInt32 available, UInt32 *out_length)
{
	uint32_t size;

	if (data == NULL) {
		return 0;
	}
	if (available < 3) {
		return -1;			/* not enough bytes to know whether there is a tag at all */
	}
	if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') {
		return 0;
	}
	if (available < 10) {
		return -1;			/* the header starts a tag; its size is still to come */
	}

	size = ((uint32_t)(data[6] & 0x7F) << 21) | ((uint32_t)(data[7] & 0x7F) << 14)
	     | ((uint32_t)(data[8] & 0x7F) << 7) | (uint32_t)(data[9] & 0x7F);

	if (out_length != NULL) {
		/* A footer is ten more bytes, and only v2.4 has one; the flag is what says so. */
		*out_length = 10 + size + ((data[5] & 0x10) ? 10u : 0u);
	}
	return 1;
}

/* **THE LARGEST FRAME THIS FORMAT CAN BE, WHICH IS A REAL ANSWER RATHER THAN A SHRUG.** The top bitrate is 320 kbit/s
 * for MPEG-1 and 160 for MPEG-2/2.5, and the lowest rate each version allows is what turns that into bytes; one byte is
 * added for the padding bit. A stream that reports this as its upper bound lets a caller size one buffer. */
UInt32 fn_mpeg_packet_size_upper_bound(const AudioStreamBasicDescription *asbd)
{
	uint32_t rate;
	UInt32 bound;

	if (asbd == NULL || asbd->mSampleRate < 1.0) {
		return 0;
	}
	rate = (uint32_t)asbd->mSampleRate;

	/* 1152 samples per frame is MPEG-1; 576 is MPEG-2 and 2.5, which is how the description distinguishes them. */
	bound = (asbd->mFramesPerPacket >= 1152)
	      ? (UInt32)(144u * 320000u / rate) + 1u
	      : (UInt32)( 72u * 160000u / rate) + 1u;
	return bound;
}
