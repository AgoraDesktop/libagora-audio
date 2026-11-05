/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AUAudioUnit.h — a unit as an object: busses, a render block, and the hardware doors.
 *
 * THE SAME INSTANCE, TWO FACES, WHICH IS APPLE'S SPLIT AND IS DELIBERATELY NOT A SECOND IMPLEMENTATION: an
 * `AUAudioUnit` wraps a C `AudioUnit` that already exists. Everything here is expressed through the C doors —
 * `inputBusses` is the element count on the input scope, `renderBlock` is `AudioUnitRender`, `startHardware`
 * is `AudioOutputUnitStart` — so the object cannot drift from the thing it describes. On macOS the same
 * relationship is called `AUAudioUnitV2Bridge`.
 *
 * WHAT THIS DOOR ADDS is the vocabulary an application actually wants: a *bus* with a format and an enabled
 * flag, a *render block* it can call with its own timestamp, and a parameter *tree* instead of four
 * coordinates passed to a function. `AudioUnitRender` is still what happens; it is simply called for you.
 *
 * ONE DECLARATION IS NOT APPLE'S: a bus's format is an `AudioStreamBasicDescription`, not an
 * `AVAudioFormat`. Formats live in AVFoundation on macOS, this library has no AVFoundation (AUDIO-PLAN §5), and
 * an opaque forward declaration would be a name a client could not construct. The C description is the same
 * fact the unit's `kAudioUnitProperty_StreamFormat` holds, so a bus is a bus. The README says so too.
 *
 * `NSSecureCoding` IS NOT CLAIMED on `AUAudioUnitPreset` either, for the same reason the parameter classes do
 * not claim it: there is no coder here, and a conformance without `-encodeWithCoder:` is a promise the class
 * cannot keep.
 *
 * OUT OF SCOPE, AND REFUSED RATHER THAN ABSENT: third-party units are not loadable here (the component
 * registry is three built-ins and there is no plug-in ABI), so `+instantiateWithComponentDescription:…`
 * reports an error instead of pretending, and the app-extension protocol `AUAudioUnitFactory` is not
 * declared — a class that cannot be loaded is worse than a name that does not exist.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_H
#define AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_H

#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AUParameters.h>
#include <CoreAudio/AudioHardwareBase.h>

#if defined(__OBJC__)

#include <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class AUAudioUnitBus;
@class AUAudioUnitBusArray;
@class AUAudioUnitPreset;

typedef OSStatus AUAudioUnitStatus;
typedef int64_t  AUEventSampleTime;
typedef uint32_t AUAudioFrameCount;
typedef uint32_t AUAudioChannelCount;

enum : AUEventSampleTime
{
	AUEventSampleTimeImmediate = (AUEventSampleTime)0xffffffff00000000LL
};

typedef NS_ENUM(NSInteger, AUAudioUnitBusType)
{
	AUAudioUnitBusTypeInput  = 1,
	AUAudioUnitBusTypeOutput = 2
};

typedef NS_OPTIONS(NSUInteger, AUHostTransportStateFlags)
{
	AUHostTransportStateChanged   = 1,
	AUHostTransportStateMoving    = 2,
	AUHostTransportStateRecording = 4,
	AUHostTransportStateCycling   = 8
};

/* The pull, in the two directions a block can face. A render block is handed a `pullInputBlock` and calls it
 * for each input it needs — which is the same shape as `AURenderCallback` with the arguments already
 * unpacked, and is why an output unit needs no clock of its own. */
typedef AUAudioUnitStatus (^AURenderPullInputBlock)(AudioUnitRenderActionFlags *actionFlags,
                                                    const AudioTimeStamp *timestamp,
                                                    AUAudioFrameCount frameCount,
                                                    NSInteger inputBusNumber,
                                                    AudioBufferList *inputData);
typedef AUAudioUnitStatus (^AURenderBlock)(AudioUnitRenderActionFlags *actionFlags,
                                           const AudioTimeStamp *timestamp,
                                           AUAudioFrameCount frameCount,
                                           NSInteger outputBusNumber,
                                           AudioBufferList *outputData,
                                           AURenderPullInputBlock _Nullable pullInputBlock);
typedef void (^AURenderObserver)(AudioUnitRenderActionFlags actionFlags,
                                 const AudioTimeStamp *timestamp,
                                 AUAudioFrameCount frameCount,
                                 NSInteger outputBusNumber);
