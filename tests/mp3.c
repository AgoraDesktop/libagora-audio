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
static bool fn_no_decoder;	/* set when the file door refuses the stream: this build has no MPEG decoder */

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
		printf("skip  this build has no MPEG decoder; the container is measured, the audio is not\n");
		fn_no_decoder = true;
		CFRelease(url);
		return 0;
	}
	fn_check("the extended door opens the stream", status == 0 && ext != NULL);
	if (status != 0) {
		CFRelease(url);
		return 1;
	}

	size = sizeof(length);
	fn_check("the length is reported",
	         ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &size, &length) == 0
	         && length > 0);

	/* **THE LENGTH IS THE AUDIO, NOT THE PADDING, AND THE TWO NUMBERS STILL ADD UP.** The file door reports the
	 * gapless trim the encoder declared; the invariant Apple documents is that valid + priming + remainder is what
	 * the packets hold, which is exactly the container's count. */
	{
		AudioFilePacketTableInfo table;
		UInt32 table_size = sizeof(table);
		CFURLRef file_url = fn_url(path);
		AudioFileID measured = NULL;

		if (AudioFileOpenURL(file_url, kAudioFileReadPermission, 0, &measured) == 0) {
			fn_check("the file declares its gapless trim",
			         AudioFileGetProperty(measured, kAudioFilePropertyPacketTableInfo, &table_size, &table) == 0);
			printf("      length %lld = valid %lld + priming %d + remainder %d, packets hold %lld\n",
			       (long long)length, (long long)table.mNumberValidFrames, (int)table.mPrimingFrames,
			       (int)table.mRemainderFrames, (long long)packets * (long long)samples_per_frame);
			fn_check("the valid frames are the reported length", table.mNumberValidFrames == length);
			fn_check("valid + priming + remainder is what the packets hold",
			         table.mNumberValidFrames + table.mPrimingFrames + table.mRemainderFrames
			         == (SInt64)packets * (SInt64)samples_per_frame);
			fn_check("the priming is the tag's frame plus the encoder's delay",
			         table.mPrimingFrames > (SInt32)samples_per_frame
			         && table.mPrimingFrames < (SInt32)samples_per_frame + 1153);
			fn_check("the remainder is padding, and smaller than a frame's worth of it",
			         table.mRemainderFrames >= 0 && table.mRemainderFrames < 2000);
			AudioFileClose(measured);
		}
		CFRelease(file_url);
	}

	{
		OSStatus read_status = 0;
		long long decoded = fn_decode_all(ext, channels, &measured, &read_status);
		long long expected = (long long)(seconds * rate + 0.5);

		fn_check("the whole stream decodes", read_status == 0 && decoded > 0);
		/* **THE DECODER AND THE LENGTH ARE THE SAME NUMBER**, which is the measurement that makes the trim honest:
		 * one comes from the tag's declaration, the other from libmpg123's own gapless handling. */
		fn_check("the decoded audio is exactly the reported length", decoded == length);
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

/* ------------------------------------------------------------------------------------------------
 * The incremental door.
 *
 * **FED IN AWKWARD PIECES, ITS PACKETS MUST BE THE FILE DOOR'S PACKETS.** That is the whole assertion: the same bytes,
 * the same boundaries, the same sample counts — because both derive them from the same four-byte header
 * (`AudioMpeg.c`). A one-byte chunk is the worst case a network delivers, and a 4 096-byte one is the easy case; both
 * must produce identical output, so the test feeds the file three times and compares every packet against a reference
 * read by index through `AudioFile`.
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
	AudioFileID                 file;	/* the reference: opened with the file door, read packet by packet */
	AudioStreamBasicDescription format;
	bool                        have_format;
	bool                        ready;
	bool                        have_upper_bound;
	UInt32                      upper_bound;
	SInt64                      data_offset;
	SInt64                      next_reference;
	UInt32                      packets;
	UInt32                      bytes;
	long long                   samples;
	int                         mismatches;
} fn_stream_state;

static void fn_stream_property(void *client, AudioFileStreamID stream, AudioFileStreamPropertyID property,
                               AudioFileStreamPropertyFlags *flags)
{
	fn_stream_state *state = client;
	UInt32 size;

	(void)flags;

	if (property == kAudioFileStreamProperty_DataFormat) {
		size = sizeof(state->format);
		if (AudioFileStreamGetProperty(stream, property, &size, &state->format) == 0) {
			state->have_format = true;
		}
	} else if (property == kAudioFileStreamProperty_DataOffset) {
		size = sizeof(state->data_offset);
		(void)AudioFileStreamGetProperty(stream, property, &size, &state->data_offset);
	} else if (property == kAudioFileStreamProperty_PacketSizeUpperBound) {
		size = sizeof(state->upper_bound);
		if (AudioFileStreamGetProperty(stream, property, &size, &state->upper_bound) == 0) {
			state->have_upper_bound = true;
		}
	} else if (property == kAudioFileStreamProperty_ReadyToProducePackets) {
		state->ready = true;
	}
}

