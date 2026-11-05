/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioFile.c — WAV, AIFF and CAF, read and written here.
 *
 * WHY THE CONTAINERS ARE OURS AND NOT A LIBRARY'S: none of the three is hard — a chunk list with a size
 * and a handful of fields — and the alternative is a dependency whose format support we would then have to
 * describe to a caller in Core Audio's vocabulary anyway. The project rules also forbid vendoring code, and
 * libsndfile is not a system library on this machine.
 *
 * WHAT "A PACKET" IS: for linear PCM, one frame. Every packet door below is therefore a frame door, with
 * packet descriptions left alone because constant-bit-rate audio has nothing per packet to describe. The
 * compressed containers that make packets interesting are the ones the codec phase brings.
 *
 * THE TWO THINGS THAT GO WRONG IN A CONTAINER WRITER are both handled explicitly: the sizes in the header
 * are unknown while the data is being written, so they are patched in `finalize` (and `AudioFileOptimize` is
 * the public name for that), and a file that is closed without finalizing leaves them at their placeholder —
 * which is why `Close` finalizes whenever the file was opened for writing.
 */

#include <AudioToolbox/AudioFile.h>
#include "AudioToolbox_internal.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct OpaqueAudioFileID {
	AudioFileTypeID             type;
	AudioFilePermissions        permissions;
	AudioStreamBasicDescription asbd;

	SInt64 data_offset;   /* where the audio starts */
	SInt64 data_bytes;    /* how much of it there is */
	SInt64 file_size;     /* the file as the OS sees it */
	SInt64 frames;        /* for PCM: data_bytes / bytesPerFrame; for MPEG: packets x samples per frame */

	/* **MPEG AUDIO HAS NO HEADER THAT COUNTS ITS FRAMES**, so the table below IS the container: one entry per MPEG
	 * frame, found by walking the stream's own sync words at open time. That is what makes a VBR file's duration
	 * exact rather than estimated, and what lets a packet be read by index without decoding everything before it. */
	struct fn_mp3_packet {
		SInt64 offset;
		UInt32 size;
	} *packet_table;
	UInt32 packet_count;
	UInt32 packet_capacity;

	/* **THE ENCODER'S GAPLESS DECLARATION, WHEN THE FIRST FRAME CARRIES ONE.** The streams' padding is not audio, and
	 * `mValidFrames` is what a duration should be; -1 priming means "this file declared nothing", which is the
	 * answer for every PCM container and for an MPEG file cut out of a longer one. */
	SInt32 mpeg_priming;
	SInt32 mpeg_remainder;

	bool   dirty;         /* a header still needs its sizes patched */

	/* Where each container keeps the numbers to patch. -1 when not applicable. */
	SInt64 patch_a;       /* WAV: RIFF size; AIFF: FORM size; CAF: data chunk size */
	SInt64 patch_b;       /* WAV: data size; AIFF: COMM frame count; CAF: unused */

	int     fd;
	void *  client;
	AudioFile_ReadProc     read_proc;
	AudioFile_WriteProc    write_proc;
	AudioFile_GetSizeProc  size_proc;
	AudioFile_SetSizeProc  set_size_proc;
};

/* ------------------------------------------------------------------------------------------------
 * Byte order.
 *
 * THE SCHEME IS PER CONTAINER AND NOT PER PLATFORM: WAV is little-endian throughout, AIFF and CAF are
 * big-endian throughout, and the sample data inherits the container's endianness — which is why an AIFF
 * description carries `kAudioFormatFlagIsBigEndian` and a WAV one does not.
 * ---------------------------------------------------------------------------------------------- */

static double fn_ldexp(double value, int exponent) { return ldexp(value, exponent); }
static double fn_frexp(double value, int *exponent) { return frexp(value, exponent); }

uint16_t fn_rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t fn_rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t fn_rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t fn_rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
uint64_t fn_rd64be(const uint8_t *p) { return ((uint64_t)fn_rd32be(p) << 32) | fn_rd32be(p + 4); }

static void fn_wr16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void fn_wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void fn_wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void fn_wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void fn_wr64be(uint8_t *p, uint64_t v) { fn_wr32be(p, (uint32_t)(v >> 32)); fn_wr32be(p + 4, (uint32_t)v); }

/* AIFF's sample rate is a 10-byte IEEE 754 extended float. Decoding it is twelve lines; encoding it is the
 * same twelve backwards, and both are checked by the round trip in the suite. */
double fn_rd_extended(const uint8_t *p)
{
	int exponent = (int)(((p[0] & 0x7F) << 8) | p[1]);
	uint64_t mantissa = fn_rd64be(p + 2);
	double value;

	if (exponent == 0 && mantissa == 0) {
		return 0.0;
	}
	if (exponent == 0x7FFF) {
		return 0.0;	/* infinity and NaN are not sample rates */
	}

	/* An 80-bit extended float is `mantissa * 2^(exponent - 16383 - 63)`, where the mantissa's top bit is
	 * an explicit 1 rather than the implicit one an IEEE double has. Getting the bias or the shift wrong
	 * yields a plausible-looking wrong rate — 48000 read back as 0.73 — which is why the suite checks it. */
	value = fn_ldexp((double)mantissa, exponent - 16383 - 63);

	return (p[0] & 0x80) ? -value : value;
}

static void fn_wr_extended(uint8_t *p, double value)
{
	double fraction;
	int exponent = 0;
	uint64_t mantissa;
	int field;

	memset(p, 0, 10);
	if (value <= 0.0) {
		return;
	}

	fraction = fn_frexp(value, &exponent);		/* value = fraction * 2^exponent, fraction in [0.5, 1) */
	field = exponent - 1 + 16383;
	mantissa = (uint64_t)(fraction * 18446744073709551616.0);	/* fraction * 2^64, top bit set */

	fn_wr16be(p, (uint16_t)(field & 0x7FFF));
	fn_wr64be(p + 2, mantissa);
}

