/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioConverter.c — format conversion between two linear PCM descriptions.
 *
 * THE PIPELINE IS THREE STEPS AND TWO OF THEM ARE THE SAME STEP: every input frame is turned into
 * interleaved float32, resampled if the rates differ, and turned back into the output format. Doing it in
 * that order is what makes the four conversions independent of each other — a 24-bit big-endian
 * non-interleaved 44.1 kHz source into a 32-bit float interleaved 48 kHz sink is one code path, not the
 * sixteen pairs a per-format matrix would need.
 *
 * THE COST IS A COPY, AND IT IS DELIBERATE: a converter that specialises each pair is faster and is also
 * where the bugs live, because the pairs nobody exercises rot. The suite exercises the pairs that matter
 * (format × interleaving × rate) and the copy is a memcpy-shaped walk over a few thousand floats.
 *
 * THE TWO CALLING MODELS DIFFER IN ONE RESPECT, AND IT IS THE WHOLE REASON BOTH EXIST: the buffered model
 * owns the resampler's history between calls, so it can change the sample rate; the one-shot model has
 * nowhere to keep that state and refuses a rate change with `kAudioConverterErr_FormatNotSupported`, which
 * is what Apple's own does.
 */

#include <AudioToolbox/AudioConverter.h>
#include "AudioToolbox_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct OpaqueAudioConverter {
	AudioStreamBasicDescription in;
	AudioStreamBasicDescription out;

	/* Derived once, because they are asked for on every frame otherwise. */
	UInt32 in_bytes_per_sample;
	UInt32 out_bytes_per_sample;

	/* The resampler exists only when the rates differ. */
	bool              needs_rate_change;
	agora_resampler   resampler;

	/* Scratch: the input of one pull, in interleaved float32, and the planar buffers handed to an input
	 * callback for a non-interleaved format. */
	float *           scratch;
	UInt32            scratch_frames;
	UInt32            scratch_capacity;
	uint8_t *         planar;
	UInt32            planar_capacity;

	/* Settings the caller can set and we honour, or honestly store and ignore. */
	AudioConverterPrimeMethod prime_method;
	AudioConverterPrimeInfo   prime_info;
	UInt32                    src_quality;
	UInt32                    src_complexity;
	double                    src_initial_phase;
	SInt32 *                  channel_map;   /* out_channels entries, or NULL for identity */
	UInt32                    channel_map_length;
};

/* ------------------------------------------------------------------------------------------------
 * Frames in and out of an AudioBufferList.
 * ---------------------------------------------------------------------------------------------- */

static void fn_ensure_scratch(AudioConverterRef converter, UInt32 frames)
{
	UInt32 channels = converter->in.mChannelsPerFrame;

	if (frames <= converter->scratch_capacity) {
		return;
	}

	converter->scratch = realloc(converter->scratch, (size_t)frames * channels * sizeof(float));
	converter->scratch_capacity = frames;
}

/* Interleaved float32 out of whatever the input callback handed over. */
static void fn_read_buffer_list(AudioConverterRef converter, const AudioBufferList *list,
                                UInt32 frames, float *out)
{
	const AudioStreamBasicDescription *asbd = &converter->in;
	UInt32 channels = asbd->mChannelsPerFrame;
	UInt32 bytes_per_sample = converter->in_bytes_per_sample;
	UInt32 frame;

	if (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
		/* One buffer per channel; a frame is one sample in each. */
		UInt32 channel;

		for (frame = 0; frame < frames; frame++) {
			for (channel = 0; channel < channels && channel < list->mNumberBuffers; channel++) {
				const uint8_t *src = (const uint8_t *)list->mBuffers[channel].mData
				                   + (size_t)frame * bytes_per_sample;
				float single[1];

				fn_pcm_frame_to_float(asbd, src, single);
				out[(size_t)frame * channels + channel] = single[0];
			}
		}
		return;
	}

	for (frame = 0; frame < frames; frame++) {
		const uint8_t *src = (const uint8_t *)list->mBuffers[0].mData
		                   + (size_t)frame * (size_t)bytes_per_sample * channels;

		fn_pcm_frame_to_float(asbd, src, out + (size_t)frame * channels);
	}
}