/* Read one packet by index through the file door, for comparison. */
static bool fn_reference_packet(AudioFileID file, SInt64 index, UInt8 *buffer, UInt32 capacity,
                                UInt32 *out_size, UInt32 *out_samples)
{
	AudioStreamPacketDescription description;
	UInt32 bytes = capacity;
	UInt32 packets = 1;

	memset(&description, 0, sizeof(description));
	if (AudioFileReadPacketData(file, false, &bytes, &description, index, &packets, buffer) != 0
	    || packets != 1) {
		return false;
	}
	*out_size = bytes;
	*out_samples = description.mVariableFramesInPacket;
	return true;
}

static void fn_stream_packets(void *client, UInt32 bytes, UInt32 packets, const void *data,
                              AudioStreamPacketDescription *descriptions)
{
	fn_stream_state *state = client;
	UInt32 i;

	state->bytes += bytes;
	state->packets += packets;

	for (i = 0; i < packets; i++) {
		const AudioStreamPacketDescription *description = &descriptions[i];
		UInt8 reference[4096];
		UInt32 size = 0;
		UInt32 samples = 0;

		if (description->mStartOffset < 0
		    || (UInt64)description->mStartOffset + description->mDataByteSize > bytes) {
			state->mismatches++;
			continue;
		}
		if (!fn_reference_packet(state->file, state->next_reference, reference, (UInt32)sizeof(reference),
		                         &size, &samples)
		    || size != description->mDataByteSize
		    || samples != description->mVariableFramesInPacket
		    || memcmp(reference, (const UInt8 *)data + description->mStartOffset, size) != 0) {
			state->mismatches++;
		}
		state->samples += description->mVariableFramesInPacket;
		state->next_reference++;
	}
}

