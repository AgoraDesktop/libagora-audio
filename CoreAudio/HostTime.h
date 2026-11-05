/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * CoreAudio/HostTime.h — the host's time base, and the conversions to and from nanoseconds.
 *
 * THE API IS macOS 10.12'S: five functions, `UInt64` host ticks and a `Float64` frequency, exactly the
 * declarations in `MacOSX10.12.sdk`, checked by `tests/api/prototypes.c`. What is ours is the clock
 * behind them: `clock_gettime(CLOCK_MONOTONIC)` scaled to Apple's tick frequency, so a host time is
 * still a number that only means anything relative to another host time — which is the whole of the
 * contract the header documents.
 */
#ifndef AGORA_COREAUDIO_HOSTTIME_H
#define AGORA_COREAUDIO_HOSTTIME_H

#include <CoreAudio/CoreAudioTypes.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

/* The current host time. Monotonic, and never a calendar date. */
extern UInt64
AudioGetCurrentHostTime(void);

/* The number of ticks per second in the host time base. */
extern Float64
AudioGetHostClockFrequency(void);

/* The smallest number of ticks two succeeding values will ever differ by. */
extern UInt32
AudioGetHostClockMinimumTimeDelta(void);

/* A host time in nanoseconds, and back. */
extern UInt64
AudioConvertHostTimeToNanos(UInt64 inHostTime);

extern UInt64
AudioConvertNanosToHostTime(UInt64 inNanos);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_COREAUDIO_HOSTTIME_H */
