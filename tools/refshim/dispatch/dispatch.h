/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * A stand-in for <dispatch/dispatch.h>: the types Apple's block-based audio APIs name in their
 * signatures, and nothing else. NOT part of the library, never installed.
 *
 * The real libagora-dispatch would work too, but the reference parse must not depend on this tree,
 * and these six typedefs are the whole of what CoreAudio and AudioToolbox mention.
 */
#ifndef __AGORA_REFSHIM_DISPATCH__
#define __AGORA_REFSHIM_DISPATCH__

typedef void (^dispatch_block_t)(void);
typedef void (*dispatch_function_t)(void *);
typedef struct dispatch_queue_s *dispatch_queue_t;
typedef struct dispatch_queue_attr_s *dispatch_queue_attr_t;
typedef struct dispatch_data_s *dispatch_data_t;
typedef struct dispatch_source_s *dispatch_source_t;
typedef struct dispatch_semaphore_s *dispatch_semaphore_t;
typedef struct dispatch_group_s *dispatch_group_t;
typedef unsigned long dispatch_queue_priority_t;
typedef unsigned long dispatch_time_t;

#define DISPATCH_QUEUE_SERIAL ((dispatch_queue_attr_t)0)

#endif
