/*
 * mp3.c — the first compressed format: what the container says, and whether the samples are audio.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE FIXTURES ARE TONES, AND THAT IS WHAT MAKES THE SAMPLES CHECKABLE.** A decoder that returns the right NUMBER of
 * frames of noise passes a frame count; counting zero crossings says whether what came out is the tone that went in.
 * The files are generated with `ffmpeg` when the suite is configured, because an MP3 is an encoder's output and no
 * checked-in blob would explain itself; without `ffmpeg` the suite says so and skips.
 *
 * TWO STREAMS, BECAUSE THE FORMAT HAS TWO SHAPES AND THE PARSER MUST KNOW BOTH: a 44.1 kHz stereo MPEG-1 file, whose
 * frames hold 1152 samples, and a 22.05 kHz mono MPEG-2 file, whose frames hold 576. A reader that assumed MPEG-1
 * would report the second stream at exactly twice its real duration, which is the kind of defect a duration in a
 * window hides and this suite does not.
 *
 * **THE CONTAINER COUNTS MORE FRAMES THAN THE DECODER PRODUCES, AND THE DIFFERENCE IS THE ENCODER'S.** MPEG frames
 * are fixed-size buckets, so the encoder pads: it declares its delay and padding in the LAME tag, and libmpg123 trims
 * both, which is why the decoded count is the tone's exact length and the packet count is not. Both numbers are
 * asserted here — the file door reports the buckets, the decoder reports the audio — and a pitch count ties the
 * second to the tone that was fed in.
 */
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioFileStream.h>
#include <AudioToolbox/ExtendedAudioFile.h>
#include <CoreFoundation/CFURL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fn_total;
static int fn_failures;

static void fn_check(const char *what, int ok)
{
	fn_total++;
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		fn_failures++;
	}
}

static CFURLRef fn_url(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path,
	                                               (CFIndex)strlen(path), false);
}

/* The pitch of interleaved frames, from the left channel, with the peak alongside it. Two crossings per cycle. */
typedef struct {
	long long frames;
	int       crossings;
	SInt16    previous;
	double    peak;
} fn_tone;

/* One read's worth. Returns 0 at end of stream, -1 on error, 1 when frames were produced. */
static int fn_read(ExtAudioFileRef ext, UInt32 channels, UInt32 want, void *buffer, UInt32 *out_frames,
                   OSStatus *out_status)
{
	AudioBufferList list;
	UInt32 wanted = want;
	OSStatus status;

	list.mNumberBuffers = 1;
	list.mBuffers[0].mNumberChannels = channels;
	list.mBuffers[0].mDataByteSize = (UInt32)(sizeof(SInt16) * want * channels);
	list.mBuffers[0].mData = buffer;

	status = ExtAudioFileRead(ext, &wanted, &list);
	*out_frames = wanted;
	*out_status = status;
	return status != 0 ? -1 : (wanted > 0 ? 1 : 0);
}

/* Read the whole stream at the file's own format, measuring the tone. */
static long long fn_decode_all(ExtAudioFileRef ext, UInt32 channels, fn_tone *tone, OSStatus *out_status)
{
	SInt16 samples[4096 * 2];
	long long total = 0;

	memset(tone, 0, sizeof(*tone));
	for (;;) {
		UInt32 got = 0;
		OSStatus status = 0;
		int step = fn_read(ext, channels, 4096, samples, &got, &status);

		if (step < 0) {
			*out_status = status;
			break;
		}
		if (step == 0) {
			*out_status = 0;
			break;
		}

		{
			UInt32 i;

			for (i = 0; i < got; i++) {
				SInt16 sample = samples[i * channels];
				double magnitude = fabs((double)sample / 32768.0);

				if (magnitude > tone->peak) {
					tone->peak = magnitude;
				}
				if ((tone->previous < 0 && sample >= 0) || (tone->previous >= 0 && sample < 0)) {
					tone->crossings++;
				}
				tone->previous = sample;
			}
		}
		total += got;
	}
	tone->frames = total;
	return total;
}

static double fn_frequency(const fn_tone *tone, double rate)
{
	double seconds = (rate > 0.0) ? (double)tone->frames / rate : 0.0;

	return (seconds > 0.0) ? (double)tone->crossings / (2.0 * seconds) : 0.0;
}

