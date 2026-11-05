/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 *
 * A stand-in for Apple's <Availability.h>: the vocabulary the audio headers decorate their
 * declarations with, defined to nothing on a target that has no Apple version numbers. The
 * declarations either side of it are what the manifest records; this only has to parse.
 */
#ifndef __AGORA_REFSHIM_AVAILABILITY__
#define __AGORA_REFSHIM_AVAILABILITY__
#define API_AVAILABLE(...)
#define API_UNAVAILABLE(...)
#define API_DEPRECATED(...)
#define API_DEPRECATED_WITH_REPLACEMENT(...)
#define API_TO_BE_DEPRECATED 100000
#define __API_AVAILABLE(...)
#define __API_DEPRECATED(...)
#define __API_DEPRECATED_WITH_REPLACEMENT(...)
#define __OSX_AVAILABLE_STARTING(...)
#define __OSX_AVAILABLE_BUT_DEPRECATED(...)
#define __OSX_AVAILABLE(...)
#define __IOS_AVAILABLE(...)
#endif
