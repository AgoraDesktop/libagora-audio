/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 *
 * A stand-in for Apple's <TargetConditionals.h>, so clang on Linux can parse the macOS 10.12
 * headers this component is specified by. NOT part of the library: nothing in CoreAudio/ includes
 * it, and it is never installed.
 *
 * The values say "Darwin, x86_64, 64-bit, not iOS", because the reference headers are Darwin
 * headers: several of them hold declarations behind TARGET_OS_MAC, and a manifest generated with
 * that false would be missing exactly the declarations it exists to record.
 */
#ifndef __AGORA_REFSHIM_TARGETCONDITIONALS__
#define __AGORA_REFSHIM_TARGETCONDITIONALS__
#define TARGET_OS_MAC 1
#define TARGET_OS_OSX 1
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_WATCH 0
#define TARGET_OS_TV 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_WIN32 0
#define TARGET_OS_EMBEDDED 0
#define TARGET_CPU_X86_64 1
#define TARGET_RT_64_BIT 1
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN 0
#endif
