/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AUAudioUnit_internal.h — what a bus and a bus array have to ask their unit.
 *
 * NOT INSTALLED (the install rule excludes `*_internal.h`), and it holds no Apple declaration: the two classes
 * in `AUAudioUnit.h` are one object with three faces — the unit knows the C instance, a bus knows which
 * element of it it *is*, and an array knows the unit. Apple's headers spell that relationship with a
 * designated initializer that takes the owner, because the owner is the only thing that can answer; this is
 * the same relationship, kept out of the public header because it exposes the C instance.
 *
 * THE ADDRESS ENCODING LIVES HERE, on purpose: `AUParameterAddress` is opaque, and the one place our choice of
 * what to put in it must agree with itself is between the tree that writes an address and the search that
 * reads one back.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_INTERNAL_H
#define AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_INTERNAL_H

#import <AudioToolbox/AUAudioUnit.h>

/* `scope << 32 | element << 16 | id`. A v2 unit's parameter has three coordinates and an address has one
 * 64-bit slot; 16 bits each for the element and the id is far more than `AGORA_UNIT_MAX_ELEMENTS` and the
 * parameter ids any built-in uses, and the top half leaves the scope where a reader can see it in hex. */
static inline AUParameterAddress fn_agora_au_address(AudioUnitScope scope, AudioUnitElement element,
                                                     AudioUnitParameterID id)
{
	return ((AUParameterAddress)scope << 32) | ((AUParameterAddress)element << 16) | (AUParameterAddress)id;
}

static inline AudioUnitScope fn_agora_au_address_scope(AUParameterAddress address)
{
	return (AudioUnitScope)(address >> 32);
}

static inline AudioUnitElement fn_agora_au_address_element(AUParameterAddress address)
{
	return (AudioUnitElement)((address >> 16) & 0xffff);
}

static inline AudioUnitParameterID fn_agora_au_address_id(AUParameterAddress address)
{
	return (AudioUnitParameterID)(address & 0xffff);
}

@interface AUAudioUnit (AgoraInternal)

/* The C instance behind the object: never NULL after a successful init, and borrowed, not owned. */
- (AudioUnit)agora_instance;
- (BOOL)agora_allocated;

@end

@interface AUAudioUnitBus (AgoraInternal)

- (instancetype)agora_initWithOwner:(AUAudioUnit *)owner
                            busType:(AUAudioUnitBusType)busType
                              index:(NSUInteger)index;

@end

@interface AUAudioUnitBusArray (AgoraInternal)

- (instancetype)agora_initWithOwner:(AUAudioUnit *)owner busType:(AUAudioUnitBusType)busType;
/* Rebuilt from the unit after `-setBusCount:error:` changed the element count underneath it. */
- (void)agora_reload;

@end

#endif /* AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_INTERNAL_H */