/* Interleaved float32 into the caller's output buffers. */
static void fn_write_buffer_list(AudioConverterRef converter, AudioBufferList *list,
                                 const float *in, UInt32 frames)
{
	const AudioStreamBasicDescription *asbd = &converter->out;
	UInt32 channels = asbd->mChannelsPerFrame;
	UInt32 bytes_per_sample = converter->out_bytes_per_sample;
	UInt32 frame;

	if (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) {
		UInt32 channel;

		for (frame = 0; frame < frames; frame++) {
			for (channel = 0; channel < channels && channel < list->mNumberBuffers; channel++) {
				uint8_t *dst = (uint8_t *)list->mBuffers[channel].mData
				             + (size_t)frame * bytes_per_sample;

				fn_pcm_frame_from_float(asbd, in + (size_t)frame * channels + channel, dst);
			}
		}
		return;
	}

	/* A CHANNEL MAP THAT IS NOT THE IDENTITY IS APPLIED HERE, on the way out: the map says which input
	 * channel feeds each output channel, and -1 is silence. */
	if (converter->channel_map != NULL) {
		float *mapped = malloc((size_t)channels * sizeof(float));

		if (mapped != NULL) {
			for (frame = 0; frame < frames; frame++) {
				UInt32 channel;

				for (channel = 0; channel < channels; channel++) {
					SInt32 source = converter->channel_map[channel];

					mapped[channel] = (source >= 0 && (UInt32)source < converter->in.mChannelsPerFrame)
					                ? in[(size_t)frame * channels + (UInt32)source] : 0.0f;
				}

				fn_pcm_frame_from_float(asbd, mapped,
				                        (uint8_t *)list->mBuffers[0].mData
				                        + (size_t)frame * (size_t)bytes_per_sample * channels);
			}

			free(mapped);
			return;
		}
	}

	/* The frame walk is the front-to-back copy; `mDataByteSize` is left to the caller to read as it sees
	 * fit, because Apple's contract has the converter report frames, not bytes. */
	for (frame = 0; frame < frames; frame++) {
		uint8_t *dst = (uint8_t *)list->mBuffers[0].mData
		             + (size_t)frame * (size_t)bytes_per_sample * channels;

		fn_pcm_frame_from_float(asbd, in + (size_t)frame * channels, dst);
	}
}

/* ------------------------------------------------------------------------------------------------
 * Creation and disposal.
 * ---------------------------------------------------------------------------------------------- */

static OSStatus fn_validate_pair(const AudioStreamBasicDescription *in,
                                 const AudioStreamBasicDescription *out)
{
	OSStatus status;

	if (!fn_pcm_is_supported(in, &status)) {
		return status;
	}

	if (!fn_pcm_is_supported(out, &status)) {
		return status;
	}

	/* THE CHANNEL COUNT IS PROPERTY OF THE MAP, NOT OF THE CONVERTER: Apple allows a stereo→mono conversion
	 * when a channel map says so, and refuses it otherwise. Here the same rule applies, with the map
	 * available as `kAudioConverterChannelMap` after creation. */
	if (in->mChannelsPerFrame != out->mChannelsPerFrame) {
		return kAudioConverterErr_FormatNotSupported;
	}

	return 0;
}