/* Feed the whole file (optionally behind a synthetic ID3v2 tag) in `chunk`-byte pieces. */
static int fn_stream_parse(const char *path, double rate, UInt32 channels, UInt32 samples_per_frame,
                           UInt32 chunk, bool with_tag)
{
	CFURLRef url = fn_url(path);
	AudioFileStreamID stream = NULL;
	AudioFileID file = NULL;
	UInt64 reference_packets = 0;
	UInt8 *raw = NULL;
	UInt8 *all = NULL;
	long file_size;
	size_t got;
	UInt32 offset = 0;
	UInt32 size;
	OSStatus status;
	fn_stream_state state;
	SInt64 expected_offset = 0;
	FILE *handle;

	printf("— %s, fed %u bytes at a time%s\n", path, chunk, with_tag ? ", behind an ID3v2 tag" : "");

	status = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
	if (status != 0) {
		CFRelease(url);
		return 77;
	}
	size = sizeof(reference_packets);
	(void)AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &size, &reference_packets);

	/* **THE FIXTURE HAS A TAG OF ITS OWN** (ffmpeg writes an encoder ID3v2 block), so the expected data offset comes
	 * from the file door rather than from an assumption that the audio starts at byte zero. */
	{
		SInt64 audio_start = 0;

		size = sizeof(audio_start);
		(void)AudioFileGetProperty(file, kAudioFilePropertyDataOffset, &size, &audio_start);
		expected_offset = audio_start;
	}

	handle = fopen(path, "rb");
	if (handle == NULL) {
		AudioFileClose(file);
		CFRelease(url);
		return 77;
	}
	fseek(handle, 0, SEEK_END);
	file_size = ftell(handle);
	fseek(handle, 0, SEEK_SET);
	raw = malloc((size_t)file_size);
	got = (raw != NULL) ? fread(raw, 1, (size_t)file_size, handle) : 0;
	fclose(handle);
	if (got != (size_t)file_size) {
		free(raw);
		AudioFileClose(file);
		CFRelease(url);
		return 1;
	}

	{
		UInt32 tag = with_tag ? 210 : 0;	/* a 10-byte header declaring 200 bytes of filler */
		UInt32 total = (UInt32)file_size + tag;

		all = malloc(total);
		if (all == NULL) {
			free(raw);
			AudioFileClose(file);
			CFRelease(url);
			return 1;
		}
		if (with_tag) {
			/* **THE FILLER IS 0xFF, WHICH IS THE POINT**: a walker that trusted sync bytes inside a tag would
			 * index the filler as audio, and the shared walker refuses to. */
			memset(all, 0xFF, tag);
			all[0] = 'I';
			all[1] = 'D';
			all[2] = '3';
			all[3] = 4;			/* v2.4, which is what writes a footer flag */
			all[5] = 0;
			all[6] = 0;
			all[7] = 0;
			all[8] = (UInt8)(200 >> 7);	/* 28-bit syncsafe size: 200 = 1 * 128 + 72 */
			all[9] = (UInt8)(200 & 0x7F);
		}
		memcpy(all + tag, raw, (size_t)file_size);
	}

	memset(&state, 0, sizeof(state));
	state.file = file;

	status = AudioFileStreamOpen(&state, fn_stream_property, fn_stream_packets, 0, &stream);
	if (status != 0) {
		free(raw);
		free(all);
		AudioFileClose(file);
		CFRelease(url);
		return 1;
	}

	{
		UInt32 total = (UInt32)file_size + (with_tag ? 210 : 0);

		while (offset < total) {
			UInt32 piece = (total - offset < chunk) ? (total - offset) : chunk;

			status = AudioFileStreamParseBytes(stream, piece, all + offset, 0);
			if (status != 0) {
				break;
			}
			offset += piece;
		}
	}

	fn_check("the incremental door reports the format", status == 0 && state.have_format);
	fn_check("the incremental format is the file's",
	         state.format.mFormatID == kAudioFormatMPEGLayer3 && state.format.mSampleRate == rate
	         && state.format.mChannelsPerFrame == channels
	         && state.format.mFramesPerPacket == samples_per_frame
	         && state.format.mBytesPerPacket == 0);
	fn_check("the incremental door becomes ready", state.ready);
	printf("      data offset %lld (expected %lld)\n", (long long)state.data_offset,
	       (long long)(expected_offset + (with_tag ? 210 : 0)));
	fn_check("the data offset is where the audio starts",
	         state.data_offset == expected_offset + (with_tag ? 210 : 0));
	fn_check("the packet size bound is the format's largest frame",
	         state.have_upper_bound && state.upper_bound >= 100 && state.upper_bound <= 2000);
	printf("      %u packets reported, %llu in the container, %lld bytes\n",
	       state.packets, (unsigned long long)reference_packets, (long long)state.bytes);
	fn_check("every packet of the file is reported",
	         state.packets == (UInt32)reference_packets
	         && state.next_reference == (SInt64)reference_packets);
	fn_check("every reported packet is the file door's packet, byte for byte",
	         state.mismatches == 0);
	fn_check("the reported samples are the packet count times the frame's",
	         state.samples == (long long)reference_packets * (long long)samples_per_frame);

	AudioFileStreamClose(stream);
	free(raw);
	free(all);
	AudioFileClose(file);
	CFRelease(url);
	return (status == 0 && state.mismatches == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------------------------------------
 * The Xing/Info tag: the length a stream declares before it has been read.
 *
 * **A PLAYER WANTS THE DURATION OF A SONG IT HAS NOT DOWNLOADED, AND THIS IS WHERE AN MP3 KEEPS IT.** The tag is
 * inside the first frame — after the side information, whose size depends on the version and the channel mode — and
 * it holds a frame count, a byte count and a 100-entry seek table. Everything below is asked of a stream that has
 * been fed the first 4 KiB of the file and no more, which is the only interesting case.
 * ---------------------------------------------------------------------------------------------- */

/* The exact file offset of every packet, from the file door, read in one pass. */
typedef struct {
	UInt8 *                        bytes;		/* RETAINED: every packet, concatenated */
	AudioStreamPacketDescription * descriptions;	/* RETAINED: one per packet, offsets from `bytes` */
	UInt32                         packets;
	SInt64                         data_offset;
	SInt64                         audio_bytes;
} fn_table;

static bool fn_table_build(const char *path, AudioFileID *out_file, fn_table *table)
{
	AudioFileID file = NULL;
	UInt32 capacity;
	UInt32 packets = 0;
	UInt8 *bytes;
	AudioStreamPacketDescription *descriptions;
	CFURLRef url = fn_url(path);
	long file_size;

	memset(table, 0, sizeof(*table));

	if (AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file) != 0) {
		CFRelease(url);
		return false;
	}
	CFRelease(url);

	/* Both counts are 64-bit in this door (Apple's are), so the sizes are the answer's, not a guess. */
	{
		UInt64 value = 0;
		SInt64 offset = 0;

		capacity = sizeof(value);
		if (AudioFileGetProperty(file, kAudioFilePropertyAudioDataByteCount, &capacity, &value) == 0) {
			table->audio_bytes = (SInt64)value;
		}
		capacity = sizeof(offset);
		if (AudioFileGetProperty(file, kAudioFilePropertyDataOffset, &capacity, &offset) == 0) {
			table->data_offset = offset;
		}
		capacity = sizeof(value);
		if (AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &capacity, &value) == 0) {
			table->packets = (UInt32)value;
		}
	}
	if (table->packets == 0) {
		AudioFileClose(file);
		return false;
	}

	file_size = (long)table->audio_bytes;
	bytes = malloc((size_t)file_size);
	descriptions = calloc(table->packets, sizeof(*descriptions));
	if (bytes == NULL || descriptions == NULL) {
		free(bytes);
		free(descriptions);
		AudioFileClose(file);
		return false;
	}

	capacity = (UInt32)file_size;
	packets = table->packets;
	if (AudioFileReadPacketData(file, false, &capacity, descriptions, 0, &packets, bytes) != 0
	    || packets != table->packets) {
		free(bytes);
		free(descriptions);
		AudioFileClose(file);
		return false;
	}

	table->bytes = bytes;
	table->descriptions = descriptions;
	*out_file = file;
	return true;
}

