/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * api/compile-cxx.cpp — the doors under a C++ compiler.
 *
 * A FRAMEWORK THAT ONLY COMPILES AS C IS HALF A FRAMEWORK: Apple's audio headers are used from C++ all
 * the time (an audio engine is usually C++), and the parts that make that work — `extern "C"`, the
 * fixed-underlying-type enumerations, the inline helpers Apple ships — are exactly the parts a
 * hand-written header forgets. Nothing here runs; if it compiles, it passed.
 */

#include <CoreAudio/CoreAudio.h>

#include <cstddef>
#include <cstring>

static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD layout");
static_assert(sizeof(AudioBufferList) == 24, "AudioBufferList layout");
static_assert(offsetof(AudioTimeStamp, mFlags) == 56, "AudioTimeStamp flags offset");

int main();
int main()
{
	AudioStreamBasicDescription asbd = {};
	AudioTimeStamp            stamp = {};

	asbd.mSampleRate = 48000.0;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = CalculateLPCMFlags(32, 32, true, false, false);

	if (!IsAudioFormatNativeEndian(asbd)) {
		return 1;
	}

	/* The flags must be the same ones the constant combination names. */
	if (asbd.mFormatFlags != kAudioFormatFlagsNativeFloatPacked) {
		return 2;
	}

	FillOutAudioTimeStampWithSampleTime(stamp, 1024.0);
	if (stamp.mFlags != kAudioTimeStampSampleTimeValid || stamp.mSampleTime != 1024.0) {
		return 3;
	}

	FillOutAudioTimeStampWithSampleAndHostTime(stamp, 2048.0, 7);
	if (stamp.mFlags != kAudioTimeStampSampleHostTimeValid || stamp.mHostTime != 7) {
		return 4;
	}

	return 0;
}