typedef void (^AUScheduleParameterBlock)(AUEventSampleTime eventSampleTime,
                                         AUAudioFrameCount rampDurationSampleFrames,
                                         AUParameterAddress parameterAddress,
                                         AUValue value);
typedef void (^AUScheduleMIDIEventBlock)(AUEventSampleTime eventSampleTime,
                                         uint8_t cable,
                                         NSInteger length,
                                         const uint8_t *midiBytes);
typedef BOOL (^AUHostMusicalContextBlock)(double * _Nullable currentTempo,
                                          double * _Nullable timeSignatureNumerator,
                                          NSInteger * _Nullable timeSignatureDenominator,
                                          double * _Nullable currentBeatPosition,
                                          NSInteger * _Nullable sampleOffsetToNextBeat,
                                          double * _Nullable currentMeasureDownbeatPosition);
typedef BOOL (^AUHostTransportStateBlock)(AUHostTransportStateFlags * _Nullable transportStateFlags,
                                          double * _Nullable currentSamplePosition,
                                          double * _Nullable cycleStartBeatPosition,
                                          double * _Nullable cycleEndBeatPosition);
typedef void (^AUInputHandler)(AudioUnitRenderActionFlags *actionFlags,
                               const AudioTimeStamp *timestamp,
                               AUAudioFrameCount frameCount,
                               NSInteger inputBusNumber);

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUAudioUnit : NSObject

