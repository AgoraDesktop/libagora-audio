/*
 * AudioCodec.c — the decoders, and the one this build has.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * **A CODEC IS A STREAM, AND THAT IS WHY IT LIVES BEHIND A HANDLE RATHER THAN BEHIND A LOOKUP.** An MPEG Layer III
 * frame is not self-contained: the bit reservoir lets one frame borrow bits from the last few, so decoding frame N
 * alone gives different samples than decoding frames 0..N in order. mpg123 keeps that state, so this file keeps an
 * mpg123 handle and feeds it bytes in the order the container hands them over. A caller that seeks restarts the
 * decoder (`fn_codec_reset`), which is the same thing a person hears as a click on a bad player.
 *
 * **THE OUTPUT IS INTERLEAVED FLOAT32**, because that is the currency of the conversion path above (`fn_pcm_frame_*`
 * in AudioFormat.c), not because mpg123 prefers it: mpg123 hands back signed 16-bit by default, and the widening here
 * is one multiply per sample.
 *
 * WHAT IS NOT HERE: an encoder, a byte seek inside mpg123 (feed mode has none — the container re-feeds from a frame
 * boundary instead), and every format that is not MPEG Layer III. Layer I and II are decoded by the same library, and
 * 10.12 does declare identifiers for them (`kAudioFormatMPEGLayer1`/`2`), but this door declares only the one it
 * implements, so a Layer II file is refused rather than mislabelled — the container's parser does that, not this file.
 */

#include "AudioToolbox_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AGORA_AUDIO_WITH_MPG123)
#include <mpg123.h>
#endif

struct AudioCodec {
#if defined(AGORA_AUDIO_WITH_MPG123)
	mpg123_handle *handle;
	int16_t       *staging;		/* ONE mpg123_read burst, before it is widened */
	size_t         staging_bytes;
	int            encoding;	/* the encoding mpg123 says it is producing */
	bool           have_format;
	double         rate;
	UInt32         channels;
	bool           ended;
#endif
	int            present;		/* keeps the struct non-empty on a build with no decoder */
};

bool fn_codec_is_decodable(const AudioStreamBasicDescription *asbd)
{
	if (asbd == NULL) {
		return false;
	}
#if defined(AGORA_AUDIO_WITH_MPG123)
	return asbd->mFormatID == kAudioFormatMPEGLayer3;
#else
	return false;
#endif
}

AudioCodec *fn_codec_open(const AudioStreamBasicDescription *asbd)
{
#if defined(AGORA_AUDIO_WITH_MPG123)
	AudioCodec *codec;
	int error = 0;

	if (!fn_codec_is_decodable(asbd)) {
		return NULL;
	}

	codec = calloc(1, sizeof(*codec));
	if (codec == NULL) {
		return NULL;
	}

	mpg123_init();
	codec->handle = mpg123_new(NULL, &error);
	if (codec->handle == NULL || mpg123_open_feed(codec->handle) != MPG123_OK) {
		if (codec->handle != NULL) {
			mpg123_delete(codec->handle);
		}
		free(codec);
		return NULL;
	}
	return codec;
#else
	(void)asbd;
	return NULL;
#endif
}

void fn_codec_close(AudioCodec *codec)
{
#if defined(AGORA_AUDIO_WITH_MPG123)
	if (codec == NULL) {
		return;
	}
	if (codec->handle != NULL) {
		mpg123_close(codec->handle);
		mpg123_delete(codec->handle);
	}
	free(codec->staging);
#endif
	free(codec);
}

/* **A SEEK RESTARTS THE STREAM, NOT THE HANDLE.** The decoder's history is meaningless across a jump, and
 * `mpg123_close` + `mpg123_open_feed` is the smallest way to say so without reallocating anything. */
void fn_codec_reset(AudioCodec *codec)
{
#if defined(AGORA_AUDIO_WITH_MPG123)
	if (codec == NULL || codec->handle == NULL) {
		return;
	}
	mpg123_close(codec->handle);
	if (mpg123_open_feed(codec->handle) != MPG123_OK) {
		codec->ended = true;
		return;
	}
	codec->have_format = false;
	codec->ended = false;
#else
	(void)codec;
#endif
}

/* ------------------------------------------------------------------------------------------------
 * Decoding.
 * ---------------------------------------------------------------------------------------------- */

