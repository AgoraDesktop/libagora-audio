/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AUParameters.m — the parameter tree over a C unit's parameter table.
 *
 * THE TREE IS BUILT FROM `AudioUnitGetProperty`, NOT FROM A SECOND TABLE. `AUAudioUnit` asks the unit for its
 * parameter list and for each id's `AudioUnitParameterInfo`, and this file turns that into nodes. There is one
 * description of a parameter in this library and it is the C one; the tree is a view of it.
 *
 * A SET GOES THROUGH THREE DOORS IN ORDER: the *implementor observer* is told (the unit writes the value),
 * then the *value provider* is asked what the value became (the unit clamps, or quantises a boolean), and only
 * then are the parameter's own observers called — with the value the unit reported, never the value that was
 * asked for. That order is why a slider cannot show a value the unit refused, and why `-value` reads the
 * provider instead of a cached copy.
 *
 * MANUAL OWNERSHIP, as everywhere in this tree: no ARC on this runtime, so an ivar is retained when it is
 * stored and released in `-dealloc`; an observer block arrives on the stack and is `Block_copy`'d; and a node's
 * parent is *not* retained, because a group owns its children and the chain would be a cycle.
 *
 * SPECIFIED BY macOS 10.12 for everything a client can see. The `-initWith…` doors used between these classes
 * are ours (Apple keeps them in its implementation header); a client builds a tree with the
 * `AUParameterTree (Factory)` methods, which are Apple's.
 */

#import <AudioToolbox/AUParameters.h>
#import <AudioToolbox/AUAudioUnit_internal.h>

#include <Block.h>

/* A token is an address a caller holds and hands back. A global counter means two registrations cannot
 * collide, which matters because a token is compared rather than dereferenced: a collision would remove
 * somebody else's observer and nothing would say so. */
static AUParameterObserverToken fn_next_token(void)
{
	static uint64_t serial = 0;
	return (AUParameterObserverToken)(uintptr_t)__atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST);
}

/* One registration: the block, which of the three kinds it is, and nothing else. A wrapper rather than a
 * bare block in the dictionary so that the block's lifetime is one statement in `-dealloc` instead of a
 * question about whether the collection retained it. */
typedef NS_ENUM(NSInteger, agora_observer_kind) {
	AGORA_OBSERVER_PLAIN = 0,
	AGORA_OBSERVER_RECORDING = 1,
	AGORA_OBSERVER_AUTOMATION = 2
};

@interface AUParameterRegistration : NSObject
{
@public
	id _block;	/* Block_copy'd */
	agora_observer_kind _kind;
}
@end

@implementation AUParameterRegistration
- (void)dealloc
{
	Block_release(_block);
	[super dealloc];
}
@end

/* ------------------------------------------------------------------------------------------------
 * AUParameterNode
 * ---------------------------------------------------------------------------------------------- */

@interface AUParameterNode ()
{
@protected
	NSString *_identifier;
	NSString *_displayName;
	AUParameterNode *_parent;	/* not retained: a group owns its children */
	NSMutableDictionary *_observers;	/* NSNumber(token) -> AUParameterRegistration */
	AUImplementorValueObserver _implementorValueObserver;
	AUImplementorValueProvider _implementorValueProvider;
	AUImplementorStringFromValueCallback _implementorStringFromValueCallback;
	AUImplementorValueFromStringCallback _implementorValueFromStringCallback;
	AUImplementorDisplayNameWithLengthCallback _implementorDisplayNameWithLengthCallback;
}
- (instancetype)initWithIdentifier:(NSString *)identifier name:(NSString *)displayName;
- (void)fn_setParent:(AUParameterNode *)parent;
- (AUParameterObserverToken)fn_addObserver:(id)block kind:(agora_observer_kind)kind;
- (void)fn_notifyAddress:(AUParameterAddress)address
                   value:(AUValue)value
                hostTime:(uint64_t)hostTime
              originator:(AUParameterObserverToken)originator;
@end

@implementation AUParameterNode

- (instancetype)initWithIdentifier:(NSString *)identifier name:(NSString *)displayName
{
	if ((self = [super init])) {
		_identifier = [identifier copy];
		_displayName = [displayName copy];
		_observers = [[NSMutableDictionary alloc] init];
	}
	return self;
}