/* One stream, through both doors: the container's own account of it, then the decoder's. */
static int fn_stream(const char *path, double rate, UInt32 channels, UInt32 samples_per_frame, double tone,
                     double seconds)
{
	CFURLRef url = fn_url(path);
	AudioFileID file = NULL;
	ExtAudioFileRef ext = NULL;
	AudioStreamBasicDescription format;
	UInt64 packets = 0;
	SInt64 length = 0;
	OSStatus status;
	fn_tone measured;
	UInt32 size;

	printf("— %s\n", path);

	status = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
	if (status != 0) {
		printf("skip  no fixture at %s (status %d)\n", path, (int)status);
		CFRelease(url);
		return 77;
	}

	{
		AudioFileTypeID type = 0;

		size = sizeof(type);
		fn_check("the file is MPEG audio",
		         AudioFileGetProperty(file, kAudioFilePropertyFileFormat, &size, &type) == 0
		         && type == kAudioFileMP3Type);
	}

	size = sizeof(format);
	fn_check("the container describes the audio",
	         AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format) == 0);
	fn_check("the codec is MPEG Layer III", format.mFormatID == kAudioFormatMPEGLayer3);
	fn_check("the rate is the file's", format.mSampleRate == rate);
	fn_check("the channel count is the file's", format.mChannelsPerFrame == channels);
	fn_check("the samples per frame are the version's", format.mFramesPerPacket == samples_per_frame);
	fn_check("a compressed frame has no constant byte size",
	         format.mBytesPerPacket == 0 && format.mBytesPerFrame == 0);

	size = sizeof(packets);
	fn_check("the packet count is exact",
	         AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &size, &packets) == 0
	         && packets > 10);
	fn_check("the packet count is the duration the frames hold",
	         llabs((long long)packets * samples_per_frame - (long long)(seconds * rate))
	         < (long long)(5 * samples_per_frame));

	/* **EVERY PACKET IS READABLE BY INDEX**, which is what the table exists for: the last packet is as reachable as
	 * the first, and its size comes from its own header rather than from an average. */
	{
		UInt8 buffer[4096];
		UInt32 capacity = sizeof(buffer);
		UInt32 count = 1;
		AudioStreamPacketDescription description;
		SInt64 last = (SInt64)packets - 1;

		memset(&description, 0, sizeof(description));
		status = AudioFileReadPacketData(file, false, &capacity, &description, last, &count, buffer);
		fn_check("the last packet reads by index",
		         status == 0 && count == 1 && capacity > 0 && description.mDataByteSize == capacity);
		fn_check("an MPEG frame is a few dozen to a few hundred bytes", capacity >= 24 && capacity < 2000);
		fn_check("the packet description counts the frame's samples",
		         description.mVariableFramesInPacket == samples_per_frame);
	}

	AudioFileClose(file);

	/* THE FILE DOOR AGAIN, NOW WITH THE DECODER BEHIND IT. No client format is set, so this is the decoder's own
	 * output path: PCM16 at the file's rate and channel count, copied out of the staging buffer. */
	status = ExtAudioFileOpenURL(url, &ext);
	if (status == kAudioFileUnsupportedDataFormatError) {
		/* The container was walked — every check above passed — and this build has no decoder for it, which is
		 * what `-DAGORA_AUDIO_WITH_MPG123=OFF` is. A skip rather than a failure, said out loud. */
		printf("skip  this build has no MPEG decoder (the container still reads)\n");
		CFRelease(url);
		return 77;
	}
	fn_check("the extended door opens the stream", status == 0 && ext != NULL);
	if (status != 0) {
		CFRelease(url);
		return 1;
	}

	size = sizeof(length);
	fn_check("the length is the frames the container counted",
	         ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &size, &length) == 0
	         && length == (SInt64)packets * samples_per_frame);

	{
		OSStatus read_status = 0;
		long long decoded = fn_decode_all(ext, channels, &measured, &read_status);
		long long expected = (long long)(seconds * rate + 0.5);

		fn_check("the whole stream decodes", read_status == 0 && decoded > 0);
		/* **THE TRIMMED LENGTH IS EXACTLY THE TONE'S**, because libmpg123 applies the encoder's own gapless
		 * declaration. The container's count is larger by the padding and is asserted above, separately. */
		fn_check("the decoder gives back exactly the audio that was encoded", decoded == expected);
		fn_check("the decoder's frames fit inside the container's", decoded <= length);
		fn_check("the samples are not silence", measured.peak > 0.05 && measured.peak < 0.5);
		printf("      %lld frames (container %lld), peak %.3f, %.0f Hz (expected %.0f)\n",
		       decoded, (long long)length, measured.peak, fn_frequency(&measured, rate), tone);
		{
			double frequency = fn_frequency(&measured, rate);

			fn_check("the decoded audio is the tone that was encoded",
			         frequency > tone * 0.95 && frequency < tone * 1.05);
		}
	}

	ExtAudioFileDispose(ext);
	CFRelease(url);
	return 0;
}

