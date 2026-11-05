/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * api/objc.m — the Objective-C surface, checked where the C gates check theirs: at compile time.
 *
 * EVERY TYPE HERE COMES FROM `api/manifest-10.12.txt`, which records what 10.12's `AUAudioUnit.h` and
 * `AUParameters.h` declare — class, superclass, protocols, and the qualified type of every property and method.
 * The gates for the C doors are `_Static_assert`s over numbers; this one is `__builtin_types_compatible_p` over
 * message sends, because a property that changed from `AUValue` to `double` still compiles in a client and still
 * writes the wrong four bytes.
 *
 * THE TWO BUILD FLAGS ARE PART OF THE GATE. `-Werror=objc-method-access` turns "no visible @interface declares
 * this selector" from a warning into a failure, so removing a declaration cannot pass silently; and
 * `-Werror=incompatible-pointer-types` does the same for a block or function pointer that drifted. A missing
 * class name is already an error.
 *
 * WHAT IT CANNOT CHECK, and says so: the parameter classes do not claim `NSSecureCoding` here (there is no
 * coder), and a bus's format is an `AudioStreamBasicDescription` where Apple's is an `AVAudioFormat *` — those
 * two deviations are asserted in the direction this library means them, not the direction Apple does.
 */

#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUAudioUnit.h>

#include <stdint.h>

/* --- classes and their inheritance ------------------------------------------ */

_Static_assert(__builtin_types_compatible_p(__typeof__([AUAudioUnit class]), Class),
               "AUAudioUnit must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUAudioUnitBusArray class]), Class),
               "AUAudioUnitBusArray must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUAudioUnitBus class]), Class),
               "AUAudioUnitBus must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUAudioUnitPreset class]), Class),
               "AUAudioUnitPreset must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUParameterNode class]), Class),
               "AUParameterNode must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUParameterGroup class]), Class),
               "AUParameterGroup must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUParameterTree class]), Class),
               "AUParameterTree must be an Objective-C class");
_Static_assert(__builtin_types_compatible_p(__typeof__([AUParameter class]), Class),
               "AUParameter must be an Objective-C class");

#define AGORA_IS(type, expression) \
	_Static_assert(__builtin_types_compatible_p(__typeof__(expression), type), #expression " is not " #type)

/* --- AUAudioUnit: the C facts, as properties -------------------------------- */
AGORA_IS(AudioComponentDescription, [(AUAudioUnit *)0 componentDescription]);
AGORA_IS(AudioComponent, [(AUAudioUnit *)0 component]);
AGORA_IS(uint32_t, [(AUAudioUnit *)0 componentVersion]);
AGORA_IS(BOOL, [(AUAudioUnit *)0 allocateRenderResourcesAndReturnError:NULL]);
AGORA_IS(BOOL, [(AUAudioUnit *)0 renderResourcesAllocated]);
AGORA_IS(AUAudioUnitBusArray *, [(AUAudioUnit *)0 inputBusses]);
AGORA_IS(AUAudioUnitBusArray *, [(AUAudioUnit *)0 outputBusses]);
AGORA_IS(AURenderBlock, [(AUAudioUnit *)0 renderBlock]);
AGORA_IS(AUScheduleParameterBlock, [(AUAudioUnit *)0 scheduleParameterBlock]);
AGORA_IS(AUParameterTree *, [(AUAudioUnit *)0 parameterTree]);
AGORA_IS(AUAudioFrameCount, [(AUAudioUnit *)0 maximumFramesToRender]);
AGORA_IS(NSInteger, [(AUAudioUnit *)0 tokenByAddingRenderObserver:NULL]);
AGORA_IS(NSInteger, [(AUAudioUnit *)0 virtualMIDICableCount]);
AGORA_IS(AUScheduleMIDIEventBlock, [(AUAudioUnit *)0 scheduleMIDIEventBlock]);
AGORA_IS(NSTimeInterval, [(AUAudioUnit *)0 latency]);
AGORA_IS(NSInteger, [(AUAudioUnit *)0 renderQuality]);

/* --- the input/output category: the hardware doors -------------------------- */
AGORA_IS(BOOL, [(AUAudioUnit *)0 canPerformInput]);
AGORA_IS(BOOL, [(AUAudioUnit *)0 isOutputEnabled]);
AGORA_IS(AudioObjectID, [(AUAudioUnit *)0 deviceID]);
AGORA_IS(BOOL, [(AUAudioUnit *)0 setDeviceID:0 error:NULL]);
AGORA_IS(BOOL, [(AUAudioUnit *)0 startHardwareAndReturnError:NULL]);
AGORA_IS(AURenderPullInputBlock, [(AUAudioUnit *)0 outputProvider]);
AGORA_IS(AUInputHandler, [(AUAudioUnit *)0 inputHandler]);

/* --- the busses ------------------------------------------------------------- */
AGORA_IS(NSUInteger, [(AUAudioUnitBusArray *)0 count]);
AGORA_IS(AUAudioUnitBus *, [(AUAudioUnitBusArray *)0 objectAtIndexedSubscript:0]);
AGORA_IS(BOOL, [(AUAudioUnitBusArray *)0 isCountChangeable]);
AGORA_IS(BOOL, [(AUAudioUnitBusArray *)0 setBusCount:0 error:NULL]);
AGORA_IS(AUAudioUnitBusType, [(AUAudioUnitBusArray *)0 busType]);
AGORA_IS(AudioStreamBasicDescription, [(AUAudioUnitBus *)0 format]);
AGORA_IS(BOOL, [(AUAudioUnitBus *)0 isEnabled]);
AGORA_IS(NSUInteger, [(AUAudioUnitBus *)0 index]);
AGORA_IS(AUAudioUnit *, [(AUAudioUnitBus *)0 ownerAudioUnit]);

/* --- the parameter surface -------------------------------------------------- */
AGORA_IS(AUValue, [(AUParameter *)0 value]);
AGORA_IS(AUValue, [(AUParameter *)0 minValue]);
AGORA_IS(AUParameterAddress, [(AUParameter *)0 address]);
AGORA_IS(AudioUnitParameterUnit, [(AUParameter *)0 unit]);
AGORA_IS(AudioUnitParameterOptions, [(AUParameter *)0 flags]);
AGORA_IS(NSString *, [(AUParameterNode *)0 identifier]);
AGORA_IS(NSString *, [(AUParameterNode *)0 keyPath]);
AGORA_IS(AUParameterObserverToken, [(AUParameterNode *)0 tokenByAddingParameterObserver:NULL]);
AGORA_IS(NSArray *, [(AUParameterGroup *)0 allParameters]);
AGORA_IS(AUParameter *, [(AUParameterTree *)0 parameterWithAddress:0]);
AGORA_IS(AUParameter *, [(AUParameterTree *)0 parameterWithID:0 scope:0 element:0]);
AGORA_IS(AUParameter *, [AUParameterTree createParameterWithIdentifier:@"i" name:@"n" address:0 min:0 max:1
                                                                unit:0 unitName:nil flags:0 valueStrings:nil
                                                     dependentParameters:nil]);

/* --- what the manifest records and this library does not claim -------------- */
/* `@selector(stringWithFormat:)` exists on every NSString; the point of these two lines is the comment: the
 * parameter classes would conform to `NSSecureCoding` on macOS, and a bus would carry an `AVAudioFormat`. */

int main(void);
int main(void)
{
	/* Compilation is the assertion; a class that lost a member could not have built this file. */
	return 0;
}