- (void)dealloc
{
	/* Releasing the dictionary releases every registration, and a registration releases its block. */
	[_observers release];
	[_identifier release];
	[_displayName release];
	if (_implementorValueObserver != NULL) { Block_release(_implementorValueObserver); }
	if (_implementorValueProvider != NULL) { Block_release(_implementorValueProvider); }
	if (_implementorStringFromValueCallback != NULL) { Block_release(_implementorStringFromValueCallback); }
	if (_implementorValueFromStringCallback != NULL) { Block_release(_implementorValueFromStringCallback); }
	if (_implementorDisplayNameWithLengthCallback != NULL) {
		Block_release(_implementorDisplayNameWithLengthCallback);
	}
	[super dealloc];
}

- (void)fn_setParent:(AUParameterNode *)parent
{
	_parent = parent;	/* not retained */
}

- (NSString *)identifier
{
	return _identifier;
}

- (NSString *)displayName
{
	return _displayName;
}

- (NSString *)keyPath
{
	NSString *parentPath;

	if (_parent == nil) {
		return _identifier;
	}
	/* A tree has no identifier of its own, so its children's key paths start at the first group. */
	parentPath = [_parent keyPath];
	if ([parentPath length] == 0) {
		return _identifier;
	}
	return [NSString stringWithFormat:@"%@.%@", parentPath, _identifier];
}

- (NSString *)displayNameWithLength:(NSInteger)maximumLength
{
	if (_implementorDisplayNameWithLengthCallback != NULL) {
		return _implementorDisplayNameWithLengthCallback(self, maximumLength);
	}
	if (maximumLength <= 0 || (NSInteger)[_displayName length] <= maximumLength) {
		return _displayName;
	}
	return [_displayName substringToIndex:(NSUInteger)maximumLength];
}

- (AUParameterObserverToken)fn_addObserver:(id)block kind:(agora_observer_kind)kind
{
	AUParameterObserverToken token = fn_next_token();
	NSNumber *key = [NSNumber numberWithUnsignedLongLong:(unsigned long long)(uintptr_t)token];
	AUParameterRegistration *registration = [[AUParameterRegistration alloc] init];

	registration->_block = Block_copy(block);
	registration->_kind = kind;
	[_observers setObject:registration forKey:key];
	[registration release];

	return token;
}

- (AUParameterObserverToken)tokenByAddingParameterObserver:(AUParameterObserver)observer
{
	return [self fn_addObserver:observer kind:AGORA_OBSERVER_PLAIN];
}

- (AUParameterObserverToken)tokenByAddingParameterRecordingObserver:(AUParameterRecordingObserver)observer
{
	return [self fn_addObserver:observer kind:AGORA_OBSERVER_RECORDING];
}

- (AUParameterObserverToken)tokenByAddingParameterAutomationObserver:(AUParameterAutomationObserver)observer
{
	return [self fn_addObserver:observer kind:AGORA_OBSERVER_AUTOMATION];
}

- (void)removeParameterObserver:(AUParameterObserverToken)token
{
	NSNumber *key = [NSNumber numberWithUnsignedLongLong:(unsigned long long)(uintptr_t)token];

	[_observers removeObjectForKey:key];
}

- (void)fn_notifyAddress:(AUParameterAddress)address
                   value:(AUValue)value
                hostTime:(uint64_t)hostTime
              originator:(AUParameterObserverToken)originator
{
	NSEnumerator *keys = [_observers keyEnumerator];
	NSNumber *key;

	while ((key = [keys nextObject]) != nil) {
		AUParameterRegistration *registration;

		if ((AUParameterObserverToken)(uintptr_t)[key unsignedLongLongValue] == originator) {
			continue;	/* the caller did this one; telling it would be an echo */
		}

		registration = [_observers objectForKey:key];
		switch (registration->_kind) {
		case AGORA_OBSERVER_PLAIN:
			((AUParameterObserver)registration->_block)(address, value);
			break;
		case AGORA_OBSERVER_RECORDING: {
			AURecordedParameterEvent event = { hostTime, address, value };
			((AUParameterRecordingObserver)registration->_block)(1, &event);
			break;
		}
		case AGORA_OBSERVER_AUTOMATION: {
			AUParameterAutomationEvent event = { hostTime, address, value,
			                                     AUParameterAutomationEventTypeValue, 0 };
			((AUParameterAutomationObserver)registration->_block)(1, &event);
			break;
		}
		}
	}
}

/* --- the implementor doors -------------------------------------------------- */

- (AUImplementorValueObserver)implementorValueObserver
{
	return _implementorValueObserver;
}

- (AUImplementorValueProvider)implementorValueProvider
{
	return _implementorValueProvider;
}

- (AUImplementorStringFromValueCallback)implementorStringFromValueCallback
{
	return _implementorStringFromValueCallback;
}

- (AUImplementorValueFromStringCallback)implementorValueFromStringCallback
{
	return _implementorValueFromStringCallback;
}