/* **A RATE CHANGE IS THE CONVERTER'S WORK, ON TOP OF THE DECODER'S.** The client asks for float32 at 48 kHz, so the
 * staged PCM16 goes through `AudioConverter` — the path a player takes when its device is not the file's rate. The
 * number of frames is the ratio the converter promises and the pitch is still the file's. */
static int fn_rate_change(const char *path, double file_rate, double tone)
{
	CFURLRef url = fn_url(path);
	ExtAudioFileRef ext = NULL;
	AudioStreamBasicDescription client;
	OSStatus status;
	float samples[4096 * 2];
	long long total = 0;
	int crossings = 0;
	float previous = 0.0f;
	double peak = 0.0;
	UInt32 i;
	UInt32 size;

	printf("— %s, converted to 48 kHz float32\n", path);

	status = ExtAudioFileOpenURL(url, &ext);
	fn_check("the extended door opens the stream for conversion", status == 0 && ext != NULL);
	if (status != 0) {
		CFRelease(url);
		return 1;
	}

	memset(&client, 0, sizeof(client));
	client.mSampleRate = 48000.0;
	client.mFormatID = kAudioFormatLinearPCM;
	client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	client.mChannelsPerFrame = 2;
	client.mBitsPerChannel = 32;
	client.mFramesPerPacket = 1;
	client.mBytesPerFrame = 2 * sizeof(float);
	client.mBytesPerPacket = 2 * sizeof(float);
	size = sizeof(client);
	status = ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, size, &client);
	fn_check("a decoded stream accepts a client format", status == 0);

	for (;;) {
		AudioBufferList list;
		UInt32 wanted = 4096;

		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = 2;
		list.mBuffers[0].mDataByteSize = (UInt32)sizeof(samples);
		list.mBuffers[0].mData = samples;

		status = ExtAudioFileRead(ext, &wanted, &list);
		if (status != 0 || wanted == 0) {
			break;
		}
		for (i = 0; i < wanted; i++) {
			float value = samples[i * 2];	/* the left channel, as everywhere else here */
			double magnitude = fabs((double)value);

			if (magnitude > peak) {
				peak = magnitude;
			}
			if ((previous < 0.0f && value >= 0.0f) || (previous >= 0.0f && value < 0.0f)) {
				crossings++;
			}
			previous = value;
		}
		total += wanted;
	}

	fn_check("the conversion produces frames", total > 0);
	{
		double seconds = (double)total / 48000.0;
		double frequency = (seconds > 0.0) ? (double)crossings / (2.0 * seconds) : 0.0;

		printf("      %lld frames at 48 kHz, peak %.3f, %.0f Hz (file is %.0f Hz at %.0f)\n",
		       total, peak, frequency, tone, file_rate);
		fn_check("the pitch survives the rate change", frequency > tone * 0.95 && frequency < tone * 1.05);
	}

	ExtAudioFileDispose(ext);
	CFRelease(url);
	return 0;
}

/* **A SEEK RESTARTS THE DECODER AT THE FRAME THAT HOLDS THE SAMPLE ASKED FOR.** The position comes back in the
 * client's frames, and the audio after the jump is still the tone — which is the part a byte offset could get wrong
 * silently. Alignment is frame-accurate rather than sample-accurate: the encoder's delay is only declared in the
 * stream's first frame, so a decoder entered mid-stream cannot trim it. */
