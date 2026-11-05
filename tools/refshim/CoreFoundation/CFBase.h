/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * A stand-in for Core Foundation's *types*, for parsing Apple's audio headers. NOT part of the
 * library: nothing in CoreAudio/ includes it, and it is never installed.
 *
 * WHY A SHIM AND NOT OUR OWN CORE FOUNDATION: the reference parse must depend on nothing in this
 * tree. With the real headers on the include path, clang follows CoreFoundation.h into time.h,
 * math.h and the rest, and a libc header that spells `__nonnull` the way Darwin does becomes the
 * manifest's problem. Thirty typedefs cannot acquire that kind of dependency.
 *
 * EVERY TYPE HERE IS APPLE': the audio headers only use these as opaque references in their
 * signatures, and the manifest records the signature, so a wrapper that is `void *` where Darwin's
 * is a pointer to a struct changes nothing the manifest is asserting.
 */
#ifndef __AGORA_REFSHIM_CFBASE__
#define __AGORA_REFSHIM_CFBASE__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool Boolean;
typedef unsigned char Byte;
typedef unsigned char UInt8;
typedef signed char SInt8;
typedef unsigned short UInt16;
typedef signed short SInt16;
typedef unsigned int UInt32;
typedef signed int SInt32;
typedef uint64_t UInt64;
typedef int64_t SInt64;
typedef SInt32 OSStatus;
typedef SInt16 OSErr;
typedef unsigned int FourCharCode;
typedef FourCharCode OSType;
typedef float Float32;
typedef double Float64;

typedef long CFIndex;
typedef double CFTimeInterval;
typedef double CFAbsoluteTime;
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFString *CFMutableStringRef;
typedef const struct __CFData *CFDataRef;
typedef struct __CFData *CFMutableDataRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFUUID *CFUUIDRef;
typedef const struct __CFRunLoop *CFRunLoopRef;
typedef struct __CFRunLoopSource *CFRunLoopSourceRef;
typedef const struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef CFStringRef CFRunLoopMode;
typedef const struct __CFPropertyList *CFPropertyListRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef const struct __CFBag *CFBagRef;
typedef const struct __CFError *CFErrorRef;
typedef const struct __CFNotificationCenter *CFNotificationCenterRef;
typedef const struct __CFCharacterSet *CFCharacterSetRef;
typedef const struct __CFSet *CFSetRef;

/* CoreFoundation's own C fallback for the enum macros, which is what Apple compiles in C as well:
 * `typedef CF_ENUM(UInt32, X) { ... };` becomes `typedef UInt32 X; enum { ... };`. Reproducing the
 * Darwin fixed-underlying-type form instead needs `enum X : UInt32`, a C++-only spelling here. */
#define _CA_PICK(_1, _2, NAME, ...) NAME
#define _CA_ENUM1(_t) enum
#define _CA_ENUM2(_t, _n) _t _n; enum
#define CF_ENUM(...) _CA_PICK(__VA_ARGS__, _CA_ENUM2, _CA_ENUM1)(__VA_ARGS__)
#define CF_OPTIONS(...) CF_ENUM(__VA_ARGS__)

#define CF_ENUM_AVAILABLE(...)
#define CF_SWIFT_NAME(_n)
#define CF_EXPORT extern
#define CF_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define CF_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#define CF_RETURNS_RETAINED
#define CF_RETURNS_NOT_RETAINED

#endif
