/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * HostTime.c — the host time base.
 *
 * **THE CLOCK IS `CLOCK_MONOTONIC`, SCALED TO APPLE'S FREQUENCY.** On macOS the host time base is a
 * counter whose frequency the system reports and whose unit is otherwise meaningless; a client only
 * ever subtracts two host times or converts both to nanoseconds. So the frequency is ours to choose,
 * and 1e9 makes host ticks nanoseconds — the same arithmetic Apple's own clock does on a modern
 * machine, and the reason `AudioGetHostClockMinimumTimeDelta` is 1.
 *
 * WHY NOT `CLOCK_MONOTONIC_RAW`: this is a *playback* clock. A device's frame position advances in
 * wall time; the raw clock drifts against it deliberately, and NTP's slewing of MONOTONIC is exactly
 * the correction a jitter-free timeline wants. `AudioDeviceGetCurrentTime` (phase 2) reports host
 * times from the same base, so everything that mixes them agrees.
 *
 * THE CONVERSIONS ARE ROUNDED, NOT TRUNCATED, and they are inverses within one tick: a caller that
 * converts out and back gets the same tick, which `tests/hosttime.c` measures rather than assumes.
 */

#include <CoreAudio/HostTime.h>

#include <time.h>

/* 1e9 ticks per second, as a double, because Apple's declaration is `Float64`. */
static const Float64 kAgoraHostClockFrequency = 1.0e9;

/* One tick, because a nanosecond clock has nothing smaller. */
static const UInt32 kAgoraHostClockMinimumTimeDelta = 1;

static UInt64 fn_now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		/* A monotonic clock that cannot be read is not a clock; report the epoch and let the
		 * first subtraction produce nonsense rather than hiding the failure here. */
		return 0;
	}

	return (UInt64)ts.tv_sec * 1000000000ULL + (UInt64)ts.tv_nsec;
}

UInt64 AudioGetCurrentHostTime(void)
{
	return fn_now();
}

Float64 AudioGetHostClockFrequency(void)
{
	return kAgoraHostClockFrequency;
}

UInt32 AudioGetHostClockMinimumTimeDelta(void)
{
	return kAgoraHostClockMinimumTimeDelta;
}

UInt64 AudioConvertHostTimeToNanos(UInt64 inHostTime)
{
	/* A tick IS a nanosecond at this frequency, so the general scaling below would round a 53-bit
	 * mantissa for nothing. The exact branch is the fast one and the honest one. */
	if (kAgoraHostClockFrequency == 1.0e9) {
		return inHostTime;
	}

	return (UInt64)((Float64)inHostTime * (1.0e9 / kAgoraHostClockFrequency) + 0.5);
}

UInt64 AudioConvertNanosToHostTime(UInt64 inNanos)
{
	if (kAgoraHostClockFrequency == 1.0e9) {
		return inNanos;
	}

	return (UInt64)((Float64)inNanos * (kAgoraHostClockFrequency / 1.0e9) + 0.5);
}