#if defined(AGORA_AUDIO_WITH_MPG123)
/* One mpg123_read burst, widened into the caller's float buffer. `written` frames are already there. */
static UInt32 fn_widen(AudioCodec *codec, size_t bytes, float *out, UInt32 capacity, UInt32 written)
{
	UInt32 channels = codec->channels;
	UInt32 frames;
	UInt32 i;
	UInt32 samples;

	if (channels == 0) {
		return 0;
	}
	frames = (UInt32)(bytes / ((size_t)channels * (size_t)((codec->encoding == MPG123_ENC_FLOAT_32) ? 4 : 2)));
	if (frames > capacity - written) {
		frames = capacity - written;
	}
	samples = frames * channels;

	if (codec->encoding == MPG123_ENC_FLOAT_32) {
		memcpy(out + (size_t)written * channels, codec->staging, (size_t)samples * sizeof(float));
		return frames;
	}
	if (codec->encoding == MPG123_ENC_SIGNED_16) {
		for (i = 0; i < samples; i++) {
			out[(size_t)written * channels + i] = (float)codec->staging[i] / 32768.0f;
		}
		return frames;
	}

	/* **AN ENCODING THIS FILE DOES NOT WIDEN IS A SILENCE NOBODY COULD EXPLAIN**, so it says so once and stops.
	 * libmpg123's default is signed 16-bit, so reaching here means an unusual build rather than an unusual file. */
	fprintf(stderr, "libagora-audio: mpg123 produced encoding 0x%x, which this build does not widen\n",
	        (unsigned)codec->encoding);
	codec->ended = true;
	return 0;
}
#endif

UInt32 fn_codec_decode(AudioCodec *codec, const void *bytes, UInt32 byte_count,
                       float *out, UInt32 capacity, UInt32 *out_channels)
{
	UInt32 written = 0;

#if defined(AGORA_AUDIO_WITH_MPG123)
	if (codec == NULL || codec->handle == NULL || codec->ended || capacity == 0) {
		if (out_channels != NULL && codec != NULL) {
			*out_channels = codec->channels;
		}
		return 0;
	}

	if (byte_count > 0) {
		if (mpg123_feed(codec->handle, (const unsigned char *)bytes, (size_t)byte_count) != MPG123_OK) {
			codec->ended = true;
			return 0;
		}
	}

	for (;;) {
		size_t capacity_bytes = mpg123_outblock(codec->handle);
		size_t done = 0;
		int status;

		if (capacity_bytes == 0) {
			capacity_bytes = 8192;
		}

		/* **NEVER ASK FOR MORE THAN THERE IS ROOM TO KEEP.** mpg123 hands back a whole burst or nothing, so a read
		 * whose output does not fit would have to drop its tail — MEASURED, that lost 14 336 frames from a
		 * three-second file. Asking for exactly the room that is left costs one more call and loses nothing. */
		if (codec->have_format && codec->channels > 0) {
			size_t sample_bytes = (codec->encoding == MPG123_ENC_FLOAT_32) ? sizeof(float) : sizeof(int16_t);
			size_t room_bytes = (size_t)(capacity - written) * codec->channels * sample_bytes;

			if (room_bytes == 0) {
				break;
			}
			if (room_bytes < capacity_bytes) {
				capacity_bytes = room_bytes;
			}
		}
		if (capacity_bytes > codec->staging_bytes) {
			int16_t *grown = realloc(codec->staging, capacity_bytes);

			if (grown == NULL) {
				codec->ended = true;
				break;
			}
			codec->staging = grown;
			codec->staging_bytes = capacity_bytes;
		}

		status = mpg123_read(codec->handle, (unsigned char *)codec->staging, capacity_bytes, &done);
		if (status == MPG123_NEW_FORMAT) {
			long rate = 0;
			int channels = 0;
			int encoding = 0;

			if (mpg123_getformat(codec->handle, &rate, &channels, &encoding) == MPG123_OK) {
				codec->rate = (double)rate;
				codec->channels = (UInt32)channels;
				codec->encoding = encoding;
				codec->have_format = true;
			}
			continue;
		}

		/* **"I WANT MORE BYTES" AND "THE STREAM ENDED" ARE BOTH MESSAGES THAT CAN CARRY AUDIO.** MEASURED: a
		 * feed-mode read returned MPG123_NEED_MORE (-10) with 290 492 bytes of decoded samples in the same
		 * call, so a reader that widened only on MPG123_OK played 12 288 frames of a three-second file. The
		 * samples are taken first and the message decides only what happens next. */
		if (done > 0) {
			written += fn_widen(codec, done, out, capacity, written);
			if (written >= capacity) {
				break;		/* the rest stays buffered inside mpg123 */
			}
		}

		if (status == MPG123_NEED_MORE) {
			break;			/* the caller feeds more; nothing was lost */
		}
		if (status == MPG123_DONE) {
			codec->ended = true;
			break;
		}
		if (status != MPG123_OK) {
			codec->ended = true;
			break;
		}
		if (done == 0) {
			break;
		}
	}

	if (out_channels != NULL) {
		*out_channels = codec->channels;
	}
#else
	(void)codec;
	(void)bytes;
	(void)byte_count;
	(void)out;
	(void)capacity;
	if (out_channels != NULL) {
		*out_channels = 0;
	}
#endif
	return written;
}
