/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AUParameters.h — a unit's parameters as objects: nodes, groups, leaves, and the observers.
 *
 * A PARAMETER IS A NUMBER WITH A NAME AND A RANGE, and this door is the first thing in this library that
 * is not C — a tree of `NSObject`s over the same values `AudioUnitGetParameter` reads. Apple splits the
 * description in two: the C side is `AudioUnitParameterInfo` (a static table per unit), and this side is a
 * tree built *from* it, with an observer called whenever a value changes.
 *
 * A TREE IS NOT A LIST, AND THE ADDRESS IS WHY. `AUParameterAddress` is an opaque 64-bit address, not a
 * `AudioUnitParameterID`: a v2 unit's parameter is identified by three coordinates (id, scope, element), so
 * this library encodes them — `scope << 32 | element << 16 | id` — and `AUParameterTree`'s
 * `-parameterWithID:scope:element:` is the door that takes the three back apart. A key path is the other
 * naming: a node's is its ancestors' identifiers joined by dots, and a tree's own is empty, because a tree is
 * not a node a caller can address. That encoding is ours, not
 * Apple's; it is stated in the README, and it is the only thing about this header that is.
 *
 * SPECIFIED BY macOS 10.12: the value and address types, the observer blocks, the four classes and every
 * member are the reference's — except the `<NSSecureCoding>` conformance those classes carry there and not
 * here: this phase has no coder for a tree, and claiming the protocol without it would be a promise
 * `-encodeWithCoder:` could not keep. The implementor blocks and the factory class methods come from
 * `AUAudioUnitImplementation.h`, Apple's header for people who *implement* a unit rather than host one;
 * they are here because this library implements units and a client should not need a second include path.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUPARAMETERS_H
#define AGORA_AUDIOTOOLBOX_AUPARAMETERS_H

#include <AudioToolbox/AudioUnit.h>

#if defined(__OBJC__)

#include <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class AUParameter;
@class AUParameterNode;

typedef float AUValue;
typedef uint64_t AUParameterAddress;

typedef NS_ENUM(uint32_t, AUParameterAutomationEventType)
{
	AUParameterAutomationEventTypeValue   = 0,
	AUParameterAutomationEventTypeTouch   = 1,
	AUParameterAutomationEventTypeRelease = 2
};

typedef struct AURecordedParameterEvent
{
	uint64_t            hostTime;	/* when it happened */
	AUParameterAddress  address;	/* which parameter */
	AUValue             value;	/* what it became */
} AURecordedParameterEvent;

typedef struct AUParameterAutomationEvent
{
	uint64_t                      hostTime;
	AUParameterAddress            address;
	AUValue                       value;
	AUParameterAutomationEventType eventType;
	uint64_t                      reserved;
} AUParameterAutomationEvent;

typedef void (^AUParameterObserver)(AUParameterAddress address, AUValue value);
typedef void (^AUParameterRecordingObserver)(NSInteger numberEvents, const AURecordedParameterEvent *events);
typedef void (^AUParameterAutomationObserver)(NSInteger numberEvents,
                                              const AUParameterAutomationEvent *events);
typedef void *AUParameterObserverToken;

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUParameterNode : NSObject

@property (nonatomic, readonly, copy) NSString *identifier;
@property (nonatomic, readonly, copy) NSString *keyPath;
@property (nonatomic, readonly, copy) NSString *displayName;

- (NSString *)displayNameWithLength:(NSInteger)maximumLength;

- (AUParameterObserverToken)tokenByAddingParameterObserver:(AUParameterObserver)observer;
- (AUParameterObserverToken)tokenByAddingParameterRecordingObserver:(AUParameterRecordingObserver)observer;
- (AUParameterObserverToken)tokenByAddingParameterAutomationObserver:(AUParameterAutomationObserver)observer
    NS_AVAILABLE(10_12, 10_0);
- (void)removeParameterObserver:(AUParameterObserverToken)token;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUParameterGroup : AUParameterNode

@property (nonatomic, readonly) NSArray<AUParameterNode *> *children;
@property (nonatomic, readonly) NSArray<AUParameter *> *allParameters;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUParameterTree : AUParameterGroup

