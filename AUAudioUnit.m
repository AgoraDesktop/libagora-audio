/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AUAudioUnit.m — the object face of a C unit, and the busses, blocks and tree it hands out.
 *
 * THIS FILE IMPLEMENTS NOTHING TWICE. Every door here is one C door with a nicer signature: `-inputBusses` is
 * `kAudioUnitProperty_ElementCount` on the input scope, `-renderBlock` is `AudioUnitRender`,
 * `-startHardwareAndReturnError:` is `AudioOutputUnitStart`, `-parameterTree` is
 * `kAudioUnitProperty_ParameterList` plus `kAudioUnitProperty_ParameterInfo`. That is deliberate and it is
 * Apple's own answer for a v2 unit: the object is a view, and a view that keeps its own state is a second
 * implementation waiting to disagree.
 *
 * WHY THE RENDER BLOCK CAPTURES THE INSTANCE AND NOT `self`: the unit owns the block and the block would
 * otherwise own the unit. A block that captured `self` would be a cycle with no way out in a manual-retain
 * world, so it captures the one thing it needs — the `AudioUnit` pointer — and calls the C door directly.
 *
 * MANUAL OWNERSHIP throughout, like `libagora-app`: retained on store, released in `-dealloc`, and blocks that
 * arrive on the stack are `Block_copy`'d because the caller's frame will not outlive the registration.
 *
 * SPECIFIED BY macOS 10.12 except where `AUAudioUnit.h` says otherwise (a bus's format is an
 * `AudioStreamBasicDescription`; there is no out-of-process instantiation).
 */

#import <AudioToolbox/AUAudioUnit.h>
#import <AudioToolbox/AUAudioUnit_internal.h>
#import <AudioToolbox/AudioOutputUnit.h>

#include <Block.h>

static NSString *const AGORA_AUDIOUNIT_ERROR_DOMAIN = @"AgoraAudioUnit";

static NSError *fn_error(NSInteger code, NSString *message)
{
	NSDictionary *info = [NSDictionary dictionaryWithObject:message forKey:@"NSLocalizedDescription"];

	return [NSError errorWithDomain:AGORA_AUDIOUNIT_ERROR_DOMAIN code:code userInfo:info];
}

static AudioUnitScope fn_bus_scope(AUAudioUnitBusType type)
{
	return (type == AUAudioUnitBusTypeInput) ? kAudioUnitScope_Input : kAudioUnitScope_Output;
}

/* One registered render observer. Wrapped so the block has exactly one owner that releases it. */
@interface AURenderObserverBox : NSObject
{
@public
	AURenderObserver _observer;	/* Block_copy'd */
}
@end

@implementation AURenderObserverBox
- (void)dealloc
{
	Block_release(_observer);
	[super dealloc];
}
@end

/* ------------------------------------------------------------------------------------------------
 * AUAudioUnit
 * ---------------------------------------------------------------------------------------------- */

@interface AUAudioUnit ()
{
	AudioComponent _component;	/* borrowed from the registry */
	AudioComponentDescription _description;
	AudioComponentInstantiationOptions _options;
	AudioUnit _instance;	/* owned: disposed in -dealloc */
	BOOL _allocated;

	AUAudioUnitBusArray *_inputBusses;
	AUAudioUnitBusArray *_outputBusses;
	AUParameterTree *_parameterTree;

	AURenderBlock _renderBlock;
	AUScheduleParameterBlock _scheduleParameterBlock;
	NSMutableDictionary *_renderObservers;	/* NSNumber(token) -> AURenderObserverBox */
	NSInteger _nextRenderToken;

	AURenderPullInputBlock _outputProvider;
	AUInputHandler _inputHandler;
	AUHostMusicalContextBlock _musicalContextBlock;
	AUHostTransportStateBlock _transportStateBlock;
	NSString *_contextName;
	AUAudioUnitPreset *_currentPreset;

	BOOL _inputEnabled;
	BOOL _outputEnabled;
	BOOL _bypass;
	BOOL _offline;
	NSInteger _renderQuality;
}
- (AudioUnit)agora_instance;
- (BOOL)agora_allocated;
- (AUAudioUnitBusArray *)fn_bussesOfType:(AUAudioUnitBusType)type;
- (void)fn_invalidateParameterTree;
@end