OSStatus AudioConverterNew(const AudioStreamBasicDescription *inSourceFormat,
                           const AudioStreamBasicDescription *inDestinationFormat,
                           AudioConverterRef *outAudioConverter)
{
	AudioConverterRef converter;
	OSStatus status;

	if (inSourceFormat == NULL || inDestinationFormat == NULL || outAudioConverter == NULL) {
		return kAudio_ParamError;
	}

	status = fn_validate_pair(inSourceFormat, inDestinationFormat);
	if (status != 0) {
		return status;
	}

	converter = calloc(1, sizeof(*converter));
	if (converter == NULL) {
		return kAudio_MemFullError;
	}

	converter->in = *inSourceFormat;
	converter->out = *inDestinationFormat;
	if (converter->in.mBytesPerFrame == 0) {
		converter->in.mBytesPerFrame = fn_pcm_bytes_per_frame(&converter->in);
	}
	if (converter->out.mBytesPerFrame == 0) {
		converter->out.mBytesPerFrame = fn_pcm_bytes_per_frame(&converter->out);
	}
	converter->in_bytes_per_sample = fn_pcm_bytes_per_sample(&converter->in);
	converter->out_bytes_per_sample = fn_pcm_bytes_per_sample(&converter->out);

	converter->needs_rate_change = converter->in.mSampleRate != converter->out.mSampleRate;
	converter->prime_method = kConverterPrimeMethod_Normal;
	converter->src_quality = kAudioConverterQuality_Medium;
	converter->src_complexity = kAudioConverterSampleRateConverterComplexity_Normal;

	if (converter->needs_rate_change) {
		double ratio = converter->out.mSampleRate / converter->in.mSampleRate;

		if (!fn_resampler_init(&converter->resampler, converter->in.mChannelsPerFrame, ratio, false,
		                       converter->src_quality)) {
			free(converter);
			return kAudio_MemFullError;
		}
	}

	*outAudioConverter = converter;
	return 0;
}

OSStatus AudioConverterNewSpecific(const AudioStreamBasicDescription *inSourceFormat,
                                   const AudioStreamBasicDescription *inDestinationFormat,
                                   UInt32 inNumberClassDescriptions,
                                   const AudioClassDescription *inClassDescriptions,
                                   AudioConverterRef *outAudioConverter)
{
	/* The class descriptions name codecs. There are none beyond linear PCM here, so a caller that names
	 * one is asking for something this component cannot do — and saying so is the answer, rather than
	 * ignoring the parameter and hoping. */
	if (inNumberClassDescriptions > 0 && inClassDescriptions != NULL) {
		UInt32 i;

		for (i = 0; i < inNumberClassDescriptions; i++) {
			if (inClassDescriptions[i].mSubType != kAudioFormatLinearPCM) {
				return kAudioConverterErr_FormatNotSupported;
			}
		}
	}

	return AudioConverterNew(inSourceFormat, inDestinationFormat, outAudioConverter);
}

OSStatus AudioConverterDispose(AudioConverterRef inAudioConverter)
{
	if (inAudioConverter == NULL) {
		return kAudio_ParamError;
	}

	fn_resampler_free(&inAudioConverter->resampler);
	free(inAudioConverter->scratch);
	free(inAudioConverter->planar);
	free(inAudioConverter->channel_map);
	free(inAudioConverter);

	return 0;
}