/* ------------------------------------------------------------------------------------------------
 * I/O.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_read_at(AudioFileID file, SInt64 offset, void *buffer, UInt32 count, UInt32 *actual)
{
	*actual = 0;

	if (file->fd >= 0) {
		ssize_t got = pread(file->fd, buffer, count, (off_t)offset);

		if (got < 0) {
			return kAudioFileInvalidFileError;
		}
		*actual = (UInt32)got;
		return 0;
	}

	if (file->read_proc == NULL) {
		return kAudioFilePermissionsError;
	}

	return file->read_proc(file->client, offset, count, buffer, actual);
}

static OSStatus fn_write_at(AudioFileID file, SInt64 offset, const void *buffer, UInt32 count)
{
	UInt32 actual = 0;

	if (file->fd >= 0) {
		ssize_t put = pwrite(file->fd, buffer, count, (off_t)offset);

		if (put < 0 || (UInt32)put != count) {
			return kAudioFilePermissionsError;
		}
		return 0;
	}

	if (file->write_proc == NULL) {
		return kAudioFilePermissionsError;
	}

	{
		OSStatus status = file->write_proc(file->client, offset, count, buffer, &actual);

		if (status != 0) {
			return status;
		}
	}

	return actual == count ? 0 : kAudioFilePermissionsError;
}

static SInt64 fn_file_size(AudioFileID file)
{
	if (file->fd >= 0) {
		struct stat info;

		if (fstat(file->fd, &info) == 0) {
			return (SInt64)info.st_size;
		}
		return 0;
	}

	if (file->size_proc != NULL) {
		return file->size_proc(file->client);
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * WAV.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_wav_parse(AudioFileID file)
{
	uint8_t header[12];
	uint8_t chunk[8];
	UInt32 got = 0;
	SInt64 position = 12;
	OSStatus status;
	bool have_format = false;

	status = fn_read_at(file, 0, header, sizeof(header), &got);
	if (status != 0 || got != sizeof(header) || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		return kAudioFileInvalidFileError;
	}

	while (position + 8 <= file->file_size) {
		uint32_t size;

		status = fn_read_at(file, position, chunk, sizeof(chunk), &got);
		if (status != 0 || got != sizeof(chunk)) {
			break;
		}

		size = fn_rd32le(chunk + 4);

		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
			uint8_t format[40];
			UInt32 format_size = size < sizeof(format) ? size : (UInt32)sizeof(format);
			uint16_t tag;
			uint16_t channels;
			uint32_t rate;
			uint16_t block_align;
			uint16_t bits;

			status = fn_read_at(file, position + 8, format, format_size, &got);
			if (status != 0 || got < 16) {
				return kAudioFileInvalidFileError;
			}

			tag = fn_rd16le(format);
			channels = fn_rd16le(format + 2);
			rate = fn_rd32le(format + 4);
			block_align = fn_rd16le(format + 12);
			bits = fn_rd16le(format + 14);

			/* WAVE_FORMAT_EXTENSIBLE carries the real tag in the first two bytes of its sub-format
			 * GUID, and every 24-bit or multichannel file ever written uses it. */
			if (tag == 0xFFFE && got >= 40) {
				tag = fn_rd16le(format + 24);
				if (bits == 0 || bits == 32) {
					bits = fn_rd16le(format + 18);
				}
			}

			if (tag != 1 && tag != 3) {
				return kAudioFileUnsupportedDataFormatError;
			}

			memset(&file->asbd, 0, sizeof(file->asbd));
			file->asbd.mSampleRate = (Float64)rate;
			file->asbd.mFormatID = kAudioFormatLinearPCM;
			file->asbd.mChannelsPerFrame = channels;
			file->asbd.mBitsPerChannel = bits;
			file->asbd.mBytesPerFrame = block_align;
			file->asbd.mBytesPerPacket = block_align;
			file->asbd.mFramesPerPacket = 1;
			file->asbd.mFormatFlags = kAudioFormatFlagIsPacked;

			if (tag == 3) {
				file->asbd.mFormatFlags |= kAudioFormatFlagIsFloat;
			} else if (bits != 8) {
				/* 8-bit WAV is UNSIGNED, which is the one place the format's convention differs from
				 * every other integer depth. */
				file->asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
			}

			have_format = true;
		} else if (memcmp(chunk, "data", 4) == 0) {
			file->data_offset = position + 8;
			/* A size of 0xFFFFFFFF is "streamed, unknown"; so is a size past the end. Both mean "the rest
			 * of the file", which is what every reader in the world assumes. */
			if (size == 0xFFFFFFFFU || file->data_offset + (SInt64)size > file->file_size) {
				file->data_bytes = file->file_size - file->data_offset;
			} else {
				file->data_bytes = (SInt64)size;
			}
		}

		position += 8 + (SInt64)size + ((size & 1U) ? 1 : 0);
	}

	if (!have_format || file->data_offset == 0) {
		return kAudioFileInvalidFileError;
	}

	return 0;
}