@implementation AUAudioUnit

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
{
	if ((self = [super init])) {
		AudioComponent component;
		AudioUnit instance = NULL;
		OSStatus status;

		component = AudioComponentFindNext(NULL, &componentDescription);
		if (component == NULL) {
			if (outError != NULL) {
				*outError = fn_error(-10879 /* kAudioUnitErr_InvalidProperty */,
				                     @"No registered component matches that description");
			}
			[self release];
			return nil;
		}

		status = AudioComponentInstanceNew(component, &instance);
		if (status != 0) {
			if (outError != NULL) {
				*outError = fn_error(status, @"AudioComponentInstanceNew failed");
			}
			[self release];
			return nil;
		}

		_component = component;
		_description = componentDescription;
		_options = options;
		_instance = instance;
		_renderObservers = [[NSMutableDictionary alloc] init];
		_inputEnabled = YES;
		_outputEnabled = YES;

		{
			AudioUnit unit = instance;
			NSMutableDictionary *observers = _renderObservers;

			/* The pull, once. The observers are called from *inside* it, before the unit renders, which is
			 * the order Apple documents and the only order in which a parameter change is in time to be
			 * heard in the same buffer. The block holds the dictionary without retaining it: both belong to
			 * this object and are released together in `-dealloc`. */
			_renderBlock = Block_copy(^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
			                                             const AudioTimeStamp *timestamp,
			                                             AUAudioFrameCount frameCount,
			                                             NSInteger outputBusNumber,
			                                             AudioBufferList *outputData,
			                                             AURenderPullInputBlock pullInputBlock) {
				NSEnumerator *boxes;
				AURenderObserverBox *box;
				AudioUnitRenderActionFlags flags = actionFlags != NULL ? *actionFlags : 0;

				(void)pullInputBlock;	/* a built-in bus is fed by its AURenderCallbackStruct, not by this */

				boxes = [observers objectEnumerator];
				while ((box = [boxes nextObject]) != nil) {
					box->_observer(flags, timestamp, frameCount, outputBusNumber);
				}

				return AudioUnitRender(unit, actionFlags, timestamp, (UInt32)outputBusNumber,
				                       frameCount, outputData);
			});
			_scheduleParameterBlock = Block_copy(^void(AUEventSampleTime eventSampleTime,
			                                           AUAudioFrameCount rampDurationSampleFrames,
			                                           AUParameterAddress address,
			                                           AUValue value) {
				/* A v2 unit has now and a value: the time and the ramp are the host's, and this library
				 * has no scheduled-parameter timeline (see the header). */
				(void)eventSampleTime;
				(void)rampDurationSampleFrames;
				AudioUnitSetParameter(unit,
				                      fn_agora_au_address_id(address),
				                      fn_agora_au_address_scope(address),
				                      fn_agora_au_address_element(address), value, 0);
			});
		}
	}
	return self;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                       error:(NSError **)outError
{
	return [self initWithComponentDescription:componentDescription options:0 error:outError];
}

- (void)dealloc
{
	if (_instance != NULL) {
		if (_allocated) {
			AudioUnitUninitialize(_instance);
		}
		AudioComponentInstanceDispose(_instance);
	}
	[_inputBusses release];
	[_outputBusses release];
	[_parameterTree release];
	[_renderObservers release];
	[_currentPreset release];
	[_contextName release];
	if (_renderBlock != NULL) { Block_release(_renderBlock); }
	if (_scheduleParameterBlock != NULL) { Block_release(_scheduleParameterBlock); }
	if (_outputProvider != NULL) { Block_release(_outputProvider); }
	if (_inputHandler != NULL) { Block_release(_inputHandler); }
	if (_musicalContextBlock != NULL) { Block_release(_musicalContextBlock); }
	if (_transportStateBlock != NULL) { Block_release(_transportStateBlock); }
	[super dealloc];
}