OSStatus AudioConverterReset(AudioConverterRef inAudioConverter)
{
	if (inAudioConverter == NULL) {
		return kAudio_ParamError;
	}

	if (inAudioConverter->needs_rate_change) {
		inAudioConverter->resampler.linear =
		    inAudioConverter->src_complexity == kAudioConverterSampleRateConverterComplexity_Linear;
		fn_resampler_reset(&inAudioConverter->resampler);
	}

	return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Properties.
 * ---------------------------------------------------------------------------------------------- */

OSStatus AudioConverterGetPropertyInfo(AudioConverterRef inAudioConverter,
                                       AudioConverterPropertyID inPropertyID,
                                       UInt32 *outSize,
                                       Boolean *outWritable)
{
	if (inAudioConverter == NULL) {
		return kAudio_ParamError;
	}

	if (outWritable != NULL) {
		*outWritable = false;
	}

	switch (inPropertyID) {
	case kAudioConverterCurrentInputStreamDescription:
		if (outSize) { *outSize = sizeof(AudioStreamBasicDescription); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterCurrentOutputStreamDescription:
		if (outSize) { *outSize = sizeof(AudioStreamBasicDescription); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterPrimeMethod:
		if (outSize) { *outSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterPrimeInfo:
		if (outSize) { *outSize = sizeof(AudioConverterPrimeInfo); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterChannelMap:
		if (outSize) { *outSize = inAudioConverter->out.mChannelsPerFrame * (UInt32)sizeof(SInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterSampleRateConverterQuality:
		if (outSize) { *outSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterSampleRateConverterComplexity:
		if (outSize) { *outSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterSampleRateConverterInitialPhase:
		if (outSize) { *outSize = sizeof(Float64); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterPropertyCalculateInputBufferSize:
	case kAudioConverterPropertyCalculateOutputBufferSize:
		if (outSize) { *outSize = sizeof(UInt32); }
		if (outWritable) { *outWritable = true; }
		return 0;

	case kAudioConverterPropertyMinimumInputBufferSize:
	case kAudioConverterPropertyMinimumOutputBufferSize:
	case kAudioConverterPropertyMaximumInputBufferSize:
	case kAudioConverterPropertyMaximumOutputPacketSize:
		if (outSize) { *outSize = sizeof(UInt32); }
		return 0;

	default:
		return kAudioConverterErr_PropertyNotSupported;
	}
}

static OSStatus fn_converter_get_scalar(AudioConverterRef converter, AudioConverterPropertyID id,
                                        UInt32 *size, void *data)
{
	UInt32 value;

	if (size == NULL || data == NULL) {
		return kAudio_ParamError;
	}
	if (*size < sizeof(UInt32)) {
		return kAudioConverterErr_BadPropertySizeError;
	}

	switch (id) {
	case kAudioConverterPrimeMethod:
		value = (UInt32)converter->prime_method;
		break;
	case kAudioConverterSampleRateConverterQuality:
		value = converter->src_quality;
		break;
	case kAudioConverterSampleRateConverterComplexity:
		value = converter->src_complexity;
		break;
	case kAudioConverterPropertyMinimumInputBufferSize:
	case kAudioConverterPropertyMinimumOutputBufferSize:
		value = fn_pcm_bytes_per_frame(&converter->in);
		break;
	case kAudioConverterPropertyMaximumInputBufferSize:
	case kAudioConverterPropertyMaximumOutputPacketSize:
		value = fn_pcm_bytes_per_frame(&converter->out);
		break;
	default:
		return kAudioConverterErr_PropertyNotSupported;
	}

	memcpy(data, &value, sizeof(value));
	*size = sizeof(value);
	return 0;
}

OSStatus AudioConverterGetProperty(AudioConverterRef inAudioConverter,
                                   AudioConverterPropertyID inPropertyID,
                                   UInt32 *ioPropertyDataSize,
                                   void *outPropertyData)
{
	if (inAudioConverter == NULL) {
		return kAudio_ParamError;
	}

	switch (inPropertyID) {
	case kAudioConverterCurrentInputStreamDescription:
		if (ioPropertyDataSize == NULL || outPropertyData == NULL
		    || *ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		memcpy(outPropertyData, &inAudioConverter->in, sizeof(AudioStreamBasicDescription));
		*ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioConverterCurrentOutputStreamDescription:
		if (ioPropertyDataSize == NULL || outPropertyData == NULL
		    || *ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		memcpy(outPropertyData, &inAudioConverter->out, sizeof(AudioStreamBasicDescription));
		*ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
		return 0;

	case kAudioConverterPrimeInfo:
		if (ioPropertyDataSize == NULL || outPropertyData == NULL
		    || *ioPropertyDataSize < sizeof(AudioConverterPrimeInfo)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		memcpy(outPropertyData, &inAudioConverter->prime_info, sizeof(AudioConverterPrimeInfo));
		*ioPropertyDataSize = sizeof(AudioConverterPrimeInfo);
		return 0;

	case kAudioConverterChannelMap:
		if (ioPropertyDataSize == NULL || outPropertyData == NULL) {
			return kAudio_ParamError;
		}
		if (*ioPropertyDataSize < inAudioConverter->out.mChannelsPerFrame * sizeof(SInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		if (inAudioConverter->channel_map != NULL) {
			memcpy(outPropertyData, inAudioConverter->channel_map,
			       inAudioConverter->out.mChannelsPerFrame * sizeof(SInt32));
		} else {
			UInt32 i;

			for (i = 0; i < inAudioConverter->out.mChannelsPerFrame; i++) {
				((SInt32 *)outPropertyData)[i] = (SInt32)i;
			}
		}
		*ioPropertyDataSize = inAudioConverter->out.mChannelsPerFrame * (UInt32)sizeof(SInt32);
		return 0;

	case kAudioConverterPropertyCalculateInputBufferSize:
	case kAudioConverterPropertyCalculateOutputBufferSize: {
		/* `inPropertyData` is a frame count, `outPropertyData` the byte size that frame count needs in
		 * the format on the other side — Apple's own contract, and the reason these two are the only
		 * properties whose setters and getters take different types. */
		AudioConverterRef converter = inAudioConverter;
		UInt32 frames;

		if (ioPropertyDataSize == NULL || outPropertyData == NULL) {
			return kAudio_ParamError;
		}
		if (*ioPropertyDataSize < sizeof(UInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}

		frames = *(const UInt32 *)outPropertyData;
		{
			/* The caller passes the frame count through the same buffer; the callers of this door put it
			 * in `outPropertyData` before asking, which is how Apple's is used in practice. */
			UInt32 bytes = (inPropertyID == kAudioConverterPropertyCalculateInputBufferSize)
			             ? frames * fn_pcm_bytes_per_frame(&converter->in)
			             : frames * fn_pcm_bytes_per_frame(&converter->out);

			memcpy(outPropertyData, &bytes, sizeof(bytes));
			*ioPropertyDataSize = sizeof(bytes);
		}
		return 0;
	}

	case kAudioConverterSampleRateConverterInitialPhase:
		if (ioPropertyDataSize == NULL || outPropertyData == NULL
		    || *ioPropertyDataSize < sizeof(Float64)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		*(Float64 *)outPropertyData = inAudioConverter->src_initial_phase;
		*ioPropertyDataSize = sizeof(Float64);
		return 0;

	default:
		return fn_converter_get_scalar(inAudioConverter, inPropertyID, ioPropertyDataSize,
		                               outPropertyData);
	}
}

OSStatus AudioConverterSetProperty(AudioConverterRef inAudioConverter,
                                   AudioConverterPropertyID inPropertyID,
                                   UInt32 inPropertyDataSize,
                                   const void *inPropertyData)
{
	if (inAudioConverter == NULL || inPropertyData == NULL) {
		return kAudio_ParamError;
	}

	switch (inPropertyID) {
	case kAudioConverterCurrentInputStreamDescription:
	case kAudioConverterCurrentOutputStreamDescription: {
		/* Changing a description re-derives the pipeline. Apple allows it; a converter that changed its
		 * rate this way would need its resampler rebuilt, which is what happens here. */
		const AudioStreamBasicDescription *replacement = inPropertyData;
		AudioStreamBasicDescription old;
		OSStatus status;

		if (inPropertyDataSize < sizeof(AudioStreamBasicDescription)) {
			return kAudioConverterErr_BadPropertySizeError;
		}

		old = (inPropertyID == kAudioConverterCurrentInputStreamDescription)
		    ? inAudioConverter->in : inAudioConverter->out;

		if (inPropertyID == kAudioConverterCurrentInputStreamDescription) {
			inAudioConverter->in = *replacement;
			inAudioConverter->in_bytes_per_sample = fn_pcm_bytes_per_sample(replacement);
		} else {
			inAudioConverter->out = *replacement;
			inAudioConverter->out_bytes_per_sample = fn_pcm_bytes_per_sample(replacement);
		}

		status = fn_validate_pair(&inAudioConverter->in, &inAudioConverter->out);
		if (status != 0) {
			if (inPropertyID == kAudioConverterCurrentInputStreamDescription) {
				inAudioConverter->in = old;
			} else {
				inAudioConverter->out = old;
			}
			return status;
		}

		inAudioConverter->needs_rate_change =
		    inAudioConverter->in.mSampleRate != inAudioConverter->out.mSampleRate;

		if (inAudioConverter->needs_rate_change) {
			double ratio = inAudioConverter->out.mSampleRate / inAudioConverter->in.mSampleRate;

			fn_resampler_free(&inAudioConverter->resampler);
			if (!fn_resampler_init(&inAudioConverter->resampler, inAudioConverter->in.mChannelsPerFrame,
			                       ratio,
			                       inAudioConverter->src_complexity
			                           == kAudioConverterSampleRateConverterComplexity_Linear,
			                       inAudioConverter->src_quality)) {
				return kAudio_MemFullError;
			}
		} else {
			fn_resampler_free(&inAudioConverter->resampler);
		}

		return 0;
	}

	case kAudioConverterPrimeMethod:
		if (inPropertyDataSize < sizeof(UInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		inAudioConverter->prime_method = (AudioConverterPrimeMethod)(*(const UInt32 *)inPropertyData);
		return 0;

	case kAudioConverterPrimeInfo:
		if (inPropertyDataSize < sizeof(AudioConverterPrimeInfo)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		memcpy(&inAudioConverter->prime_info, inPropertyData, sizeof(AudioConverterPrimeInfo));
		return 0;

	case kAudioConverterSampleRateConverterQuality:
		if (inPropertyDataSize < sizeof(UInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		inAudioConverter->src_quality = *(const UInt32 *)inPropertyData;
		if (inAudioConverter->src_quality > kAudioConverterQuality_Max) {
			inAudioConverter->src_quality = kAudioConverterQuality_Max;
		}
		return 0;

	case kAudioConverterSampleRateConverterComplexity: {
		UInt32 complexity;

		if (inPropertyDataSize < sizeof(UInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}

		complexity = *(const UInt32 *)inPropertyData;
		switch (complexity) {
		case kAudioConverterSampleRateConverterComplexity_Linear:
		case kAudioConverterSampleRateConverterComplexity_Normal:
		case kAudioConverterSampleRateConverterComplexity_Mastering:
		case kAudioConverterSampleRateConverterComplexity_MinimumPhase:
			inAudioConverter->src_complexity = complexity;
			if (inAudioConverter->needs_rate_change) {
				inAudioConverter->resampler.linear =
				    complexity == kAudioConverterSampleRateConverterComplexity_Linear;
			}
			return 0;
		default:
			return kAudioConverterErr_OperationNotSupported;
		}
	}

	case kAudioConverterSampleRateConverterInitialPhase:
		if (inPropertyDataSize < sizeof(Float64)) {
			return kAudioConverterErr_BadPropertySizeError;
		}
		inAudioConverter->src_initial_phase = *(const Float64 *)inPropertyData;
		return 0;

	case kAudioConverterChannelMap: {
		UInt32 channels = inAudioConverter->out.mChannelsPerFrame;

		if (inPropertyDataSize < channels * sizeof(SInt32)) {
			return kAudioConverterErr_BadPropertySizeError;
		}

		free(inAudioConverter->channel_map);
		inAudioConverter->channel_map = malloc((size_t)channels * sizeof(SInt32));
		if (inAudioConverter->channel_map == NULL) {
			return kAudio_MemFullError;
		}

		memcpy(inAudioConverter->channel_map, inPropertyData, (size_t)channels * sizeof(SInt32));
		inAudioConverter->channel_map_length = channels;

		/* A map that names an input channel that does not exist is a mistake worth refusing. */
		{
			UInt32 i;

			for (i = 0; i < channels; i++) {
				SInt32 source = inAudioConverter->channel_map[i];

				if (source >= (SInt32)inAudioConverter->in.mChannelsPerFrame) {
					free(inAudioConverter->channel_map);
					inAudioConverter->channel_map = NULL;
					return kAudio_ParamError;
				}
			}
		}

		return 0;
	}

	default:
		return kAudioConverterErr_PropertyNotSupported;
	}
}

/* ------------------------------------------------------------------------------------------------
 * Conversion.
 * ---------------------------------------------------------------------------------------------- */

/* One input pull: ask the callback for `wanted` frames and normalise them into interleaved float32.
 * Returns the frame count actually produced (0 at end of stream). */
static UInt32 fn_pull_input(AudioConverterRef converter, AudioConverterComplexInputDataProc proc,
                            void *user, UInt32 wanted, float *out)
{
	AudioBufferList list;
	AudioStreamPacketDescription *descriptions = NULL;
	UInt32 frames = wanted;
	OSStatus status;

	memset(&list, 0, sizeof(list));
	list.mNumberBuffers = (converter->in.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
	                    ? converter->in.mChannelsPerFrame : 1;

	if ((converter->in.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
		UInt32 channel;
		UInt32 needed = wanted * converter->in_bytes_per_sample * converter->in.mChannelsPerFrame;

		/* ONE REGION PER CHANNEL, so the callback may write straight into what it is handed: each
		 * channel's buffer is `wanted` samples long, and the deinterleave below reads them back. */
		if (needed > converter->planar_capacity) {
			uint8_t *grown = realloc(converter->planar, needed);

			if (grown == NULL) {
				return 0;
			}
			converter->planar = grown;
			converter->planar_capacity = needed;
		}

		for (channel = 0; channel < list.mNumberBuffers; channel++) {
			list.mBuffers[channel].mNumberChannels = 1;
			list.mBuffers[channel].mDataByteSize = wanted * converter->in_bytes_per_sample;
			list.mBuffers[channel].mData =
			    converter->planar + (size_t)channel * wanted * converter->in_bytes_per_sample;
		}
	} else {
		list.mBuffers[0].mNumberChannels = converter->in.mChannelsPerFrame;
		list.mBuffers[0].mDataByteSize = wanted * fn_pcm_bytes_per_frame(&converter->in);
		list.mBuffers[0].mData = out;
	}

	status = proc(converter, &frames, &list, &descriptions, user);
	if (status != 0 || frames == 0) {
		return 0;
	}

	fn_read_buffer_list(converter, &list, frames, out);
	return frames;
}

OSStatus AudioConverterFillComplexBuffer(AudioConverterRef inAudioConverter,
                                         AudioConverterComplexInputDataProc inInputDataProc,
                                         void *inInputDataProcUserData,
                                         UInt32 *ioOutputDataPacketSize,
                                         AudioBufferList *outOutputData,
                                         AudioStreamPacketDescription *outPacketDescription)
{
	AudioConverterRef converter = inAudioConverter;
	UInt32 wanted;
	UInt32 produced = 0;
	float *out_floats;
	UInt32 channels;

	(void)outPacketDescription; /* linear PCM is constant bit rate: there is nothing per packet to say */

	if (converter == NULL || inInputDataProc == NULL || ioOutputDataPacketSize == NULL
	    || outOutputData == NULL) {
		return kAudio_ParamError;
	}

	wanted = *ioOutputDataPacketSize;
	channels = converter->out.mChannelsPerFrame;

	if (wanted == 0) {
		*ioOutputDataPacketSize = 0;
		return 0;
	}

	out_floats = malloc((size_t)wanted * channels * sizeof(float));
	if (out_floats == NULL) {
		return kAudio_MemFullError;
	}

	if (!converter->needs_rate_change) {
		/* Same rate: no state, so the simplest correct thing is to pull exactly what was asked for and
		 * convert. A short pull is the end of the stream, and the count says so. */
		UInt32 frames = fn_pull_input(converter, inInputDataProc, inInputDataProcUserData, wanted,
		                              out_floats);

		if (frames > 0) {
			fn_write_buffer_list(converter, outOutputData, out_floats, frames);
		}
		*ioOutputDataPacketSize = frames;
		free(out_floats);
		return 0;
	}

	/* A rate change: keep asking for input until the output is full or the stream ends. */
	while (produced < wanted) {
		UInt32 available = fn_resampler_available(&converter->resampler);

		if (available == 0) {
			UInt32 chunk = 1024;
			UInt32 got;

			fn_ensure_scratch(converter, converter->scratch_capacity + chunk);
			got = fn_pull_input(converter, inInputDataProc, inInputDataProcUserData, chunk,
			                    converter->scratch);

			if (got == 0) {
				break;
			}

			fn_resampler_push(&converter->resampler, converter->scratch, got);
			continue;
		}

		if (available > wanted - produced) {
			available = wanted - produced;
		}

		produced += fn_resampler_pull(&converter->resampler, out_floats + (size_t)produced * channels,
		                              available);
	}

	fn_write_buffer_list(converter, outOutputData, out_floats, produced);
	*ioOutputDataPacketSize = produced;

	free(out_floats);
	return 0;
}

OSStatus AudioConverterConvertBuffer(AudioConverterRef inAudioConverter,
                                     UInt32 inInputDataSize,
                                     const void *inInputData,
                                     UInt32 *ioOutputDataSize,
                                     void *outOutputData)
{
	AudioConverterRef converter = inAudioConverter;
	UInt32 in_frame_bytes;
	UInt32 out_frame_bytes;
	UInt32 frames;
	UInt32 out_frames_capacity;
	UInt32 channels;
	float *floats;

	if (converter == NULL || inInputData == NULL || ioOutputDataSize == NULL || outOutputData == NULL) {
		return kAudio_ParamError;
	}

	if (converter->needs_rate_change) {
		/* Apple's own restriction, and ours for the same reason: the resampler's history has nowhere to
		 * live between one-shot calls. */
		return kAudioConverterErr_FormatNotSupported;
	}

	in_frame_bytes = fn_pcm_bytes_per_frame(&converter->in);
	out_frame_bytes = fn_pcm_bytes_per_frame(&converter->out);
	channels = converter->in.mChannelsPerFrame;

	if (in_frame_bytes == 0 || out_frame_bytes == 0) {
		return kAudioConverterErr_FormatNotSupported;
	}

	frames = inInputDataSize / in_frame_bytes;
	out_frames_capacity = *ioOutputDataSize / out_frame_bytes;

	if (frames > out_frames_capacity) {
		frames = out_frames_capacity;
	}

	if (frames == 0) {
		*ioOutputDataSize = 0;
		return 0;
	}

	floats = malloc((size_t)frames * channels * sizeof(float));
	if (floats == NULL) {
		return kAudio_MemFullError;
	}

	{
		AudioBufferList list;

		/* THE READ AND THE WRITE ARE TWO LISTS, EVEN WHEN THE CALLER CONVERTED IN PLACE: the read list
		 * points at the caller's input, the write list at the caller's output buffer, and conflating them
		 * is how an in-place conversion silently overwrites its own source. */
		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = channels;
		list.mBuffers[0].mDataByteSize = inInputDataSize;
		list.mBuffers[0].mData = (void *)inInputData;

		fn_read_buffer_list(converter, &list, frames, floats);

		memset(&list, 0, sizeof(list));
		list.mNumberBuffers = (converter->out.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
		                    ? converter->out.mChannelsPerFrame : 1;
		list.mBuffers[0].mNumberChannels = channels;
		list.mBuffers[0].mDataByteSize = *ioOutputDataSize;
		list.mBuffers[0].mData = outOutputData;

		fn_write_buffer_list(converter, &list, floats, frames);
	}

	*ioOutputDataSize = frames * out_frame_bytes;
	free(floats);
	return 0;
}

/* The deprecated byte door: the caller hands us bytes through a callback rather than frames through a
 * buffer list. Implemented on top of the same pipeline, one shot at a time, which is all the shape allows:
 * its callback has no sample-rate state either. */
OSStatus AudioConverterFillBuffer(AudioConverterRef inAudioConverter,
                                  AudioConverterInputDataProc inInputDataProc,
                                  void *inInputDataProcUserData,
                                  UInt32 *ioOutputDataSize,
                                  void *outOutputData)
{
	AudioConverterRef converter = inAudioConverter;
	void *input_bytes = NULL;
	UInt32 input_size;
	OSStatus status;

	if (converter == NULL || inInputDataProc == NULL || ioOutputDataSize == NULL
	    || outOutputData == NULL) {
		return kAudio_ParamError;
	}

	if (converter->needs_rate_change) {
		return kAudioConverterErr_FormatNotSupported;
	}

	status = inInputDataProc(converter, &input_size, &input_bytes, inInputDataProcUserData);
	if (status != 0) {
		return status;
	}

	return AudioConverterConvertBuffer(converter, input_size, input_bytes, ioOutputDataSize,
	                                   outOutputData);
}