static OSStatus fn_wav_create(AudioFileID file, const AudioStreamBasicDescription *asbd)
{
	uint8_t header[44];
	uint16_t tag;
	uint32_t bytes_per_frame;
	OSStatus status;

	/* WAV has one byte order, one interleaving and one sample layout per tag. Anything else is a
	 * different container's answer, and saying so beats writing a file nobody can read. */
	if ((asbd->mFormatFlags & kAudioFormatFlagIsBigEndian)
	    || (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
		return kAudioFileUnsupportedDataFormatError;
	}

	if (asbd->mBitsPerChannel != 8 && asbd->mBitsPerChannel != 16
	    && asbd->mBitsPerChannel != 24 && asbd->mBitsPerChannel != 32) {
		return kAudioFileUnsupportedDataFormatError;
	}

	tag = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) ? 3 : 1;
	bytes_per_frame = (asbd->mBitsPerChannel / 8) * asbd->mChannelsPerFrame;

	memcpy(header, "RIFF", 4);
	fn_wr32le(header + 4, 0);			/* patched on finalize */
	memcpy(header + 8, "WAVE", 4);
	memcpy(header + 12, "fmt ", 4);
	fn_wr32le(header + 16, 16);
	fn_wr16le(header + 20, tag);
	fn_wr16le(header + 22, (uint16_t)asbd->mChannelsPerFrame);
	fn_wr32le(header + 24, (uint32_t)asbd->mSampleRate);
	fn_wr32le(header + 28, (uint32_t)(asbd->mSampleRate * bytes_per_frame));
	fn_wr16le(header + 32, (uint16_t)bytes_per_frame);
	fn_wr16le(header + 34, (uint16_t)asbd->mBitsPerChannel);
	memcpy(header + 36, "data", 4);
	fn_wr32le(header + 40, 0);			/* patched on finalize */

	status = fn_write_at(file, 0, header, sizeof(header));
	if (status != 0) {
		return status;
	}

	file->asbd = *asbd;
	file->asbd.mFormatFlags &= ~kAudioFormatFlagIsNonInterleaved;
	file->asbd.mBytesPerFrame = bytes_per_frame;
	file->asbd.mBytesPerPacket = bytes_per_frame;
	file->asbd.mFramesPerPacket = 1;
	file->data_offset = sizeof(header);
	file->data_bytes = 0;
	file->frames = 0;
	file->patch_a = 4;
	file->patch_b = 40;
	file->dirty = true;

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * AIFF.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_aiff_parse(AudioFileID file)
{
	uint8_t header[12];
	uint8_t chunk[8];
	UInt32 got = 0;
	SInt64 position = 12;
	OSStatus status;
	bool have_comm = false;

	status = fn_read_at(file, 0, header, sizeof(header), &got);
	if (status != 0 || got != sizeof(header) || memcmp(header, "FORM", 4) != 0) {
		return kAudioFileInvalidFileError;
	}

	if (memcmp(header + 8, "AIFF", 4) != 0) {
		/* AIFC is a different container: it carries a compression type and a version, and this phase
		 * reads only uncompressed AIFF. */
		return kAudioFileUnsupportedFileTypeError;
	}

	while (position + 8 <= file->file_size) {
		uint32_t size;

		status = fn_read_at(file, position, chunk, sizeof(chunk), &got);
		if (status != 0 || got != sizeof(chunk)) {
			break;
		}

		size = fn_rd32be(chunk + 4);

		if (memcmp(chunk, "COMM", 4) == 0 && size >= 18) {
			uint8_t comm[18];

			status = fn_read_at(file, position + 8, comm, sizeof(comm), &got);
			if (status != 0 || got != sizeof(comm)) {
				return kAudioFileInvalidFileError;
			}

			memset(&file->asbd, 0, sizeof(file->asbd));
			file->asbd.mSampleRate = fn_rd_extended(comm + 8);
			file->asbd.mFormatID = kAudioFormatLinearPCM;
			file->asbd.mChannelsPerFrame = fn_rd16be(comm);
			file->asbd.mBitsPerChannel = fn_rd16be(comm + 6);
			file->asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
			                        | kAudioFormatFlagIsBigEndian;
			file->asbd.mBytesPerFrame = file->asbd.mChannelsPerFrame * ((file->asbd.mBitsPerChannel + 7) / 8);
			file->asbd.mBytesPerPacket = file->asbd.mBytesPerFrame;
			file->asbd.mFramesPerPacket = 1;
			have_comm = true;
		} else if (memcmp(chunk, "SSND", 4) == 0 && size >= 8) {
			uint8_t ssnd[8];

			status = fn_read_at(file, position + 8, ssnd, sizeof(ssnd), &got);
			if (status != 0 || got != sizeof(ssnd)) {
				return kAudioFileInvalidFileError;
			}

			file->data_offset = position + 8 + 8 + (SInt64)fn_rd32be(ssnd);
			file->data_bytes = (SInt64)size - 8 - (SInt64)fn_rd32be(ssnd);
			if (file->data_offset + file->data_bytes > file->file_size) {
				file->data_bytes = file->file_size - file->data_offset;
			}
		}

		position += 8 + (SInt64)size + ((size & 1U) ? 1 : 0);
	}

	if (!have_comm || file->data_offset == 0) {
		return kAudioFileInvalidFileError;
	}

	return 0;
}

static OSStatus fn_aiff_create(AudioFileID file, const AudioStreamBasicDescription *asbd)
{
	uint8_t header[62];
	uint32_t bytes_per_frame;
	OSStatus status;

	if ((asbd->mFormatFlags & kAudioFormatFlagIsFloat)
	    || (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
		/* AIFF's uncompressed form is signed big-endian integers, interleaved. A float AIFF is AIFC. */
		return kAudioFileUnsupportedDataFormatError;
	}

	if (asbd->mBitsPerChannel != 8 && asbd->mBitsPerChannel != 16
	    && asbd->mBitsPerChannel != 24 && asbd->mBitsPerChannel != 32) {
		return kAudioFileUnsupportedDataFormatError;
	}

	bytes_per_frame = (asbd->mBitsPerChannel / 8) * asbd->mChannelsPerFrame;

	memset(header, 0, sizeof(header));
	memcpy(header, "FORM", 4);
	fn_wr32be(header + 4, 0);			/* patched */
	memcpy(header + 8, "AIFF", 4);
	memcpy(header + 12, "COMM", 4);
	fn_wr32be(header + 16, 18);
	fn_wr16be(header + 20, (uint16_t)asbd->mChannelsPerFrame);
	fn_wr32be(header + 22, 0);			/* frames, patched */
	fn_wr16be(header + 26, (uint16_t)asbd->mBitsPerChannel);
	fn_wr_extended(header + 28, asbd->mSampleRate);
	memcpy(header + 38, "SSND", 4);
	fn_wr32be(header + 42, 0);			/* patched */
	fn_wr32be(header + 46, 0);			/* offset */
	fn_wr32be(header + 50, 0);			/* block size */

	status = fn_write_at(file, 0, header, 54);
	if (status != 0) {
		return status;
	}

	file->asbd = *asbd;
	file->asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
	                         | kAudioFormatFlagIsBigEndian;
	file->asbd.mFormatFlags &= ~kAudioFormatFlagIsNonInterleaved;
	file->asbd.mBytesPerFrame = bytes_per_frame;
	file->asbd.mBytesPerPacket = bytes_per_frame;
	file->asbd.mFramesPerPacket = 1;
	file->data_offset = 54;
	file->data_bytes = 0;
	file->frames = 0;
	file->patch_a = 4;		/* FORM size */
	file->patch_b = 22;		/* COMM frame count */
	file->dirty = true;

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * CAF.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_caf_parse(AudioFileID file)
{
	uint8_t header[8];
	UInt32 got = 0;
	SInt64 position = 8;
	OSStatus status;
	bool have_desc = false;

	status = fn_read_at(file, 0, header, sizeof(header), &got);
	if (status != 0 || got != sizeof(header) || memcmp(header, "caff", 4) != 0) {
		return kAudioFileInvalidFileError;
	}

	while (position + 12 <= file->file_size) {
		uint8_t chunk[12];
		uint64_t size;

		status = fn_read_at(file, position, chunk, sizeof(chunk), &got);
		if (status != 0 || got != sizeof(chunk)) {
			break;
		}

		size = fn_rd64be(chunk + 4);

		if (memcmp(chunk, "desc", 4) == 0 && size >= 32) {
			uint8_t desc[32];

			status = fn_read_at(file, position + 12, desc, sizeof(desc), &got);
			if (status != 0 || got != sizeof(desc)) {
				return kAudioFileInvalidFileError;
			}

			/* CAF stores the description big-endian and without `mReserved`, which is why this is not a
			 * memcpy of an `AudioStreamBasicDescription`. */
			memset(&file->asbd, 0, sizeof(file->asbd));
			{
				union { uint64_t bits; double value; } rate;

				rate.bits = fn_rd64be(desc);
				file->asbd.mSampleRate = rate.value;
			}
			file->asbd.mFormatID = fn_rd32be(desc + 8);
			file->asbd.mFormatFlags = fn_rd32be(desc + 12);
			file->asbd.mBytesPerPacket = fn_rd32be(desc + 16);
			file->asbd.mFramesPerPacket = fn_rd32be(desc + 20);
			file->asbd.mChannelsPerFrame = fn_rd32be(desc + 24);
			file->asbd.mBitsPerChannel = fn_rd32be(desc + 28);
			file->asbd.mBytesPerFrame = (file->asbd.mFramesPerPacket != 0)
			                          ? file->asbd.mBytesPerPacket / file->asbd.mFramesPerPacket : 0;
			have_desc = true;
		} else if (memcmp(chunk, "data", 4) == 0) {
			file->data_offset = position + 12 + 4;	/* past the edit count */
			if (size == 0xFFFFFFFFFFFFFFFFULL
			    || file->data_offset + (SInt64)(size - 4) > file->file_size) {
				file->data_bytes = file->file_size - file->data_offset;
			} else {
				file->data_bytes = (SInt64)size - 4;
			}
		}

		position += 12 + (SInt64)(size & 0x7FFFFFFFFFFFFFFFULL);
	}

	if (!have_desc || file->data_offset == 0) {
		return kAudioFileInvalidFileError;
	}

	return 0;
}

static OSStatus fn_caf_create(AudioFileID file, const AudioStreamBasicDescription *asbd)
{
	uint8_t header[68];
	uint32_t bytes_per_packet;
	uint32_t frames_per_packet;
	OSStatus status;

	bytes_per_packet = fn_pcm_bytes_per_frame(asbd);
	frames_per_packet = 1;

	memset(header, 0, sizeof(header));
	memcpy(header, "caff", 4);
	fn_wr16be(header + 4, 1);		/* version */
	fn_wr16be(header + 6, 0);		/* flags */
	memcpy(header + 8, "desc", 4);
	fn_wr64be(header + 12, 32);
	{
		union { uint64_t bits; double value; } rate;

		rate.value = asbd->mSampleRate;
		fn_wr64be(header + 20, rate.bits);
	}
	fn_wr32be(header + 28, asbd->mFormatID);
	fn_wr32be(header + 32, asbd->mFormatFlags);
	fn_wr32be(header + 36, bytes_per_packet);
	fn_wr32be(header + 40, frames_per_packet);
	fn_wr32be(header + 44, asbd->mChannelsPerFrame);
	fn_wr32be(header + 48, asbd->mBitsPerChannel);
	memcpy(header + 52, "data", 4);
	fn_wr64be(header + 56, 0);		/* patched: data bytes + 4 */
	fn_wr32be(header + 64, 0);		/* edit count */

	status = fn_write_at(file, 0, header, sizeof(header));
	if (status != 0) {
		return status;
	}

	file->asbd = *asbd;
	file->asbd.mBytesPerPacket = bytes_per_packet;
	file->asbd.mFramesPerPacket = 1;
	file->asbd.mBytesPerFrame = fn_pcm_bytes_per_frame(asbd);
	file->data_offset = sizeof(header);
	file->data_bytes = 0;
	file->frames = 0;
	file->patch_a = 56;		/* the data chunk's 64-bit size */
	file->patch_b = -1;
	file->dirty = true;

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * The header numbers, once the data is written.
 * ---------------------------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------------------------------
 * MPEG Layer III.
 *
 * **THE STREAM HAS NO HEADER, SO THE INDEX IS BUILT BY WALKING IT.** There is no frame count to read anywhere: the
 * only way to know how many frames a file holds — and therefore how long it is — is to walk from the first sync word
 * to the last. That walk happens once, at open, and its result is a table of (offset, size) that every later read
 * indexes. It is also what makes a VARIABLE-bitrate file exact: each frame's length comes from its own header.
 *
 * The alternative, dividing the byte count by the first frame's bitrate, is what a naive reader does, and it is
 * wrong by minutes on a VBR file — so this door does not offer it.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	SInt64       offset;
	fn_mpeg_frame frame;	/* the header, parsed by `AudioMpeg.c` and shared with the incremental door */
} fn_mp3_frame;

/* One COMPLETE frame at `at`: its header parses, and the whole frame is inside the file. A truncated final frame is
 * not a packet — an MP3 whose last frame is cut off is a file with a shorter last packet, not a broken file. */
static bool fn_mp3_frame_at(const uint8_t *data, SInt64 file_size, SInt64 at, fn_mp3_frame *out)
{
	if (at < 0 || at > file_size || file_size - at > (SInt64)0xFFFFFFFF) {
		return false;
	}
	if (!fn_mpeg_frame_header(data, (UInt32)file_size, (UInt32)at, &out->frame)) {
		return false;
	}
	if (at + (SInt64)out->frame.size > file_size) {
		return false;
	}
	out->offset = at;
	return true;
}

static bool fn_mp3_push(AudioFileID file, const fn_mp3_frame *frame)
{
	if (file->packet_count == file->packet_capacity) {
		UInt32 grown = (file->packet_capacity == 0) ? 1024 : file->packet_capacity * 2;
		void *table = realloc(file->packet_table, (size_t)grown * sizeof(*file->packet_table));

		if (table == NULL) {
			return false;
		}
		file->packet_table = table;
		file->packet_capacity = grown;
	}
	file->packet_table[file->packet_count].offset = frame->offset;
	file->packet_table[file->packet_count].size = frame->frame.size;
	file->packet_count++;
	return true;
}

static OSStatus fn_mp3_parse(AudioFileID file)
{
	uint8_t *data;
	SInt64 file_size = file->file_size;
	SInt64 position;
	UInt32 got = 0;
	fn_mp3_frame first;
	OSStatus status;

	/* **A FILE TOO BIG TO WALK IN MEMORY IS REFUSED RATHER THAN READ SLOWLY.** 8 MiB a minute is a 320 kbit/s
	 * MP3; the ceiling below is hours of it, and a file past it is not a song. */
	if (file_size <= 0 || file_size > (SInt64)(256u << 20)) {
		return kAudioFileUnsupportedFileTypeError;
	}

	data = malloc((size_t)file_size);
	if (data == NULL) {
		return kAudio_MemFullError;
	}
	status = fn_read_at(file, 0, data, (UInt32)file_size, &got);
	if (status != 0 || got != (UInt32)file_size) {
		free(data);
		return kAudioFileInvalidFileError;
	}

	position = 0;

	/* **AN ID3v2 TAG IS SKIPPED BY ITS OWN SIZE FIELD, before any sync is looked for**: it is allowed to contain
	 * 0xFF bytes, and a walk that started inside one would index 100 KB of artwork as audio. */
	{
		UInt32 tag = 0;

		if (fn_mpeg_id3v2_length(data, (UInt32)file_size, &tag) == 1 && tag > 0 && (SInt64)tag < file_size) {
			position = (SInt64)tag;
		}
	}

	memset(&first, 0, sizeof(first));

	file->mpeg_priming = -1;
	file->mpeg_remainder = -1;

	while (position + 4 <= file_size) {
		fn_mp3_frame frame;

		if (!fn_mp3_frame_at(data, file_size, position, &frame)) {
			position++;		/* resync by one byte: garbage, an APE tag, a Lyrics3 block */
			continue;
		}
		if (file->packet_count == 0) {
			first = frame;
		} else if (frame.frame.rate != first.frame.rate || frame.frame.channels != first.frame.channels) {
			/* A frame that disagrees about the format is not part of this stream — MPEG headers can be
			 * faked by chance in tag data, and one such frame would poison the description. */
			position++;
			continue;
		}
		if (!fn_mp3_push(file, &frame)) {
			free(data);
			return kAudio_MemFullError;
		}
		position = frame.offset + (SInt64)frame.frame.size;
	}

	/* **THE FILE'S BYTES OUTLIVE THE WALK, BECAUSE THE FIRST FRAME IS STILL READ BELOW.** The Xing/Info tag lives in
	 * that frame, so freeing here left the parse reading a block that glibc had already unmapped — a USE AFTER FREE
	 * that only fired on files big enough to be served by `mmap` (> 128 KiB, i.e. more than about five seconds at
	 * 192 kbit/s): MEASURED, a 1.2 MB MP3 segfaulted in `fn_mpeg_xing_parse` while the suite's 73 KB fixtures opened
	 * happily, which is why nothing caught it. The free is after the parse now. */
	if (file->packet_count == 0) {
		free(data);
		return kAudioFileUnsupportedFileTypeError;
	}

	memset(&file->asbd, 0, sizeof(file->asbd));
	file->asbd.mSampleRate = (Float64)first.frame.rate;
	file->asbd.mFormatID = kAudioFormatMPEGLayer3;
	file->asbd.mChannelsPerFrame = first.frame.channels;
	file->asbd.mFramesPerPacket = first.frame.samples;
	/* Bytes per packet and per frame stay ZERO: an MPEG frame's length is in its header, and a description that
	 * claimed a constant would be lying about every VBR file. */

	/* **THE FIRST FRAME MAY DECLARE THE GAPLESS TRIM, AND ONLY THE FIRST FRAME CAN.** It sits in the frame the walk
	 * started at, which is still in memory here; the frame that carries it is the encoder's declaration rather than
	 * audio, so it counts as priming along with the encoder's own delay. That is the arithmetic the decoder applies,
	 * measured frame for frame against `ffmpeg`'s output. */
	{
		fn_mpeg_xing tag;

		if (file->packet_table[0].offset >= 0 && file->packet_table[0].offset < file_size
		    && fn_mpeg_xing_parse(data + file->packet_table[0].offset,
		                          (UInt32)(file_size - file->packet_table[0].offset),
		                          &first.frame, &tag)
		    && tag.has_gapless) {
			file->mpeg_priming = (SInt32)(first.frame.samples + tag.delay);
			file->mpeg_remainder = (SInt32)tag.padding;
		}
	}

	free(data);

	file->data_offset = file->packet_table[0].offset;
	file->data_bytes = (file->packet_table[file->packet_count - 1].offset
	                    + (SInt64)file->packet_table[file->packet_count - 1].size) - file->data_offset;
	return 0;
}

static OSStatus fn_finalize(AudioFileID file)
{
	uint8_t patch[8];
	OSStatus status;
	SInt64 total = fn_file_size(file);

	if (!file->dirty) {
		return 0;
	}

	switch (file->type) {
	case kAudioFileWAVEType: {
		uint32_t riff = (uint32_t)(total - 8);
		uint32_t data = (uint32_t)file->data_bytes;

		fn_wr32le(patch, riff);
		status = fn_write_at(file, file->patch_a, patch, 4);
		if (status != 0) {
			return status;
		}
		fn_wr32le(patch, data);
		return fn_write_at(file, file->patch_b, patch, 4);
	}

	case kAudioFileAIFFType: {
		uint32_t form = (uint32_t)(total - 8);
		uint32_t frames = (uint32_t)file->frames;
		uint32_t ssnd = (uint32_t)(file->data_bytes + 8);

		fn_wr32be(patch, form);
		status = fn_write_at(file, file->patch_a, patch, 4);
		if (status != 0) {
			return status;
		}
		fn_wr32be(patch, frames);
		status = fn_write_at(file, file->patch_b, patch, 4);
		if (status != 0) {
			return status;
		}
		fn_wr32be(patch, ssnd);
		return fn_write_at(file, 42, patch, 4);	/* the SSND chunk's size */
	}

	case kAudioFileCAFType: {
		uint64_t size = (uint64_t)file->data_bytes + 4;

		fn_wr64be(patch, size);
		return fn_write_at(file, file->patch_a, patch, 8);
	}

	default:
		return kAudioFileUnsupportedFileTypeError;
	}
}

static void fn_update_frames(AudioFileID file)
{
	UInt32 bytes_per_frame = fn_pcm_bytes_per_frame(&file->asbd);

	if (file->type == kAudioFileMP3Type) {
		/* A packet is a frame of audio, not a byte: the sampres-per-frame is in every MPEG header. */
		file->frames = (SInt64)file->packet_count * (SInt64)file->asbd.mFramesPerPacket;
		return;
	}

	file->frames = (bytes_per_frame != 0) ? file->data_bytes / bytes_per_frame : 0;
}

/* ------------------------------------------------------------------------------------------------
 * Opening and creating.
 * ---------------------------------------------------------------------------------------------- */

static bool fn_path_for_url(CFURLRef url, char *buffer, size_t capacity)
{
	if (url == NULL || buffer == NULL || capacity == 0) {
		return false;
	}

	return CFURLGetFileSystemRepresentation(url, true, (UInt8 *)buffer, (CFIndex)capacity) ? true : false;
}

static OSStatus fn_open_fd(AudioFileID file, CFURLRef url, AudioFilePermissions permissions, AudioFileTypeID type)
{
	char path[4096];
	int flags;
	OSStatus status;

	if (!fn_path_for_url(url, path, sizeof(path))) {
		return kAudio_BadFilePathError;
	}

	if (permissions == kAudioFileReadPermission) {
		flags = O_RDONLY;
	} else if (permissions == kAudioFileWritePermission) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	file->fd = open(path, flags);
	if (file->fd < 0) {
		switch (errno) {
		case ENOENT: return kAudio_FileNotFoundError;
		case EACCES: return kAudio_FilePermissionError;
		default:     return kAudioFileInvalidFileError;
		}
	}

	file->file_size = fn_file_size(file);
	file->type = type;

	/* Sniff the container when the caller did not say, because the magic is the only honest source. */
	if (file->type == 0) {
		uint8_t magic[4];
		UInt32 got = 0;

		status = fn_read_at(file, 0, magic, sizeof(magic), &got);
		if (status != 0 || got != sizeof(magic)) {
			return kAudioFileInvalidFileError;
		}

		if (memcmp(magic, "RIFF", 4) == 0) {
			file->type = kAudioFileWAVEType;
		} else if (memcmp(magic, "FORM", 4) == 0) {
			file->type = kAudioFileAIFFType;
		} else if (memcmp(magic, "caff", 4) == 0) {
			file->type = kAudioFileCAFType;
		} else if (memcmp(magic, "ID3", 3) == 0 || (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0)) {
			/* **AN MP3 BEGINS WITH AN OPTIONAL ID3 TAG OR WITH A FRAME SYNC**, and those are its only two openings:
			 * there is no four-character magic, because the format has no header at all. */
			file->type = kAudioFileMP3Type;
		} else {
			return kAudioFileUnsupportedFileTypeError;
		}
	}

	switch (file->type) {
	case kAudioFileWAVEType:  status = fn_wav_parse(file); break;
	case kAudioFileAIFFType:  status = fn_aiff_parse(file); break;
	case kAudioFileCAFType:   status = fn_caf_parse(file); break;
	case kAudioFileMP3Type:   status = fn_mp3_parse(file); break;
	default:                  status = kAudioFileUnsupportedFileTypeError; break;
	}

	if (status == 0) {
		fn_update_frames(file);
	}

	return status;
}

OSStatus AudioFileOpenURL(CFURLRef inFileRef, AudioFilePermissions inPermissions,
                          AudioFileTypeID inFileTypeHint, AudioFileID *outAudioFile)
{
	AudioFileID file;
	OSStatus status;

	if (inFileRef == NULL || outAudioFile == NULL) {
		return kAudio_ParamError;
	}

	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return kAudio_MemFullError;
	}

	file->fd = -1;
	file->permissions = inPermissions;

	status = fn_open_fd(file, inFileRef, inPermissions, inFileTypeHint);
	if (status != 0) {
		if (file->fd >= 0) {
			close(file->fd);
		}
		free(file);
		return status;
	}

	*outAudioFile = file;
	return 0;
}

OSStatus AudioFileOpenWithCallbacks(void *inClientData, AudioFile_ReadProc inReadFunc,
                                    AudioFile_WriteProc inWriteFunc, AudioFile_GetSizeProc inGetSizeFunc,
                                    AudioFile_SetSizeProc inSetSizeFunc, AudioFileTypeID inFileTypeHint,
                                    AudioFileID *outAudioFile)
{
	AudioFileID file;
	OSStatus status;
	uint8_t magic[4];
	UInt32 got = 0;

	if (inReadFunc == NULL || inGetSizeFunc == NULL || outAudioFile == NULL) {
		return kAudio_ParamError;
	}

	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return kAudio_MemFullError;
	}

	file->fd = -1;
	file->client = inClientData;
	file->read_proc = inReadFunc;
	file->write_proc = inWriteFunc;
	file->size_proc = inGetSizeFunc;
	file->set_size_proc = inSetSizeFunc;
	file->permissions = (inWriteFunc != NULL) ? kAudioFileReadWritePermission : kAudioFileReadPermission;
	file->file_size = inGetSizeFunc(inClientData);
	file->type = inFileTypeHint;

	if (file->type == 0) {
		status = fn_read_at(file, 0, magic, sizeof(magic), &got);
		if (status != 0 || got != sizeof(magic)) {
			free(file);
			return kAudioFileInvalidFileError;
		}

		if (memcmp(magic, "RIFF", 4) == 0) {
			file->type = kAudioFileWAVEType;
		} else if (memcmp(magic, "FORM", 4) == 0) {
			file->type = kAudioFileAIFFType;
		} else if (memcmp(magic, "caff", 4) == 0) {
			file->type = kAudioFileCAFType;
		} else if (memcmp(magic, "ID3", 3) == 0 || (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0)) {
			file->type = kAudioFileMP3Type;
		} else {
			free(file);
			return kAudioFileUnsupportedFileTypeError;
		}
	}

	switch (file->type) {
	case kAudioFileWAVEType:  status = fn_wav_parse(file); break;
	case kAudioFileAIFFType:  status = fn_aiff_parse(file); break;
	case kAudioFileCAFType:   status = fn_caf_parse(file); break;
	case kAudioFileMP3Type:   status = fn_mp3_parse(file); break;
	default:                  status = kAudioFileUnsupportedFileTypeError; break;
	}

	if (status != 0) {
		free(file);
		return status;
	}

	fn_update_frames(file);
	*outAudioFile = file;
	return 0;
}

static OSStatus fn_create_with(AudioFileID file, AudioFileTypeID type,
                               const AudioStreamBasicDescription *format, AudioFileFlags flags)
{
	OSStatus status;

	if (format == NULL || format->mFormatID != kAudioFormatLinearPCM) {
		return kAudioFileUnsupportedDataFormatError;
	}

	file->type = type;
	file->permissions = kAudioFileReadWritePermission;
	file->dirty = false;
	file->patch_a = -1;
	file->patch_b = -1;

	switch (type) {
	case kAudioFileWAVEType:  status = fn_wav_create(file, format); break;
	case kAudioFileAIFFType:  status = fn_aiff_create(file, format); break;
	case kAudioFileCAFType:   status = fn_caf_create(file, format); break;
	default:                  status = kAudioFileUnsupportedFileTypeError; break;
	}

	(void)flags;
	return status;
}

OSStatus AudioFileCreateWithURL(CFURLRef inFileRef, AudioFileTypeID inFileType,
                                const AudioStreamBasicDescription *inFormat, AudioFileFlags inFlags,
                                AudioFileID *outAudioFile)
{
	char path[4096];
	AudioFileID file;
	OSStatus status;

	if (inFileRef == NULL || inFormat == NULL || outAudioFile == NULL) {
		return kAudio_ParamError;
	}

	if (!fn_path_for_url(inFileRef, path, sizeof(path))) {
		return kAudio_BadFilePathError;
	}

	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return kAudio_MemFullError;
	}

	file->fd = -1;

	/* `kAudioFileFlags_EraseFile` is the difference between "make a new file" and "fail because one is
	 * there". Without it, O_EXCL: a caller that did not ask to erase must not silently erase. */
	file->fd = open(path, O_RDWR | O_CREAT | ((inFlags & kAudioFileFlags_EraseFile) ? O_TRUNC : O_EXCL), 0644);
	if (file->fd < 0) {
		status = (errno == EACCES) ? kAudio_FilePermissionError : kAudioFileInvalidFileError;
		free(file);
		return status;
	}

	status = fn_create_with(file, inFileType, inFormat, inFlags);
	if (status != 0) {
		close(file->fd);
		unlink(path);
		free(file);
		return status;
	}

	*outAudioFile = file;
	return 0;
}