+ (void)instantiateWithComponentDescription:(AudioComponentDescription)componentDescription
                                    options:(AudioComponentInstantiationOptions)options
                          completionHandler:(void (^)(AUAudioUnit *, NSError *))completionHandler
{
	if (completionHandler == NULL) {
		return;
	}
	/* Refused by name: the registry is three built-ins compiled into this library and there is no plug-in
	 * ABI, so an out-of-process instantiation has nothing to find. The handler is still called — a caller
	 * that waits for it must not wait forever. */
	completionHandler(nil, fn_error(-10879 /* kAudioUnitErr_InvalidProperty */,
	                                @"Out-of-process instantiation is not supported: the component "
	                                @"registry holds three built-in units and no plug-in ABI"));
}

- (AudioComponentDescription)componentDescription { return _description; }
- (AudioComponent)component { return _component; }
- (uint32_t)componentVersion { return 0; }

- (NSString *)componentName
{
	CFStringRef name = NULL;

	if (AudioComponentCopyName(_component, &name) != 0 || name == NULL) {
		return nil;
	}
	return [(NSString *)name autorelease];
}

- (NSString *)audioUnitName { return [self componentName]; }

- (NSString *)manufacturerName
{
	return [NSString stringWithFormat:@"%c%c%c%c",
	                (char)((_description.componentManufacturer >> 24) & 0xff),
	                (char)((_description.componentManufacturer >> 16) & 0xff),
	                (char)((_description.componentManufacturer >> 8) & 0xff),
	                (char)(_description.componentManufacturer & 0xff)];
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError
{
	OSStatus status;

	status = AudioUnitInitialize(_instance);
	if (status != 0) {
		if (outError != NULL) {
			*outError = fn_error(status, @"AudioUnitInitialize failed");
		}
		return NO;
	}
	_allocated = YES;
	return YES;
}

- (void)deallocateRenderResources
{
	if (!_allocated) {
		return;
	}
	AudioUnitUninitialize(_instance);
	_allocated = NO;
}

- (BOOL)renderResourcesAllocated { return _allocated; }

- (void)reset
{
	AudioUnitReset(_instance, kAudioUnitScope_Global, 0);
}

- (AudioUnit)agora_instance { return _instance; }
- (BOOL)agora_allocated { return _allocated; }

/* --- busses ----------------------------------------------------------------- */

- (AUAudioUnitBusArray *)fn_bussesOfType:(AUAudioUnitBusType)type
{
	if (type == AUAudioUnitBusTypeInput) {
		if (_inputBusses == nil) {
			_inputBusses = [[AUAudioUnitBusArray alloc] agora_initWithOwner:self busType:type];
		}
		return _inputBusses;
	}
	if (_outputBusses == nil) {
		_outputBusses = [[AUAudioUnitBusArray alloc] agora_initWithOwner:self busType:type];
	}
	return _outputBusses;
}

- (AUAudioUnitBusArray *)inputBusses { return [self fn_bussesOfType:AUAudioUnitBusTypeInput]; }
- (AUAudioUnitBusArray *)outputBusses { return [self fn_bussesOfType:AUAudioUnitBusTypeOutput]; }

- (AURenderBlock)renderBlock { return _renderBlock; }
- (AUScheduleParameterBlock)scheduleParameterBlock { return _scheduleParameterBlock; }

- (AUAudioFrameCount)maximumFramesToRender
{
	UInt32 value = 0;
	UInt32 size = sizeof(value);

	if (AudioUnitGetProperty(_instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
	                         &value, &size) != 0) {
		return 0;
	}
	return value;
}

- (void)setMaximumFramesToRender:(AUAudioFrameCount)maximumFramesToRender
{
	UInt32 value = maximumFramesToRender;

	AudioUnitSetProperty(_instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
	                     &value, sizeof(value));
}

- (NSInteger)tokenByAddingRenderObserver:(AURenderObserver)observer
{
	NSInteger token = ++_nextRenderToken;
	NSNumber *key = [NSNumber numberWithLong:(long)token];
	AURenderObserverBox *box = [[AURenderObserverBox alloc] init];

	box->_observer = Block_copy(observer);
	[_renderObservers setObject:box forKey:key];
	[box release];

	return token;
}

- (void)removeRenderObserver:(NSInteger)token
{
	[_renderObservers removeObjectForKey:[NSNumber numberWithLong:(long)token]];
}

/* --- the parameter tree ----------------------------------------------------- */

- (void)fn_invalidateParameterTree
{
	[_parameterTree release];
	_parameterTree = nil;
}

- (AUParameterTree *)parameterTree
{
	NSMutableArray *groups;
	AudioUnitScope scope;

	if (_parameterTree != nil) {
		return _parameterTree;
	}

	groups = [[NSMutableArray alloc] init];

	/* The scopes a parameter can live on, in Apple's order. An empty scope contributes no group. */
	for (scope = kAudioUnitScope_Global; scope <= kAudioUnitScope_Output; scope++) {
		AudioUnitParameterID ids[64];
		UInt32 size = 0;
		UInt32 count;
		UInt32 element;
		NSMutableArray *parameters;
		UInt32 elements = 0;

		if (AudioUnitGetPropertyInfo(_instance, kAudioUnitProperty_ParameterList, scope, 0, &size, NULL) != 0
		    || size == 0) {
			continue;
		}
		count = size / (UInt32)sizeof(AudioUnitParameterID);
		if (count > (UInt32)(sizeof(ids) / sizeof(ids[0]))) {
			count = (UInt32)(sizeof(ids) / sizeof(ids[0]));
		}
		if (AudioUnitGetProperty(_instance, kAudioUnitProperty_ParameterList, scope, 0, ids, &size) != 0) {
			continue;
		}

		/* A parameter's *value* is per element and its *description* is not, so the nodes are per
		 * (parameter, element): that is exactly the coordinate an address carries. Only the input scope has
		 * more than one element here — the element count in this library counts a mixer's buses. */
		elements = 1;
		if (scope == kAudioUnitScope_Input) {
			UInt32 elements_size = sizeof(elements);

			if (AudioUnitGetProperty(_instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Global, 0,
			                         &elements, &elements_size) != 0 || elements == 0) {
				elements = 1;
			}
		}

		parameters = [[NSMutableArray alloc] init];
		for (element = 0; element < elements; element++) {
			UInt32 index;

			for (index = 0; index < count; index++) {
				AudioUnitParameterInfo info;
				UInt32 info_size = sizeof(info);
				AUParameter *parameter;
				AudioUnit unit = _instance;
				AudioUnitParameterID param_id = ids[index];
				AudioUnitElement slot = element;
				AudioUnitScope node_scope = scope;

				if (AudioUnitGetProperty(_instance, kAudioUnitProperty_ParameterInfo, scope, param_id, &info,
				                         &info_size) != 0) {
					continue;
				}

				parameter = [AUParameterTree createParameterWithIdentifier:
				                    [NSString stringWithFormat:@"%s_%u", info.name, (unsigned)element]
				                                                      name:[NSString stringWithUTF8String:info.name]
				                                                   address:fn_agora_au_address(scope, element, param_id)
				                                                       min:info.minValue
				                                                       max:info.maxValue
				                                                      unit:info.unit
				                                                  unitName:nil
				                                                     flags:info.flags
				                                              valueStrings:nil
				                                       dependentParameters:nil];

				parameter.implementorValueObserver = ^(AUParameter *node, AUValue value) {
					(void)node;
					AudioUnitSetParameter(unit, param_id, node_scope, slot, value, 0);
				};
				parameter.implementorValueProvider = ^AUValue(AUParameter *node) {
					AudioUnitParameterValue value = 0;

					(void)node;
					AudioUnitGetParameter(unit, param_id, node_scope, slot, &value);
					return value;
				};
				[parameters addObject:parameter];
			}
		}

		if ([parameters count] > 0) {
			static const char *names[] = { "Global", "Input", "Output" };
			AUParameterGroup *group = [AUParameterTree createGroupWithIdentifier:
			                                  [NSString stringWithUTF8String:names[scope]]
			                                                          name:[NSString stringWithUTF8String:names[scope]]
			                                                      children:parameters];

			[groups addObject:group];
		}
		[parameters release];
	}

	_parameterTree = [[AUParameterTree createTreeWithChildren:groups] retain];
	[groups release];

	return _parameterTree;
}

- (BOOL)allParameterValues { return YES; }
- (BOOL)isMusicDeviceOrEffect { return NO; }
- (NSInteger)virtualMIDICableCount { return 0; }
- (AUScheduleMIDIEventBlock)scheduleMIDIEventBlock { return nil; }

/* --- the optional doors ----------------------------------------------------- */
/* Read-only and named: this phase's units have no state dictionary, no presets and no bypass switch, and a
 * door that returned an empty dictionary would be a lie a host could archive. */

- (NSDictionary *)fullState { return nil; }
- (void)setFullState:(NSDictionary *)state { (void)state; }
- (NSDictionary *)fullStateForDocument { return nil; }
- (void)setFullStateForDocument:(NSDictionary *)state { (void)state; }
- (NSArray *)factoryPresets { return nil; }
- (NSTimeInterval)latency { return 0.0; }
- (NSTimeInterval)tailTime { return 0.0; }
- (NSInteger)renderQuality { return _renderQuality; }
- (void)setRenderQuality:(NSInteger)quality { _renderQuality = quality; }
- (BOOL)shouldBypassEffect { return _bypass; }
- (void)setShouldBypassEffect:(BOOL)bypass { _bypass = bypass; }
- (BOOL)canProcessInPlace { return NO; }
- (BOOL)isRenderingOffline { return _offline; }
- (void)setRenderingOffline:(BOOL)offline { _offline = offline; }
- (NSArray *)channelCapabilities { return nil; }
- (BOOL)supportsMPE { return NO; }
- (NSArray *)channelMap { return nil; }
- (void)setChannelMap:(NSArray *)map { (void)map; }

- (AUAudioUnitPreset *)currentPreset { return _currentPreset; }

- (void)setCurrentPreset:(AUAudioUnitPreset *)preset
{
	if (_currentPreset != preset) {
		[_currentPreset release];
		_currentPreset = [preset retain];
	}
}

- (NSString *)contextName { return _contextName; }

- (void)setContextName:(NSString *)name
{
	if (_contextName != name) {
		[_contextName release];
		_contextName = [name copy];
	}
}

- (AUHostMusicalContextBlock)musicalContextBlock { return _musicalContextBlock; }

- (void)setMusicalContextBlock:(AUHostMusicalContextBlock)block
{
	_musicalContextBlock = block != NULL ? Block_copy(block) : NULL;
}

- (AUHostTransportStateBlock)transportStateBlock { return _transportStateBlock; }

- (void)setTransportStateBlock:(AUHostTransportStateBlock)block
{
	_transportStateBlock = block != NULL ? Block_copy(block) : NULL;
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUAudioUnit (AUAudioInputOutputUnit)
 * ---------------------------------------------------------------------------------------------- */

@implementation AUAudioUnit (AUAudioInputOutputUnit)

- (BOOL)canPerformInput { return NO; }
- (BOOL)canPerformOutput { return YES; }
- (BOOL)isInputEnabled { return _inputEnabled; }
- (void)setInputEnabled:(BOOL)enabled { _inputEnabled = enabled; }
- (BOOL)isOutputEnabled { return _outputEnabled; }
- (void)setOutputEnabled:(BOOL)enabled { _outputEnabled = enabled; }
- (AURenderPullInputBlock)outputProvider { return _outputProvider; }

- (void)setOutputProvider:(AURenderPullInputBlock)provider
{
	_outputProvider = provider != NULL ? Block_copy(provider) : NULL;
}

- (AUInputHandler)inputHandler { return _inputHandler; }

- (void)setInputHandler:(AUInputHandler)handler
{
	_inputHandler = handler != NULL ? Block_copy(handler) : NULL;
}

- (AudioObjectID)deviceID
{
	AudioDeviceID device = kAudioObjectUnknown;
	UInt32 size = sizeof(device);

	AudioUnitGetProperty([self agora_instance], kAudioUnitProperty_CurrentPlaybackDevice, kAudioUnitScope_Global,
	                     0, &device, &size);
	return device;
}

- (BOOL)setDeviceID:(AudioObjectID)deviceID error:(NSError **)outError
{
	AudioDeviceID device = deviceID;
	OSStatus status = AudioUnitSetProperty([self agora_instance], kAudioUnitProperty_CurrentPlaybackDevice,
	                                       kAudioUnitScope_Global, 0, &device, sizeof(device));

	if (status != 0) {
		if (outError != NULL) {
			*outError = fn_error(status, @"AudioUnitSetProperty(CurrentPlaybackDevice) failed");
		}
		return NO;
	}
	return YES;
}

- (BOOL)startHardwareAndReturnError:(NSError **)outError
{
	OSStatus status = AudioOutputUnitStart([self agora_instance]);

	if (status != 0) {
		if (outError != NULL) {
			*outError = fn_error(status, @"AudioOutputUnitStart failed");
		}
		return NO;
	}
	return YES;
}

- (void)stopHardware
{
	AudioOutputUnitStop([self agora_instance]);
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUAudioUnitBusArray
 * ---------------------------------------------------------------------------------------------- */

@interface AUAudioUnitBusArray ()
{
	AUAudioUnit *_owner;	/* not retained: the unit owns the array */
	AUAudioUnitBusType _busType;
	NSMutableArray *_busses;
}
- (void)agora_reload;
@end

@implementation AUAudioUnitBusArray

- (instancetype)initWithAudioUnit:(AUAudioUnit *)owner
                          busType:(AUAudioUnitBusType)busType
                           busses:(NSArray *)busArray
{
	if ((self = [super init])) {
		_owner = owner;
		_busType = busType;
		_busses = [[NSMutableArray alloc] initWithArray:busArray];
	}
	return self;
}

- (instancetype)initWithAudioUnit:(AUAudioUnit *)owner busType:(AUAudioUnitBusType)busType
{
	if ((self = [self initWithAudioUnit:owner busType:busType busses:[NSArray array]])) {
		[self agora_reload];
	}
	return self;
}

- (instancetype)agora_initWithOwner:(AUAudioUnit *)owner busType:(AUAudioUnitBusType)busType
{
	return [self initWithAudioUnit:owner busType:busType];
}

- (void)dealloc
{
	[_busses release];
	[super dealloc];
}

- (void)agora_reload
{
	UInt32 count = 1;	/* an output bus is the device: there is one */
	UInt32 i;

	if (_busType == AUAudioUnitBusTypeInput) {
		UInt32 size = sizeof(count);

		if (AudioUnitGetProperty([_owner agora_instance], kAudioUnitProperty_ElementCount,
		                         kAudioUnitScope_Global, 0, &count, &size) != 0) {
			count = 1;
		}
	}

	[_busses removeAllObjects];
	for (i = 0; i < count; i++) {
		AUAudioUnitBus *bus = [[AUAudioUnitBus alloc] agora_initWithOwner:_owner busType:_busType index:i];

		[_busses addObject:bus];
		[bus release];
	}
}

- (NSUInteger)count { return [_busses count]; }

- (AUAudioUnitBus *)objectAtIndexedSubscript:(NSUInteger)index
{
	return [_busses objectAtIndex:index];
}

- (BOOL)isCountChangeable
{
	UInt32 size = 0;
	Boolean writable = false;

	if (_busType != AUAudioUnitBusTypeInput) {
		return NO;
	}
	if (AudioUnitGetPropertyInfo([_owner agora_instance], kAudioUnitProperty_ElementCount,
	                             kAudioUnitScope_Global, 0, &size, &writable) != 0) {
		return NO;
	}
	return writable ? YES : NO;
}

- (BOOL)setBusCount:(NSUInteger)count error:(NSError **)outError
{
	UInt32 value = (UInt32)count;
	OSStatus status;

	if (_busType != AUAudioUnitBusTypeInput) {
		if (outError != NULL) {
			*outError = fn_error(-10851 /* kAudioUnitErr_InvalidPropertyValue */,
			                     @"A unit's output bus count is the device's, not the caller's");
		}
		return NO;
	}

	status = AudioUnitSetProperty([_owner agora_instance], kAudioUnitProperty_ElementCount,
	                              kAudioUnitScope_Global, 0, &value, sizeof(value));
	if (status != 0) {
		if (outError != NULL) {
			*outError = fn_error(status, @"the element count was refused");
		}
		return NO;
	}

	[self agora_reload];
	[_owner fn_invalidateParameterTree];
	return YES;
}

- (void)addObserverToAllBusses:(NSObject *)observer
                    forKeyPath:(NSString *)keyPath
                       options:(NSKeyValueObservingOptions)options
                       context:(void *)context
{
	for (AUAudioUnitBus *bus in _busses) {
		[bus addObserver:observer forKeyPath:keyPath options:options context:context];
	}
}

- (void)removeObserverFromAllBusses:(NSObject *)observer
                         forKeyPath:(NSString *)keyPath
                            context:(void *)context
{
	for (AUAudioUnitBus *bus in _busses) {
		[bus removeObserver:observer forKeyPath:keyPath context:context];
	}
}

- (AUAudioUnit *)ownerAudioUnit { return _owner; }
- (AUAudioUnitBusType)busType { return _busType; }

- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                  objects:(id *)buffer
                                    count:(NSUInteger)length
{
	return [_busses countByEnumeratingWithState:state objects:buffer count:length];
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUAudioUnitBus
 * ---------------------------------------------------------------------------------------------- */

@interface AUAudioUnitBus ()
{
	AUAudioUnit *_owner;	/* not retained: the array owns the bus and the unit owns the array */
	AUAudioUnitBusType _busType;
	NSUInteger _index;
	BOOL _enabled;
	NSString *_name;
	NSTimeInterval _contextPresentationLatency;
}
@end

@implementation AUAudioUnitBus

- (instancetype)agora_initWithOwner:(AUAudioUnit *)owner
                            busType:(AUAudioUnitBusType)busType
                              index:(NSUInteger)index
{
	if ((self = [super init])) {
		_owner = owner;
		_busType = busType;
		_index = index;
		_enabled = YES;
	}
	return self;
}

- (void)dealloc
{
	[_name release];
	[super dealloc];
}

- (AudioStreamBasicDescription)format
{
	AudioStreamBasicDescription format;
	UInt32 size = sizeof(format);

	memset(&format, 0, sizeof(format));
	AudioUnitGetProperty([_owner agora_instance], kAudioUnitProperty_StreamFormat,
	                     fn_bus_scope(_busType), (AudioUnitElement)_index, &format, &size);
	return format;
}

- (BOOL)setFormat:(AudioStreamBasicDescription)format error:(NSError **)outError
{
	AudioStreamBasicDescription copy = format;
	OSStatus status = AudioUnitSetProperty([_owner agora_instance], kAudioUnitProperty_StreamFormat,
	                                       fn_bus_scope(_busType), (AudioUnitElement)_index,
	                                       &copy, sizeof(copy));

	if (status != 0) {
		if (outError != NULL) {
			*outError = fn_error(status, @"the bus's format was refused");
		}
		return NO;
	}
	return YES;
}

- (BOOL)isEnabled { return _enabled; }
- (void)setEnabled:(BOOL)enabled { _enabled = enabled; }
- (NSString *)name { return _name; }

- (void)setName:(NSString *)name
{
	if (_name != name) {
		[_name release];
		_name = [name copy];
	}
}

- (NSUInteger)index { return _index; }
- (AUAudioUnitBusType)busType { return _busType; }
- (AUAudioUnit *)ownerAudioUnit { return _owner; }
- (NSArray *)supportedChannelLayoutTags { return nil; }
- (NSTimeInterval)contextPresentationLatency { return _contextPresentationLatency; }

- (void)setContextPresentationLatency:(NSTimeInterval)latency
{
	_contextPresentationLatency = latency;
}

@end

/* ------------------------------------------------------------------------------------------------
 * AUAudioUnitPreset
 * ---------------------------------------------------------------------------------------------- */

@interface AUAudioUnitPreset ()
{
	NSInteger _number;
	NSString *_name;
}
@end

@implementation AUAudioUnitPreset

- (void)dealloc
{
	[_name release];
	[super dealloc];
}

- (NSInteger)number { return _number; }
- (void)setNumber:(NSInteger)number { _number = number; }
- (NSString *)name { return _name; }

- (void)setName:(NSString *)name
{
	if (_name != name) {
		[_name release];
		_name = [name copy];
	}
}

@end