- (AUParameter * _Nullable)parameterWithAddress:(AUParameterAddress)address;
- (AUParameter * _Nullable)parameterWithID:(AudioUnitParameterID)paramID
                                     scope:(AudioUnitScope)scope
                                   element:(AudioUnitElement)element;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUParameter : AUParameterNode

@property (nonatomic, readonly) AUValue minValue;
@property (nonatomic, readonly) AUValue maxValue;
@property (nonatomic, readonly) AudioUnitParameterUnit unit;
@property (nonatomic, readonly, copy, nullable) NSString *unitName;
@property (nonatomic, readonly) AudioUnitParameterOptions flags;
@property (nonatomic, readonly) AUParameterAddress address;
@property (nonatomic, readonly, copy, nullable) NSArray<NSString *> *valueStrings;
@property (nonatomic, readonly, copy, nullable) NSArray<NSNumber *> *dependentParameters;
@property (nonatomic) AUValue value;

- (void)setValue:(AUValue)value originator:(AUParameterObserverToken _Nullable)originator;
- (void)setValue:(AUValue)value
        originator:(AUParameterObserverToken _Nullable)originator
        atHostTime:(uint64_t)hostTime;
- (void)setValue:(AUValue)value
        originator:(AUParameterObserverToken _Nullable)originator
        atHostTime:(uint64_t)hostTime
         eventType:(AUParameterAutomationEventType)eventType NS_AVAILABLE(10_12, 10_0);
- (NSString *)stringFromValue:(const AUValue * _Nullable)value;
- (AUValue)valueFromString:(NSString *)string;

@end

/* --- what a unit that *implements* a parameter tree has to supply ---------------------------------- */

typedef void (^AUImplementorValueObserver)(AUParameter *param, AUValue value);
typedef AUValue (^AUImplementorValueProvider)(AUParameter *param);
typedef NSString * _Nonnull (^AUImplementorStringFromValueCallback)(AUParameter *param,
                                                                    const AUValue * _Nullable value);
typedef AUValue (^AUImplementorValueFromStringCallback)(AUParameter *param, NSString *string);
typedef NSString * _Nonnull (^AUImplementorDisplayNameWithLengthCallback)(AUParameterNode *node,
                                                                         NSInteger desiredLength);

@interface AUParameterNode (AUParameterNodeImplementation)

/* A set is told to the unit through the observer and read back through the provider. That pair is the
 * whole bridge between a tree and a `AudioUnitSetParameter` — and it is why the two are separate: the unit
 * may clamp, quantise or ignore, and the tree is only correct if it asks what the value became. */
@property (nonatomic, copy) AUImplementorValueObserver implementorValueObserver;
@property (nonatomic, copy) AUImplementorValueProvider implementorValueProvider;
@property (nonatomic, copy) AUImplementorStringFromValueCallback implementorStringFromValueCallback;
@property (nonatomic, copy) AUImplementorValueFromStringCallback implementorValueFromStringCallback;
@property (nonatomic, copy) AUImplementorDisplayNameWithLengthCallback implementorDisplayNameWithLengthCallback;

@end

@interface AUParameterTree (Factory)

+ (AUParameter *)createParameterWithIdentifier:(NSString *)identifier
                                          name:(NSString *)name
                                       address:(AUParameterAddress)address
                                           min:(AUValue)min
                                           max:(AUValue)max
                                          unit:(AudioUnitParameterUnit)unit
                                      unitName:(NSString * _Nullable)unitName
                                         flags:(AudioUnitParameterOptions)flags
                                  valueStrings:(NSArray<NSString *> * _Nullable)valueStrings
                           dependentParameters:(NSArray<NSNumber *> * _Nullable)dependentParameters;
+ (AUParameterGroup *)createGroupWithIdentifier:(NSString *)identifier
                                           name:(NSString *)name
                                       children:(NSArray<AUParameterNode *> *)children;
+ (AUParameterTree *)createTreeWithChildren:(NSArray<AUParameterNode *> *)children;

@end

NS_ASSUME_NONNULL_END

#endif /* __OBJC__ */

#endif /* AGORA_AUDIOTOOLBOX_AUPARAMETERS_H */