OSStatus AudioFileInitializeWithCallbacks(void *inClientData, AudioFile_ReadProc inReadFunc,
                                          AudioFile_WriteProc inWriteFunc, AudioFile_GetSizeProc inGetSizeFunc,
                                          AudioFile_SetSizeProc inSetSizeFunc, AudioFileTypeID inFileType,
                                          const AudioStreamBasicDescription *inFormat, AudioFileFlags inFlags,
                                          AudioFileID *outAudioFile)
{
	AudioFileID file;
	OSStatus status;

	if (inWriteFunc == NULL || inGetSizeFunc == NULL || inFormat == NULL || outAudioFile == NULL) {
		return kAudio_ParamError;
	}

	file = calloc(1, sizeof(*file));
	if (file == NULL) {
		return kAudio_MemFullError;
	}

	file->fd = -1;
	file->client = inClientData;
	file->read_proc = inReadFunc;
	file->write_proc = inWriteFunc;
	file->size_proc = inGetSizeFunc;
	file->set_size_proc = inSetSizeFunc;

	status = fn_create_with(file, inFileType, inFormat, inFlags);
	if (status != 0) {
		free(file);
		return status;
	}

	*outAudioFile = file;
	return 0;
}

OSStatus AudioFileOptimize(AudioFileID inAudioFile)
{
	if (inAudioFile == NULL) {
		return kAudio_ParamError;
	}

	fn_update_frames(inAudioFile);
	return fn_finalize(inAudioFile);
}

