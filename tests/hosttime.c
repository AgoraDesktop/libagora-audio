/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * hosttime.c — the host clock, measured.
 *
 * THE CONTRACT IS SMALL AND EVERY PART OF IT IS CHECKABLE WITHOUT A SOUND CARD: the clock advances, it
 * never goes backwards, its frequency is the one it reports, the two conversions are inverses, and a
 * host time and the nanoseconds it converts to advance together. A playback clock that fails any of
 * these makes every timestamp above it a lie, so they are asserted rather than assumed.
 */

#include <CoreAudio/HostTime.h>

#include <stdio.h>
#include <time.h>

static int g_failures;

static void fn_check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n", what);
		g_failures++;
	}
}

/* A real sleep, because the point is that the clock moves while a person waits. */
static void fn_sleep_ms(long milliseconds)
{
	struct timespec ts;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0) {
		/* Interrupted: finish the sleep. */
	}
}

int main(void);
int main(void)
{
	UInt64 first = AudioGetCurrentHostTime();
	UInt64 second;
	UInt64 nanos_first;
	UInt64 nanos_second;
	Float64 frequency = AudioGetHostClockFrequency();
	UInt32 minimum_delta = AudioGetHostClockMinimumTimeDelta();
	int i;

	fn_check("the frequency is 1e9 ticks per second", frequency == 1.0e9);
	fn_check("the minimum delta is one tick", minimum_delta == 1);
	fn_check("the clock is not zero", first != 0);

	/* Monotonic, and strictly increasing across a real wait. */
	for (i = 0; i < 1000; i++) {
		second = AudioGetCurrentHostTime();
		fn_check("the clock never goes backwards", second >= first);
		if (second < first) {
			break;
		}
		first = second;
	}

	nanos_first = AudioConvertHostTimeToNanos(first);
	fn_sleep_ms(20);
	second = AudioGetCurrentHostTime();
	nanos_second = AudioConvertHostTimeToNanos(second);

	fn_check("a 20 ms sleep advances the clock", second > first);
	fn_check("a 20 ms sleep is at least 15 ms of host time",
	         (nanos_second - nanos_first) >= 15000000ULL);
	fn_check("a 20 ms sleep is less than 5 s of host time (no clock jump)",
	         (nanos_second - nanos_first) < 5000000000ULL);

	/* The two conversions are inverses, which is what makes a latency measurement invertible. */
	fn_check("nanos -> host -> nanos is the identity for a live time",
	         AudioConvertHostTimeToNanos(AudioConvertNanosToHostTime(nanos_second)) == nanos_second);
	fn_check("host -> nanos -> host is the identity for a live time",
	         AudioConvertNanosToHostTime(AudioConvertHostTimeToNanos(second)) == second);

	/* And they agree with each other across the same interval. */
	fn_check("the converted interval equals the interval",
	         (nanos_second - nanos_first) == (second - first));

	if (g_failures != 0) {
		fprintf(stderr, "hosttime: %d failure(s)\n", g_failures);
		return 1;
	}

	printf("hosttime: monotonic, %g ticks/s, conversions invert\n", (double)frequency);
	return 0;
}