- (instancetype)init NS_UNAVAILABLE;
- (nullable instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                              options:(AudioComponentInstantiationOptions)options
                                                error:(NSError **)outError NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                                error:(NSError **)outError;

/* Refused by name: instantiation is in-process and the registry holds three built-ins, so there is nothing
 * an out-of-process host could find. The handler is still called, with an error, because a caller that waits
 * for it must not wait forever. */
+ (void)instantiateWithComponentDescription:(AudioComponentDescription)componentDescription
                                    options:(AudioComponentInstantiationOptions)options
                          completionHandler:(void (^)(AUAudioUnit * _Nullable audioUnit,
                                                      NSError * _Nullable error))completionHandler;

@property (nonatomic, readonly) AudioComponentDescription componentDescription;
@property (nonatomic, readonly) AudioComponent component;
@property (nonatomic, readonly, copy, nullable) NSString *componentName;
@property (nonatomic, readonly, copy, nullable) NSString *audioUnitName;
@property (nonatomic, readonly, copy, nullable) NSString *manufacturerName;
@property (nonatomic, readonly) uint32_t componentVersion;

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError;
- (void)deallocateRenderResources;
@property (nonatomic, readonly) BOOL renderResourcesAllocated;
- (void)reset;

@property (nonatomic, readonly) AUAudioUnitBusArray *inputBusses;
@property (nonatomic, readonly) AUAudioUnitBusArray *outputBusses;
@property (nonatomic, readonly) AURenderBlock renderBlock;
@property (nonatomic, readonly) AUScheduleParameterBlock scheduleParameterBlock;
@property (nonatomic, readonly, nullable) AUParameterTree *parameterTree;
@property (nonatomic) AUAudioFrameCount maximumFramesToRender;

- (NSInteger)tokenByAddingRenderObserver:(AURenderObserver)observer;
- (void)removeRenderObserver:(NSInteger)token;

@property (nonatomic, readonly) BOOL allParameterValues;
@property (nonatomic, readonly, getter=isMusicDeviceOrEffect) BOOL musicDeviceOrEffect;
@property (nonatomic, readonly) NSInteger virtualMIDICableCount;
@property (nonatomic, readonly, nullable) AUScheduleMIDIEventBlock scheduleMIDIEventBlock;

/* Optional on macOS, and here the state is the unit's own `kAudioUnitProperty_ClassInfo`, which this phase
 * does not carry: a unit that answered a full state dictionary would be inventing a serialisation the C
 * side cannot round-trip. Read-only doors return nil / their default and are named in the README. */
@property (nonatomic, copy, nullable) NSDictionary<NSString *, id> *fullState;
@property (nonatomic, copy, nullable) NSDictionary<NSString *, id> *fullStateForDocument;
@property (nonatomic, readonly, copy, nullable) NSArray<AUAudioUnitPreset *> *factoryPresets;
@property (nonatomic, retain, nullable) AUAudioUnitPreset *currentPreset;
@property (nonatomic, readonly) NSTimeInterval latency;
@property (nonatomic, readonly) NSTimeInterval tailTime;
@property (nonatomic) NSInteger renderQuality;
@property (nonatomic) BOOL shouldBypassEffect;
@property (nonatomic, readonly) BOOL canProcessInPlace;
@property (nonatomic, getter=isRenderingOffline) BOOL renderingOffline;
@property (nonatomic, readonly, copy, nullable) NSArray<NSNumber *> *channelCapabilities;
@property (nonatomic, copy, nullable) AUHostMusicalContextBlock musicalContextBlock;
@property (nonatomic, copy, nullable) AUHostTransportStateBlock transportStateBlock;
@property (nonatomic, copy, nullable) NSString *contextName;
@property (nonatomic, readonly) BOOL supportsMPE NS_AVAILABLE(10_12, 10_0);
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *channelMap NS_AVAILABLE(10_12, 10_0);

@end

@interface AUAudioUnit (AUAudioInputOutputUnit)

@property (nonatomic, readonly) BOOL canPerformInput;
@property (nonatomic, readonly) BOOL canPerformOutput;
@property (nonatomic, getter=isInputEnabled) BOOL inputEnabled;
@property (nonatomic, getter=isOutputEnabled) BOOL outputEnabled;
@property (nonatomic, copy, nullable) AURenderPullInputBlock outputProvider;
@property (nonatomic, copy, nullable) AUInputHandler inputHandler;
@property (nonatomic, readonly) AudioObjectID deviceID;
- (BOOL)setDeviceID:(AudioObjectID)deviceID error:(NSError **)outError;
- (BOOL)startHardwareAndReturnError:(NSError **)outError;
- (void)stopHardware;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUAudioUnitBusArray : NSObject <NSFastEnumeration>

- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithAudioUnit:(AUAudioUnit *)owner
                          busType:(AUAudioUnitBusType)busType
                           busses:(NSArray<AUAudioUnitBus *> *)busArray NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithAudioUnit:(AUAudioUnit *)owner busType:(AUAudioUnitBusType)busType;

@property (nonatomic, readonly) NSUInteger count;
- (AUAudioUnitBus *)objectAtIndexedSubscript:(NSUInteger)index;
@property (nonatomic, readonly, getter=isCountChangeable) BOOL countChangeable;
- (BOOL)setBusCount:(NSUInteger)count error:(NSError **)outError;
- (void)addObserverToAllBusses:(NSObject *)observer
                    forKeyPath:(NSString *)keyPath
                       options:(NSKeyValueObservingOptions)options
                       context:(void * _Nullable)context;
- (void)removeObserverFromAllBusses:(NSObject *)observer
                         forKeyPath:(NSString *)keyPath
                            context:(void * _Nullable)context;

@property (nonatomic, readonly, assign) AUAudioUnit *ownerAudioUnit;
@property (nonatomic, readonly) AUAudioUnitBusType busType;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUAudioUnitBus : NSObject

/* Apple's is an `AVAudioFormat *`. See the header comment: this is the description the unit's own
 * `kAudioUnitProperty_StreamFormat` returns, and the only format type this library has. */
@property (nonatomic, readonly) AudioStreamBasicDescription format;
- (BOOL)setFormat:(AudioStreamBasicDescription)format error:(NSError **)outError;
@property (nonatomic, getter=isEnabled) BOOL enabled;
@property (nonatomic, copy, nullable) NSString *name;
@property (nonatomic, readonly) NSUInteger index;
@property (nonatomic, readonly) AUAudioUnitBusType busType;
@property (nonatomic, readonly, assign) AUAudioUnit *ownerAudioUnit;
@property (nonatomic, readonly, copy, nullable) NSArray<NSNumber *> *supportedChannelLayoutTags;
@property (nonatomic) NSTimeInterval contextPresentationLatency;

@end

NS_CLASS_AVAILABLE(10_11, 9_0)
@interface AUAudioUnitPreset : NSObject

@property (nonatomic) NSInteger number;
@property (nonatomic, copy) NSString *name;

@end

NS_ASSUME_NONNULL_END

#endif /* __OBJC__ */

#endif /* AGORA_AUDIOTOOLBOX_AUAUDIOUNIT_H */