OSStatus AudioFileClose(AudioFileID inAudioFile)
{
	OSStatus status = 0;

	if (inAudioFile == NULL) {
		return kAudio_ParamError;
	}

	if (inAudioFile->dirty) {
		fn_update_frames(inAudioFile);
		status = fn_finalize(inAudioFile);
	}

	if (inAudioFile->fd >= 0) {
		close(inAudioFile->fd);
	}

	free(inAudioFile->packet_table);
	free(inAudioFile);
	return status;
}

/* ------------------------------------------------------------------------------------------------
 * Properties.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioFileGetPropertyInfo(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                                  UInt32 *outDataSize, UInt32 *outWritable)
{
	if (inAudioFile == NULL) {
		return kAudio_ParamError;
	}

	if (outWritable != NULL) {
		*outWritable = 0;
	}

	switch (inPropertyID) {
	case kAudioFilePropertyFileFormat:
		if (outDataSize) { *outDataSize = sizeof(AudioFileTypeID); }
		return 0;
	case kAudioFilePropertyDataFormat:
		if (outDataSize) { *outDataSize = sizeof(AudioStreamBasicDescription); }
		return 0;
	case kAudioFilePropertyDataOffset:
		if (outDataSize) { *outDataSize = sizeof(SInt64); }
		return 0;
	case kAudioFilePropertyAudioDataByteCount:
		if (outDataSize) { *outDataSize = sizeof(UInt64); }
		return 0;
	case kAudioFilePropertyAudioDataPacketCount:
		if (outDataSize) { *outDataSize = sizeof(UInt64); }
		return 0;
	case kAudioFilePropertyEstimatedDuration:
		if (outDataSize) { *outDataSize = sizeof(Float64); }
		return 0;
	case kAudioFilePropertyIsOptimized:
		if (outDataSize) { *outDataSize = sizeof(UInt32); }
		return 0;
	case kAudioFilePropertyPacketTableInfo:
		/* A file that declared no trim has no table to report, and saying so is better than reporting zeros: a
		 * zero priming with a zero remainder is a file whose every frame is audio. */
		if (inAudioFile != NULL && inAudioFile->mpeg_priming < 0) {
			return kAudioFileUnsupportedPropertyError;
		}
		if (outDataSize) { *outDataSize = sizeof(AudioFilePacketTableInfo); }
		return 0;
	default:
		return kAudioFileUnsupportedPropertyError;
	}
}