static int fn_seek(const char *path, double rate, UInt32 channels, double tone, double seconds)
{
	CFURLRef url = fn_url(path);
	ExtAudioFileRef ext = NULL;
	OSStatus status;
	SInt64 target = (SInt64)(rate * seconds / 2.0);
	SInt64 tell = 0;

	printf("— %s, seeking to %.2f s\n", path, (double)target / rate);

	status = ExtAudioFileOpenURL(url, &ext);
	if (status != 0) {
		CFRelease(url);
		return 0;
	}

	/* **A SEEK TO ZERO IS BIT-EXACT, AND THAT IS THE ASSERTION A BYTE OFFSET CANNOT FAKE.** The first 4096 frames
	 * after seeking back to the start are compared with the frames a fresh stream produced: the restart has to
	 * re-read the first frame, where the encoder declares its delay, or the audio slides. */
	{
		static SInt16 reference[4096 * 2];
		static SInt16 again[4096 * 2];
		UInt32 want = 4096;
		UInt32 got_reference = 0;
		UInt32 got_again = 0;
		OSStatus first = 0;
		OSStatus second = 0;

		fn_check("the stream reads from the start",
		         fn_read(ext, channels, want, reference, &got_reference, &first) == 1
		         && got_reference == want);

		status = ExtAudioFileSeek(ext, 0);
		fn_check("the stream seeks back to the start", status == 0);

		fn_check("the stream reads again after seeking to zero",
		         fn_read(ext, channels, want, again, &got_again, &second) == 1
		         && got_again == got_reference);
		fn_check("a seek to zero restarts the audio exactly",
		         memcmp(reference, again, (size_t)got_reference * channels * sizeof(SInt16)) == 0);
	}

	status = ExtAudioFileSeek(ext, target);
	fn_check("an MPEG stream seeks", status == 0);
	status = ExtAudioFileTell(ext, &tell);
	fn_check("the position is the frame that was asked for", status == 0 && tell == target);

	{
		SInt16 samples[1024 * 2];
		fn_tone measured;
		UInt32 i;
		long long settle = 2 * 1152;	/* the bit reservoir is lost by a jump: two frames to resynchronise */

		memset(&measured, 0, sizeof(measured));
		for (;;) {
			UInt32 got = 0;
			OSStatus read_status = 0;
			int step = fn_read(ext, channels, 1024, samples, &got, &read_status);

			if (step <= 0) {
				break;
			}
			for (i = 0; i < got; i++) {
				SInt16 sample = samples[i * channels];

				if (settle > 0) {
					settle--;
					continue;
				}
				if ((measured.previous < 0 && sample >= 0) || (measured.previous >= 0 && sample < 0)) {
					measured.crossings++;
				}
				measured.previous = sample;
				measured.frames++;
			}
			if (measured.frames > (long long)(rate / 4.0)) {
				break;		/* a quarter second is plenty for a pitch count */
			}
		}

		printf("      %lld frames after the seek, %.0f Hz (expected %.0f)\n",
		       measured.frames, fn_frequency(&measured, rate), tone);
		fn_check("the audio after the seek is still the tone",
		         fn_frequency(&measured, rate) > tone * 0.98 && fn_frequency(&measured, rate) < tone * 1.02);

		status = ExtAudioFileTell(ext, &tell);
		fn_check("the position advances past what was read", status == 0 && tell > target);
	}

	/* And backwards, which is the direction a byte-offset-only seek gets wrong. */
	status = ExtAudioFileSeek(ext, (SInt64)(rate * seconds / 8.0));
	fn_check("the stream seeks backwards", status == 0);
	{
		SInt16 samples[1024 * 2];
		UInt32 got = 0;
		OSStatus read_status = 0;

		fn_check("reading after a backwards seek produces frames",
		         fn_read(ext, channels, 1024, samples, &got, &read_status) == 1 && got > 0);
	}

	ExtAudioFileDispose(ext);
	CFRelease(url);
	return 0;
}

/* The fake stream the last refusal reads: one plausible MPEG-1 frame header (128 kbit/s, 44.1 kHz, so the frame
 * length the header implies is 417 bytes) and too few bytes behind it for that frame to exist. */
typedef struct {
	UInt8 bytes[100];
} fn_fake;

static OSStatus fake_read(void *client, SInt64 position, UInt32 request, void *buffer, UInt32 *actual)
{
	fn_fake *fake = client;
	size_t available;

	if (position < 0 || position > (SInt64)sizeof(fake->bytes)) {
		*actual = 0;
		return 0;
	}
	available = sizeof(fake->bytes) - (size_t)position;
	if (available > request) {
		available = request;
	}
	memcpy(buffer, fake->bytes + position, available);
	*actual = (UInt32)available;
	return 0;
}