- (AUImplementorDisplayNameWithLengthCallback)implementorDisplayNameWithLengthCallback
{
	return _implementorDisplayNameWithLengthCallback;
}

- (void)setImplementorValueObserver:(AUImplementorValueObserver)observer
{
	_implementorValueObserver = observer != NULL ? Block_copy(observer) : NULL;
}

- (void)setImplementorValueProvider:(AUImplementorValueProvider)provider
{
	_implementorValueProvider = provider != NULL ? Block_copy(provider) : NULL;
}

- (void)setImplementorStringFromValueCallback:(AUImplementorStringFromValueCallback)callback
{
	_implementorStringFromValueCallback = callback != NULL ? Block_copy(callback) : NULL;
}

- (void)setImplementorValueFromStringCallback:(AUImplementorValueFromStringCallback)callback
{
	_implementorValueFromStringCallback = callback != NULL ? Block_copy(callback) : NULL;
}

- (void)setImplementorDisplayNameWithLengthCallback:(AUImplementorDisplayNameWithLengthCallback)callback
{
	_implementorDisplayNameWithLengthCallback = callback != NULL ? Block_copy(callback) : NULL;
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUParameterGroup and AUParameterTree
 * ---------------------------------------------------------------------------------------------- */

@interface AUParameterGroup ()
{
	NSArray *_children;
	NSArray *_allParameters;	/* built on first ask */
}
- (instancetype)initWithIdentifier:(NSString *)identifier
                             name:(NSString *)displayName
                         children:(NSArray *)children;
@end

@implementation AUParameterGroup

- (instancetype)initWithIdentifier:(NSString *)identifier
                             name:(NSString *)displayName
                         children:(NSArray *)children
{
	if ((self = [super initWithIdentifier:identifier name:displayName])) {
		_children = [children copy];
		for (AUParameterNode *child in _children) {
			[child fn_setParent:self];
		}
	}
	return self;
}

- (void)dealloc
{
	[_children release];
	[_allParameters release];
	[super dealloc];
}

- (NSArray *)children
{
	return _children;
}

- (NSArray *)allParameters
{
	NSMutableArray *flat;

	if (_allParameters != nil) {
		return _allParameters;
	}

	flat = [[NSMutableArray alloc] init];
	for (AUParameterNode *child in _children) {
		if ([child isKindOfClass:[AUParameterGroup class]]) {
			[flat addObjectsFromArray:[(AUParameterGroup *)child allParameters]];
		} else {
			[flat addObject:child];
		}
	}
	_allParameters = [[NSArray alloc] initWithArray:flat];
	[flat release];

	return _allParameters;
}

@end

@implementation AUParameterTree

- (AUParameter *)parameterWithAddress:(AUParameterAddress)address
{
	for (AUParameter *parameter in [self allParameters]) {
		if ([parameter address] == address) {
			return parameter;
		}
	}
	return nil;
}

- (AUParameter *)parameterWithID:(AudioUnitParameterID)paramID
                           scope:(AudioUnitScope)scope
                         element:(AudioUnitElement)element
{
	return [self parameterWithAddress:fn_agora_au_address(scope, element, paramID)];
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUParameter
 * ---------------------------------------------------------------------------------------------- */

@interface AUParameter ()
{
	AUValue _minValue;
	AUValue _maxValue;
	AudioUnitParameterUnit _unit;
	NSString *_unitName;
	AudioUnitParameterOptions _flags;
	AUParameterAddress _address;
	NSArray *_valueStrings;
	NSArray *_dependentParameters;
	AUValue _value;
}
- (instancetype)initWithAddress:(AUParameterAddress)address
                     identifier:(NSString *)identifier
                           name:(NSString *)displayName
                            min:(AUValue)min
                            max:(AUValue)max
                           unit:(AudioUnitParameterUnit)unit
                       unitName:(NSString *)unitName
                          flags:(AudioUnitParameterOptions)flags
                   valueStrings:(NSArray *)valueStrings
            dependentParameters:(NSArray *)dependentParameters;
@end

@implementation AUParameter

- (instancetype)initWithAddress:(AUParameterAddress)address
                     identifier:(NSString *)identifier
                           name:(NSString *)displayName
                            min:(AUValue)min
                            max:(AUValue)max
                           unit:(AudioUnitParameterUnit)unit
                       unitName:(NSString *)unitName
                          flags:(AudioUnitParameterOptions)flags
                   valueStrings:(NSArray *)valueStrings
            dependentParameters:(NSArray *)dependentParameters
{
	if ((self = [super initWithIdentifier:identifier name:displayName])) {
		_address = address;
		_minValue = min;
		_maxValue = max;
		_unit = unit;
		_unitName = [unitName copy];
		_flags = flags;
		_valueStrings = [valueStrings copy];
		_dependentParameters = [dependentParameters copy];
		_value = min;
	}
	return self;
}

- (void)dealloc
{
	[_unitName release];
	[_valueStrings release];
	[_dependentParameters release];
	[super dealloc];
}

- (AUValue)minValue { return _minValue; }
- (AUValue)maxValue { return _maxValue; }
- (AudioUnitParameterUnit)unit { return _unit; }
- (NSString *)unitName { return _unitName; }
- (AudioUnitParameterOptions)flags { return _flags; }
- (AUParameterAddress)address { return _address; }
- (NSArray *)valueStrings { return _valueStrings; }
- (NSArray *)dependentParameters { return _dependentParameters; }

- (AUValue)value
{
	if (self.implementorValueProvider != NULL) {
		return self.implementorValueProvider(self);
	}
	return _value;
}

- (void)setValue:(AUValue)value
{
	[self setValue:value originator:NULL atHostTime:0];
}

- (void)setValue:(AUValue)value originator:(AUParameterObserverToken)originator
{
	[self setValue:value originator:originator atHostTime:0];
}

- (void)setValue:(AUValue)value
       originator:(AUParameterObserverToken)originator
       atHostTime:(uint64_t)hostTime
{
	[self setValue:value originator:originator atHostTime:hostTime
	      eventType:AUParameterAutomationEventTypeValue];
}

- (void)setValue:(AUValue)value
       originator:(AUParameterObserverToken)originator
       atHostTime:(uint64_t)hostTime
        eventType:(AUParameterAutomationEventType)eventType
{
	AUValue clamped = value;
	AUValue applied;

	(void)eventType;	/* a v2 unit has one value and no automation timeline: every write is a value */

	if (clamped < _minValue) {
		clamped = _minValue;
	}
	if (clamped > _maxValue) {
		clamped = _maxValue;
	}

	if (self.implementorValueObserver != NULL) {
		self.implementorValueObserver(self, clamped);
	} else {
		_value = clamped;
	}

	/* What the unit did, not what was asked: a provider that clamps is the authority. */
	applied = [self value];
	[self fn_notifyAddress:_address value:applied hostTime:hostTime originator:originator];
}

- (NSString *)stringFromValue:(const AUValue *)value
{
	AUValue shown = value != NULL ? *value : [self value];

	if (self.implementorStringFromValueCallback != NULL) {
		return self.implementorStringFromValueCallback(self, value);
	}
	return [NSString stringWithFormat:@"%g", (double)shown];
}

- (AUValue)valueFromString:(NSString *)string
{
	if (self.implementorValueFromStringCallback != NULL) {
		return self.implementorValueFromStringCallback(self, string);
	}
	return (AUValue)[string doubleValue];
}

@end

/* ------------------------------------------------------------------------------------------------
 * The factory
 * ---------------------------------------------------------------------------------------------- */

@implementation AUParameterTree (Factory)

+ (AUParameter *)createParameterWithIdentifier:(NSString *)identifier
                                          name:(NSString *)name
                                       address:(AUParameterAddress)address
                                           min:(AUValue)min
                                           max:(AUValue)max
                                          unit:(AudioUnitParameterUnit)unit
                                      unitName:(NSString *)unitName
                                         flags:(AudioUnitParameterOptions)flags
                                  valueStrings:(NSArray *)valueStrings
                           dependentParameters:(NSArray *)dependentParameters
{
	AUParameter *parameter = [[AUParameter alloc] initWithAddress:address
	                                                   identifier:identifier
	                                                         name:name
	                                                          min:min
	                                                          max:max
	                                                         unit:unit
	                                                     unitName:unitName
	                                                        flags:flags
	                                                 valueStrings:valueStrings
	                                          dependentParameters:dependentParameters];
	return [parameter autorelease];
}

+ (AUParameterGroup *)createGroupWithIdentifier:(NSString *)identifier
                                           name:(NSString *)name
                                       children:(NSArray *)children
{
	AUParameterGroup *group = [[AUParameterGroup alloc] initWithIdentifier:identifier
	                                                                name:name
	                                                            children:children];
	return [group autorelease];
}

+ (AUParameterTree *)createTreeWithChildren:(NSArray *)children
{
	AUParameterTree *tree = [[AUParameterTree alloc] initWithIdentifier:@""
	                                                              name:@"Root"
	                                                          children:children];
	return [tree autorelease];
}

@end