OSStatus AudioFileGetProperty(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                              UInt32 *ioDataSize, void *outPropertyData)
{
	if (inAudioFile == NULL || ioDataSize == NULL || outPropertyData == NULL) {
		return kAudio_ParamError;
	}

	fn_update_frames(inAudioFile);

	switch (inPropertyID) {
	case kAudioFilePropertyFileFormat:
		if (*ioDataSize < sizeof(AudioFileTypeID)) { return kAudioFileBadPropertySizeError; }
		*(AudioFileTypeID *)outPropertyData = inAudioFile->type;
		*ioDataSize = sizeof(AudioFileTypeID);
		return 0;

	case kAudioFilePropertyDataFormat:
		if (*ioDataSize < sizeof(AudioStreamBasicDescription)) { return kAudioFileBadPropertySizeError; }
		*(AudioStreamBasicDescription *)outPropertyData = inAudioFile->asbd;
		*ioDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioFilePropertyDataOffset:
		if (*ioDataSize < sizeof(SInt64)) { return kAudioFileBadPropertySizeError; }
		*(SInt64 *)outPropertyData = inAudioFile->data_offset;
		*ioDataSize = sizeof(SInt64);
		return 0;

	case kAudioFilePropertyAudioDataByteCount:
		if (*ioDataSize < sizeof(UInt64)) { return kAudioFileBadPropertySizeError; }
		*(UInt64 *)outPropertyData = (UInt64)inAudioFile->data_bytes;
		*ioDataSize = sizeof(UInt64);
		return 0;

	case kAudioFilePropertyAudioDataPacketCount:
		if (*ioDataSize < sizeof(UInt64)) { return kAudioFileBadPropertySizeError; }
		/* One packet per frame for PCM; one per MPEG frame otherwise, which is a different number. */
		*(UInt64 *)outPropertyData = (UInt64)((inAudioFile->type == kAudioFileMP3Type)
		                                    ? (SInt64)inAudioFile->packet_count
		                                    : inAudioFile->frames);
		*ioDataSize = sizeof(UInt64);
		return 0;

	case kAudioFilePropertyEstimatedDuration:
		if (*ioDataSize < sizeof(Float64)) { return kAudioFileBadPropertySizeError; }
		*(Float64 *)outPropertyData = (inAudioFile->asbd.mSampleRate > 0.0)
		                            ? (Float64)inAudioFile->frames / inAudioFile->asbd.mSampleRate
		                            : 0.0;
		*ioDataSize = sizeof(Float64);
		return 0;

	case kAudioFilePropertyIsOptimized: {
		if (*ioDataSize < sizeof(UInt32)) { return kAudioFileBadPropertySizeError; }
		*(UInt32 *)outPropertyData = inAudioFile->dirty ? 0 : 1;
		*ioDataSize = sizeof(UInt32);
		return 0;
	}

	case kAudioFilePropertyPacketTableInfo: {
		/* **THE THREE NUMBERS ADD UP TO THE PACKETS, WHICH IS THE INVARIANT A CLIENT CHECKS.** valid is what a
		 * duration should be; priming and remainder are what the decoder throws away at each end. */
		AudioFilePacketTableInfo info;

		if (inAudioFile->type != kAudioFileMP3Type || inAudioFile->mpeg_priming < 0) {
			return kAudioFileUnsupportedPropertyError;
		}
		if (*ioDataSize < sizeof(AudioFilePacketTableInfo)) { return kAudioFileBadPropertySizeError; }

		info.mPrimingFrames = inAudioFile->mpeg_priming;
		info.mRemainderFrames = inAudioFile->mpeg_remainder;
		info.mNumberValidFrames = (SInt64)inAudioFile->frames - (SInt64)info.mPrimingFrames
		                        - (SInt64)info.mRemainderFrames;
		if (info.mNumberValidFrames < 0) {
			info.mNumberValidFrames = 0;
		}
		*(AudioFilePacketTableInfo *)outPropertyData = info;
		*ioDataSize = sizeof(AudioFilePacketTableInfo);
		return 0;
	}

	default:
		return kAudioFileUnsupportedPropertyError;
	}
}