static SInt64 fake_size(void *client)
{
	fn_fake *fake = client;

	(void)fake;
	return (SInt64)sizeof(fake->bytes);
}

/* The stream door's two callbacks, which this suite never lets run: the file is refused before anything is heard. */
static void fake_property(void *client, AudioFileStreamID stream, AudioFileStreamPropertyID property,
                          UInt32 *flags)
{
	(void)client;
	(void)stream;
	(void)property;
	(void)flags;
}

static void fake_packets(void *client, UInt32 count, UInt32 bytes, const void *data,
                         AudioStreamPacketDescription *descriptions)
{
	(void)client;
	(void)count;
	(void)bytes;
	(void)data;
	(void)descriptions;
}

/* The refusals, each by name, because a format that is read is not a format that is written and not a stream that
 * can be parsed incrementally. */
static int fn_refusals(const char *path)
{
	CFURLRef out = fn_url("/tmp/agora-mp3-write-probe.mp3");
	fn_fake fake;
	AudioStreamBasicDescription format;
	AudioFileID file = NULL;
	AudioFileStreamID stream = NULL;
	OSStatus status;
	FILE *handle;
	UInt8 bytes[4096];
	size_t got;

	memset(&fake, 0, sizeof(fake));
	fake.bytes[0] = 0xFF;
	fake.bytes[1] = 0xFB;		/* MPEG-1 Layer III, no CRC, 128 kbit/s, 44.1 kHz */
	fake.bytes[2] = 0x90;
	fake.bytes[3] = 0x00;

	/* An MP3 is read here and not written: there is no encoder. */
	memset(&format, 0, sizeof(format));
	format.mSampleRate = 44100.0;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	format.mChannelsPerFrame = 2;
	format.mBitsPerChannel = 16;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = 4;
	format.mBytesPerPacket = 4;
	status = AudioFileCreateWithURL(out, kAudioFileMP3Type, &format, kAudioFileFlags_EraseFile, &file);
	fn_check("writing MPEG audio is refused by name", status == kAudioFileUnsupportedFileTypeError);
	if (status == 0) {
		AudioFileClose(file);
	}
	CFRelease(out);

	/* The incremental door still parses only the three containers: this door has no MPEG walker. */
	{
		status = AudioFileStreamOpen(NULL, fake_property, fake_packets, 0, &stream);
		fn_check("the incremental door opens", status == 0 && stream != NULL);

		handle = fopen(path, "rb");
		got = (handle != NULL) ? fread(bytes, 1, sizeof(bytes), handle) : 0;
		if (handle != NULL) {
			fclose(handle);
		}
		status = AudioFileStreamParseBytes(stream, (UInt32)got, bytes, 0);
		fn_check("an MPEG stream is refused by the incremental door", status != 0);
		if (stream != NULL) {
			AudioFileStreamClose(stream);
		}
	}

	/* And the file door refuses bytes that only look like a sync word: a real MPEG-1 Layer III header followed by
	 * zeroes is a walker's worst case, because the first four bytes pass every check. */
	{
		AudioFileID opened = NULL;

		status = AudioFileOpenWithCallbacks(&fake, fake_read, NULL, fake_size, NULL, 0, &opened);
		fn_check("a sync word with no frames behind it is refused",
		         status == kAudioFileUnsupportedFileTypeError && opened == NULL);
		if (status == 0) {
			AudioFileClose(opened);
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	int skipped = 0;

	if (argc < 3) {
		printf("skip  no fixtures (an MP3 is an encoder's output, and this configuration had no ffmpeg)\n");
		return 77;
	}

	skipped += (fn_stream(argv[1], 44100.0, 2, 1152, 440.0, 3.0) == 77) ? 1 : 0;
	skipped += (fn_stream(argv[2], 22050.0, 1, 576, 330.0, 2.0) == 77) ? 1 : 0;

	if (skipped == 2) {
		printf("skip  no fixtures\n");
		return 77;
	}

	if (skipped == 0) {
		fn_rate_change(argv[1], 44100.0, 440.0);
		fn_seek(argv[1], 44100.0, 2, 440.0, 3.0);
		fn_refusals(argv[1]);
	}

	printf("\n%d check(s), %d failure(s)\n", fn_total, fn_failures);
	return fn_failures == 0 ? 0 : 1;
}