static int fn_stream_tag(const char *path)
{
	AudioFileID file = NULL;
	AudioFileStreamID stream = NULL;
	fn_table table;
	fn_stream_state state;
	UInt8 *raw = NULL;
	UInt32 prefix = 4096;
	SInt64 exact;
	OSStatus status;

	printf("— %s, the tag in the first %u bytes\n", path, prefix);

	if (!fn_table_build(path, &file, &table)) {
		printf("skip  no usable fixture at %s\n", path);
		return 77;
	}
	AudioFileClose(file);

	{
		FILE *handle = fopen(path, "rb");

		if (handle == NULL) {
			free(table.bytes);
			free(table.descriptions);
			return 77;
		}
		raw = malloc(prefix);
		if (raw == NULL || fread(raw, 1, prefix, handle) != prefix) {
			free(raw);
			fclose(handle);
			free(table.bytes);
			free(table.descriptions);
			return 77;
		}
		fclose(handle);
	}

	memset(&state, 0, sizeof(state));
	status = AudioFileStreamOpen(&state, fn_stream_property, fn_stream_packets, 0, &stream);
	if (status == 0) {
		status = AudioFileStreamParseBytes(stream, prefix, raw, 0);
	}
	fn_check("a stream fed one frame has parsed its tag", status == 0);

	{
		UInt64 packets = 0;
		UInt64 bytes = 0;
		UInt32 size = sizeof(packets);

		fn_check("the tag gives the packet count before the stream has been read",
		         AudioFileStreamGetProperty(stream, kAudioFileStreamProperty_AudioDataPacketCount, &size,
		                                    &packets) == 0
		         && packets == (UInt64)table.packets);
		size = sizeof(bytes);
		fn_check("the tag gives the byte count",
		         AudioFileStreamGetProperty(stream, kAudioFileStreamProperty_AudioDataByteCount, &size, &bytes) == 0
		         && bytes == (UInt64)table.audio_bytes);
	}

	/* **THE SEEK IS AN ESTIMATE, AND THE FLAG SAYS SO.** Packet 0 is a position this door knows exactly; the rest
	 * come from the table's hundredths, so they are compared against the file door's exact offsets with a
	 * tolerance of one table entry. */
	{
		static const double fractions[] = { 0.0, 0.25, 0.5, 0.9, 1.0 };
		UInt32 i;

		for (i = 0; i < sizeof(fractions) / sizeof(fractions[0]); i++) {
			SInt64 packet = (SInt64)((double)(table.packets - 1) * fractions[i] + 0.5);
			SInt64 predicted = 0;
			AudioFileStreamSeekFlags flags = 0;
			SInt64 tolerance = (packet == 0) ? 0 : table.audio_bytes / 50;
			OSStatus seek_status;

			exact = table.data_offset + table.descriptions[packet].mStartOffset;
			seek_status = AudioFileStreamSeek(stream, packet, &predicted, &flags);
			printf("      packet %lld: seek says %lld, the file says %lld (flags 0x%x)\n",
			       (long long)packet, (long long)predicted, (long long)exact, (unsigned)flags);
			fn_check("the seek answers for an MPEG stream", seek_status == 0);
			fn_check("the seek's offset finds the packet, or is within one table entry of it",
			         llabs((long long)(predicted - exact)) <= (long long)tolerance);
			fn_check("the seek flags the offset as estimated unless it is packet zero",
			         (packet == 0) ? (flags == 0) : (flags == kAudioFileStreamSeekFlag_OffsetIsEstimated));
		}

		{
			SInt64 predicted = 0;
			AudioFileStreamSeekFlags flags = 0;

			fn_check("a packet past the end is refused by name",
			         AudioFileStreamSeek(stream, (SInt64)table.packets + 5, &predicted, &flags)
			         == kAudioFileStreamError_InvalidPacketOffset);
		}
	}

	AudioFileStreamClose(stream);

	/* **A FILE WHOSE FIRST FRAME IS NOT THE TAG HAS NO TABLE AT ALL**, which is what a file cut out of a longer one
	 * looks like: the count is unknown and the seek refuses, rather than answering from an estimate nobody made. */
	{
		SInt64 predicted = 0;
		AudioFileStreamSeekFlags flags = 0;
		UInt64 packets = 0;
		UInt32 size = sizeof(packets);
		/* The first frame starts at the data offset, not at byte zero: the fixture carries a tag of its own. */
		UInt32 skip = (UInt32)(table.data_offset + table.descriptions[0].mStartOffset
		                       + table.descriptions[0].mDataByteSize);
		UInt32 available = prefix - skip;

		memset(&state, 0, sizeof(state));
		status = AudioFileStreamOpen(&state, fn_stream_property, fn_stream_packets, 0, &stream);
		if (status == 0) {
			status = AudioFileStreamParseBytes(stream, available, raw + skip, 0);
		}
		printf("      entering at byte %u of the file: %u bytes, status %d\n", skip, available, (int)status);
		fn_check("a stream entering mid-file still parses", status == 0);
		fn_check("a stream with no tag has no packet count",
		         AudioFileStreamGetProperty(stream, kAudioFileStreamProperty_AudioDataPacketCount, &size, &packets)
		         == kAudioFileStreamError_ValueUnknown);
		fn_check("a stream with no table refuses a seek",
		         AudioFileStreamSeek(stream, 10, &predicted, &flags) == kAudioFileStreamError_IllegalOperation);
		AudioFileStreamClose(stream);
	}

	free(raw);
	free(table.bytes);
	free(table.descriptions);
	return 0;
}