OSStatus AudioFileSetProperty(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                              UInt32 inPropertyDataSize, const void *inPropertyData)
{
	(void)inAudioFile;
	(void)inPropertyID;
	(void)inPropertyDataSize;
	(void)inPropertyData;

	/* Nothing here is writable yet: the format and the sizes are consequences of the audio, not settings.
	 * `kAudioFilePropertyDeferSizeUpdates` is the one Apple allows, and it arrives with a caller that
	 * wants it. */
	return kAudioFileUnsupportedPropertyError;
}

/* ------------------------------------------------------------------------------------------------
 * Bytes and packets.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioFileReadBytes(AudioFileID inAudioFile, Boolean inUseCache, SInt64 inStartingByte,
                            UInt32 *ioNumBytes, void *outBuffer)
{
	UInt32 wanted;
	UInt32 got = 0;
	OSStatus status;

	(void)inUseCache;

	if (inAudioFile == NULL || ioNumBytes == NULL || outBuffer == NULL) {
		return kAudio_ParamError;
	}

	if (inStartingByte < 0 || inStartingByte >= inAudioFile->data_bytes) {
		*ioNumBytes = 0;
		return 0;
	}

	wanted = *ioNumBytes;
	if ((SInt64)wanted > inAudioFile->data_bytes - inStartingByte) {
		wanted = (UInt32)(inAudioFile->data_bytes - inStartingByte);
	}

	status = fn_read_at(inAudioFile, inAudioFile->data_offset + inStartingByte, outBuffer, wanted, &got);
	if (status != 0) {
		return status;
	}

	*ioNumBytes = got;
	return 0;
}

OSStatus AudioFileWriteBytes(AudioFileID inAudioFile, Boolean inUseCache, SInt64 inStartingByte,
                             UInt32 *ioNumBytes, const void *inBuffer)
{
	OSStatus status;

	(void)inUseCache;

	if (inAudioFile == NULL || ioNumBytes == NULL || inBuffer == NULL) {
		return kAudio_ParamError;
	}

	status = fn_write_at(inAudioFile, inAudioFile->data_offset + inStartingByte, inBuffer, *ioNumBytes);
	if (status != 0) {
		return status;
	}

	if (inStartingByte + (SInt64)*ioNumBytes > inAudioFile->data_bytes) {
		inAudioFile->data_bytes = inStartingByte + (SInt64)*ioNumBytes;
	}

	inAudioFile->dirty = true;
	fn_update_frames(inAudioFile);
	return 0;
}

OSStatus AudioFileReadPacketData(AudioFileID inAudioFile, Boolean inUseCache, UInt32 *ioNumBytes,
                                 AudioStreamPacketDescription *outPacketDescriptions, SInt64 inStartingPacket,
                                 UInt32 *ioNumPackets, void *outBuffer)
{
	UInt32 bytes_per_frame;
	UInt32 frames_wanted;
	UInt32 frames_possible;
	UInt32 got = 0;
	SInt64 offset;
	OSStatus status;

	(void)inUseCache;
	(void)outPacketDescriptions;	/* constant bit rate: one frame per packet, nothing to describe */

	if (inAudioFile == NULL || ioNumBytes == NULL || ioNumPackets == NULL || outBuffer == NULL) {
		return kAudio_ParamError;
	}

	/* **A COMPRESSED PACKET IS READ BY INDEX, AND THE DESCRIPTIONS ARE THE ANSWER RATHER THAN AN OPTION.** Each
	 * MPEG frame is a different size, so the caller's byte capacity decides how many fit and `outPacketDescriptions`
	 * says where each one landed. A caller that passes NULL gets the concatenated bytes and no boundaries — which is
	 * exactly what the packet count is for, and why the table exists. */
	if (inAudioFile->type == kAudioFileMP3Type) {
		UInt32 bytes = 0;
		UInt32 packets = 0;
		SInt64 packet = inStartingPacket;

		while (packets < *ioNumPackets && packet >= 0 && packet < (SInt64)inAudioFile->packet_count) {
			UInt32 size = inAudioFile->packet_table[packet].size;
			UInt32 read = 0;
			OSStatus status;

			if (bytes + size > *ioNumBytes) {
				break;
			}
			status = fn_read_at(inAudioFile, inAudioFile->packet_table[packet].offset,
			                    (uint8_t *)outBuffer + bytes, size, &read);
			if (status != 0 || read != size) {
				break;
			}
			if (outPacketDescriptions != NULL) {
				outPacketDescriptions[packets].mStartOffset = (SInt64)bytes;
				outPacketDescriptions[packets].mVariableFramesInPacket =
				    inAudioFile->asbd.mFramesPerPacket;
				outPacketDescriptions[packets].mDataByteSize = size;
			}
			bytes += size;
			packets++;
			packet++;
		}

		*ioNumBytes = bytes;
		*ioNumPackets = packets;
		return 0;
	}

	bytes_per_frame = fn_pcm_bytes_per_frame(&inAudioFile->asbd);
	if (bytes_per_frame == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	fn_update_frames(inAudioFile);

	if (inStartingPacket >= inAudioFile->frames) {
		*ioNumPackets = 0;
		*ioNumBytes = 0;
		return 0;
	}

	frames_wanted = *ioNumPackets;
	frames_possible = (UInt32)(inAudioFile->frames - inStartingPacket);

	if (frames_wanted > frames_possible) {
		frames_wanted = frames_possible;
	}
	if (frames_wanted * bytes_per_frame > *ioNumBytes) {
		frames_wanted = *ioNumBytes / bytes_per_frame;
	}

	offset = inAudioFile->data_offset + inStartingPacket * (SInt64)bytes_per_frame;
	status = fn_read_at(inAudioFile, offset, outBuffer, frames_wanted * bytes_per_frame, &got);
	if (status != 0) {
		return status;
	}

	*ioNumPackets = got / bytes_per_frame;
	*ioNumBytes = got;
	return 0;
}

