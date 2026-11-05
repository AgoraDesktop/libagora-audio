/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * file.c — the three containers, written and read back byte for byte.
 *
 * BIT-EXACTNESS IS THE ONLY HONEST TEST OF A CONTAINER WRITER. A file that opens, reports the right format
 * and hands back samples that differ by one is a file that has quietly converted something it was asked to
 * store. So the suite writes a buffer of known bytes, closes, reopens, reads, and compares with `memcmp` —
 * for WAV, AIFF and CAF, in that order, because each has its own idea of where the size fields go.
 *
 * THE PROPERTIES ARE CHECKED SEPARATELY, because they are the file's answers about itself and the numbers a
 * client uses to size its buffers: byte count, frame count, duration, data offset.
 *
 * AND ONE CONVERSION THROUGH THE OTHER DOOR: `ExtAudioFile` with a client format that differs from the
 * file's — first a format change that must be exact, then a rate change that must produce the right number
 * of frames without falling over.
 */

#include <AudioToolbox/ExtendedAudioFile.h>
#include <CoreFoundation/CFURL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

/* A URL for a temporary path, and the path itself so the test can unlink it. */
static CFURLRef fn_temp_url(char *path, size_t capacity)
{
	char template_path[] = "/tmp/agora-audio-XXXXXX";
	int fd = mkstemp(template_path);
	CFURLRef url;
	size_t length;

	if (fd < 0) {
		return NULL;
	}
	close(fd);

	length = strlen(template_path);
	if (length + 1 > capacity) {
		return NULL;
	}
	memcpy(path, template_path, length + 1);

	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path,
	                                              (CFIndex)length, false);
	return url;
}

static AudioStreamBasicDescription fn_pcm(double rate, UInt32 channels, UInt32 bits, AudioFormatFlags flags)
{
	AudioStreamBasicDescription asbd;

	memset(&asbd, 0, sizeof(asbd));
	asbd.mSampleRate = rate;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = flags;
	asbd.mChannelsPerFrame = channels;
	asbd.mBitsPerChannel = bits;
	asbd.mBytesPerFrame = (bits / 8) * ((flags & kAudioFormatFlagIsNonInterleaved) ? 1 : channels);
	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	asbd.mFramesPerPacket = 1;

	return asbd;
}

/* ================================================================================================
 * The three containers, byte for byte.
 * ============================================================================================== */

static void fn_test_container(AudioFileTypeID type, const char *name, AudioFormatFlags flags,
                              UInt32 channels, UInt32 bits)
{
	char path[64];
	CFURLRef url = fn_temp_url(path, sizeof(path));
	AudioFileID file = NULL;
	AudioStreamBasicDescription format = fn_pcm(48000.0, channels, bits, flags);
	UInt32 frames = 1024;
	UInt32 bytes = frames * format.mBytesPerFrame;
	uint8_t *written;
	uint8_t *read_back;
	OSStatus status;
	UInt32 i;

	if (url == NULL) {
		fn_check("a temporary file is created", 0);
		return;
	}

	written = malloc(bytes);
	read_back = malloc(bytes);
	if (written == NULL || read_back == NULL) {
		fn_check("scratch allocation", 0);
		return;
	}

	for (i = 0; i < bytes; i++) {
		written[i] = (uint8_t)(i * 7 + 3);
	}

	status = AudioFileCreateWithURL(url, type, &format, kAudioFileFlags_EraseFile, &file);
	fn_check(name, status == 0 && file != NULL);
	if (status != 0) {
		goto cleanup;
	}

	{
		UInt32 count = bytes;

		status = AudioFileWriteBytes(file, false, 0, &count, written);
		fn_check("the audio is written", status == 0 && count == bytes);
	}

	status = AudioFileClose(file);
	fn_check("closing a written file finalizes its sizes", status == 0);
	file = NULL;

	status = AudioFileOpenURL(url, kAudioFileReadPermission, type, &file);
	fn_check("the file reopens", status == 0 && file != NULL);
	if (status != 0) {
		goto cleanup;
	}

	{
		AudioFileTypeID reported = 0;
		UInt32 size = sizeof(reported);

		status = AudioFileGetProperty(file, kAudioFilePropertyFileFormat, &size, &reported);
		fn_check("the format property is right", status == 0 && reported == type);
	}

	{
		AudioStreamBasicDescription described;
		UInt32 size = sizeof(described);

		memset(&described, 0, sizeof(described));
		status = AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &described);
		fn_check("the data format property is right",
		         status == 0
		         && described.mFormatID == kAudioFormatLinearPCM
		         && described.mChannelsPerFrame == channels
		         && described.mBitsPerChannel == bits
		         && described.mSampleRate == 48000.0);
	}

	{
		UInt64 count = 0;
		UInt32 size = sizeof(count);

		status = AudioFileGetProperty(file, kAudioFilePropertyAudioDataByteCount, &size, &count);
		fn_check("the byte count is the bytes written", status == 0 && count == bytes);
	}

	{
		UInt64 count = 0;
		UInt32 size = sizeof(count);

		status = AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &size, &count);
		fn_check("the packet count is the frames written", status == 0 && count == (UInt64)frames);
	}

	{
		Float64 duration = 0.0;
		UInt32 size = sizeof(duration);

		status = AudioFileGetProperty(file, kAudioFilePropertyEstimatedDuration, &size, &duration);
		fn_check("the duration is frames over rate",
		         status == 0 && duration > 1023.0 / 48000.0 && duration < 1025.0 / 48000.0);
	}

	{
		UInt32 count = bytes;

		status = AudioFileReadBytes(file, false, 0, &count, read_back);
		fn_check("the bytes read back are the bytes written",
		         status == 0 && count == bytes && memcmp(written, read_back, bytes) == 0);
	}

	/* The packet door, started at a frame boundary in the middle, is also bit-exact. */
	{
		UInt32 half = bytes / 2;
		UInt32 count = half;
		UInt32 packets = frames / 2;

		status = AudioFileReadPacketData(file, false, &count, NULL, frames / 2, &packets, read_back);
		fn_check("the packet door agrees with the byte door",
		         status == 0 && packets == frames / 2 && memcmp(written + half, read_back, half) == 0);
	}

	AudioFileClose(file);
	file = NULL;