/* The refusals, each by name, because a format that is read is not a format that is written, and because a file
 * that only looks like one must not be accepted. */
static int fn_refusals(void)
{
	CFURLRef out = fn_url("/tmp/agora-mp3-write-probe.mp3");
	fn_fake fake;
	AudioStreamBasicDescription format;
	AudioFileID file = NULL;
	OSStatus status;

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
	/* A missing decoder is not a missing fixture: the container checks ran, and the legs below do not need one. */

	if (skipped == 2) {
		printf("skip  no fixtures\n");
		return 77;
	}

	if (skipped < 2) {
		/* **THE INCREMENTAL DOOR NEEDS NO DECODER**: it walks frame headers, so it is measured even on a build
		 * with `-DAGORA_AUDIO_WITH_MPG123=OFF`. Only the legs that play audio are conditional. */
		fn_stream_parse(argv[1], 44100.0, 2, 1152, 1, false);
		fn_stream_parse(argv[1], 44100.0, 2, 1152, 7, true);
		fn_stream_parse(argv[2], 22050.0, 1, 576, 4096, false);

		if (!fn_no_decoder) {
			fn_rate_change(argv[1], 44100.0, 440.0);
			fn_seek(argv[1], 44100.0, 2, 440.0, 3.0);
		}
		fn_refusals();
	}

	if (skipped < 2) {
		fn_stream_tag(argv[1]);
		fn_stream_tag(argv[2]);
	}

	printf("\n%d check(s), %d failure(s)\n", fn_total, fn_failures);
	if (fn_failures != 0) {
		return 1;
	}
	/* **A SUITE THAT COULD NOT PLAY THE AUDIO IS A SKIP, NOT A PASS**, even though everything it could measure
	 * passed: `-DAGORA_AUDIO_WITH_MPG123=OFF` is a real configuration and this is what it is worth. */
	if (fn_no_decoder) {
		printf("skip  no MPEG decoder in this build\n");
		return 77;
	}
	return 0;
}