OSStatus AudioFileReadPackets(AudioFileID inAudioFile, Boolean inUseCache, UInt32 *outNumBytes,
                              AudioStreamPacketDescription *outPacketDescriptions, SInt64 inStartingPacket,
                              UInt32 *ioNumPackets, void *outBuffer)
{
	/* The deprecated shape of the same question, and Apple's own notes say it is. It differs only in
	 * taking the buffer size as an output. */
	UInt32 capacity = *outNumBytes;

	*outNumBytes = 0;
	return AudioFileReadPacketData(inAudioFile, inUseCache, &capacity, outPacketDescriptions,
	                               inStartingPacket, ioNumPackets, outBuffer);
}

OSStatus AudioFileWritePackets(AudioFileID inAudioFile, Boolean inUseCache, UInt32 inNumBytes,
                               const AudioStreamPacketDescription *inPacketDescriptions,
                               SInt64 inStartingPacket, UInt32 *ioNumPackets, const void *inBuffer)
{
	UInt32 bytes_per_frame;
	SInt64 offset;
	OSStatus status;

	(void)inUseCache;
	(void)inPacketDescriptions;

	if (inAudioFile == NULL || ioNumPackets == NULL || inBuffer == NULL) {
		return kAudio_ParamError;
	}

	bytes_per_frame = fn_pcm_bytes_per_frame(&inAudioFile->asbd);
	if (bytes_per_frame == 0) {
		return kAudioFileUnsupportedDataFormatError;
	}

	if (inNumBytes < *ioNumPackets * bytes_per_frame) {
		return kAudio_ParamError;
	}

	offset = inAudioFile->data_offset + inStartingPacket * (SInt64)bytes_per_frame;
	status = fn_write_at(inAudioFile, offset, inBuffer, *ioNumPackets * bytes_per_frame);
	if (status != 0) {
		return status;
	}

	{
		SInt64 end = inStartingPacket * (SInt64)bytes_per_frame + (SInt64)*ioNumPackets * bytes_per_frame;

		if (end > inAudioFile->data_bytes) {
			inAudioFile->data_bytes = end;
		}
	}

	inAudioFile->dirty = true;
	fn_update_frames(inAudioFile);
	return 0;
}