cleanup:
	if (file != NULL) {
		AudioFileClose(file);
	}
	CFRelease(url);
	unlink(path);
	free(written);
	free(read_back);
}

/* ================================================================================================
 * ExtAudioFile: format conversion, then rate conversion.
 * ============================================================================================== */

static void fn_test_extended_conversion(void)
{
	char path[64];
	CFURLRef url = fn_temp_url(path, sizeof(path));
	ExtAudioFileRef writer = NULL;
	ExtAudioFileRef reader = NULL;
	AudioStreamBasicDescription file_format = fn_pcm(48000.0, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
	AudioStreamBasicDescription client_format = fn_pcm(48000.0, 1, 16, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
	int16_t source[256];
	int16_t read_back[256];
	OSStatus status;
	int i;

	if (url == NULL) {
		fn_check("a temporary file is created", 0);
		return;
	}

	for (i = 0; i < 256; i++) {
		source[i] = (int16_t)(-32768 + i * 257);
	}

	status = ExtAudioFileCreateWithURL(url, kAudioFileCAFType, &file_format, NULL,
	                                   kAudioFileFlags_EraseFile, &writer);
	fn_check("ExtAudioFile creates a file", status == 0 && writer != NULL);
	if (status != 0) {
		CFRelease(url);
		unlink(path);
		return;
	}

	/* The client writes int16; the file holds float32. */
	status = ExtAudioFileSetProperty(writer, kExtAudioFileProperty_ClientDataFormat,
	                                 sizeof(client_format), &client_format);
	fn_check("the client format is accepted", status == 0);

	{
		AudioBufferList list;

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = 1;
		list.mBuffers[0].mDataByteSize = (UInt32)sizeof(source);
		list.mBuffers[0].mData = source;

		status = ExtAudioFileWrite(writer, 256, &list);
		fn_check("the client's frames are written", status == 0);
	}

	ExtAudioFileDispose(writer);

	/* Read the same way round: the file says float32, the client asks for int16. */
	status = ExtAudioFileOpenURL(url, &reader);
	fn_check("the file reopens through the extended door", status == 0 && reader != NULL);
	if (status == 0) {
		SInt64 length = 0;
		UInt32 size = sizeof(length);

		status = ExtAudioFileSetProperty(reader, kExtAudioFileProperty_ClientDataFormat,
		                                 sizeof(client_format), &client_format);
		fn_check("the client format is accepted for reading", status == 0);

		status = ExtAudioFileGetProperty(reader, kExtAudioFileProperty_FileLengthFrames, &size, &length);
		fn_check("the file's frame count survived the conversion", status == 0 && length == 256);

		{
			AudioBufferList list;
			UInt32 frames = 256;

			memset(&list, 0, sizeof(list));
			list.mNumberBuffers = 1;
			list.mBuffers[0].mNumberChannels = 1;
			list.mBuffers[0].mDataByteSize = (UInt32)sizeof(read_back);
			list.mBuffers[0].mData = read_back;

			status = ExtAudioFileRead(reader, &frames, &list);
			fn_check("the frames read back", status == 0 && frames == 256);
			fn_check("int16 -> float32 -> int16 is bit-exact",
			         memcmp(source, read_back, sizeof(source)) == 0);
		}

		ExtAudioFileDispose(reader);
		reader = NULL;
	}

	CFRelease(url);
	unlink(path);
}

static void fn_test_extended_rate_change(void)
{
	char path[64];
	CFURLRef url = fn_temp_url(path, sizeof(path));
	ExtAudioFileRef writer = NULL;
	OSStatus status;
	float *source;
	UInt32 frames = 44100;
	UInt32 i;

	if (url == NULL) {
		fn_check("a temporary file is created", 0);
		return;
	}

	{
		AudioStreamBasicDescription file_format = fn_pcm(48000.0, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);
		AudioStreamBasicDescription client_format = fn_pcm(44100.0, 1, 32, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked);

		source = malloc((size_t)frames * sizeof(float));
		if (source == NULL) {
			CFRelease(url);
			unlink(path);
			return;
		}
		for (i = 0; i < frames; i++) {
			source[i] = (float)(0.25 * ((i % 100) - 50) / 50.0);
		}

		status = ExtAudioFileCreateWithURL(url, kAudioFileCAFType, &file_format, NULL,
		                                   kAudioFileFlags_EraseFile, &writer);
		fn_check("a rate-changing writer is created", status == 0 && writer != NULL);

		if (status == 0) {
			status = ExtAudioFileSetProperty(writer, kExtAudioFileProperty_ClientDataFormat,
			                                 sizeof(client_format), &client_format);
			fn_check("a client format at another rate is accepted", status == 0);

			{
				AudioBufferList list;

				memset(&list, 0, sizeof(list));
				list.mNumberBuffers = 1;
				list.mBuffers[0].mNumberChannels = 1;
				list.mBuffers[0].mDataByteSize = frames * (UInt32)sizeof(float);
				list.mBuffers[0].mData = source;

				status = ExtAudioFileWrite(writer, frames, &list);
				fn_check("the rate-changing write succeeds", status == 0);
			}

			ExtAudioFileDispose(writer);

			/* The file's own frame count is in ITS rate, so there should be about 48000 frames. */
			{
				ExtAudioFileRef probe = NULL;

				status = ExtAudioFileOpenURL(url, &probe);
				if (status == 0) {
					SInt64 length = 0;
					UInt32 size = sizeof(length);

					status = ExtAudioFileGetProperty(probe, kExtAudioFileProperty_FileLengthFrames, &size,
					                                 &length);
					fn_check("the file holds frames at its own rate",
					         status == 0 && length > 47000 && length < 49000);
					ExtAudioFileDispose(probe);
				} else {
					fn_check("the rate-changed file reopens", 0);
				}
			}
		}

		free(source);
	}

	CFRelease(url);
	unlink(path);
}

/* ================================================================================================
 * Refusals.
 * ============================================================================================== */

static void fn_test_refusals(void)
{
	char path[64];
	CFURLRef url = fn_temp_url(path, sizeof(path));
	AudioFileID file = NULL;
	OSStatus status;

	if (url == NULL) {
		fn_check("a temporary file is created", 0);
		return;
	}

	/* AAC cannot be stored here: no codec, and the answer says which. */
	{
		AudioStreamBasicDescription aac = fn_pcm(44100.0, 2, 16, kAudioFormatFlagIsSignedInteger);

		aac.mFormatID = kAudioFormatMPEG4AAC;
		status = AudioFileCreateWithURL(url, kAudioFileCAFType, &aac, kAudioFileFlags_EraseFile, &file);
		fn_check("an AAC file is refused by name", status == kAudioFileUnsupportedDataFormatError);
	}

	/* A container this does not know is refused by name too. */
	{
		AudioStreamBasicDescription pcm = fn_pcm(44100.0, 2, 16, kAudioFormatFlagIsSignedInteger);

		status = AudioFileCreateWithURL(url, 'M4Af', &pcm, kAudioFileFlags_EraseFile, &file);
		fn_check("an unsupported container is refused by name",
		         status == kAudioFileUnsupportedFileTypeError);
	}

	/* A WAV cannot hold big-endian or non-interleaved audio; that is a container limitation, not a refusal
	 * of the audio. */
	{
		AudioStreamBasicDescription big = fn_pcm(44100.0, 2, 16,
		                                         kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian);

		status = AudioFileCreateWithURL(url, kAudioFileWAVEType, &big, kAudioFileFlags_EraseFile, &file);
		fn_check("WAV refuses big-endian audio", status == kAudioFileUnsupportedDataFormatError);
	}

	/* A file that is not one of the three. */
	{
		FILE *handle = fopen(path, "wb");

		if (handle != NULL) {
			fputs("not an audio file at all", handle);
			fclose(handle);
		}

		status = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
		fn_check("garbage is refused as an unsupported type",
		         status == kAudioFileUnsupportedFileTypeError);
	}

	CFRelease(url);
	unlink(path);
}

int main(void);
int main(void)
{
	fn_test_container(kAudioFileWAVEType, "WAV round-trips bit-exactly",
	                  kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 2, 16);
	fn_test_container(kAudioFileAIFFType, "AIFF round-trips bit-exactly",
	                  kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
	                  | kAudioFormatFlagIsBigEndian, 1, 16);
	fn_test_container(kAudioFileCAFType, "CAF round-trips bit-exactly",
	                  kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 2, 32);

	fn_test_extended_conversion();
	fn_test_extended_rate_change();
	fn_test_refusals();

	if (g_failures != 0) {
		fprintf(stderr, "file: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("file: WAV, AIFF and CAF round-trip bit-exactly; ExtAudioFile converts; four refusals answered\n");
	return 0;
}
