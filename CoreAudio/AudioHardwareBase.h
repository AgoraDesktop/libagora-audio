/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * CoreAudio/AudioHardwareBase.h — the object model everything in Core Audio is addressed through.
 *
 * EVERYTHING IS AN OBJECT WITH A NUMBER: the system, its devices, their streams, their controls. A question
 * about any of them is a `(selector, scope, element)` triple and a size, which is why one function
 * (`AudioObjectGetPropertyData`) answers almost all of them — devices are not a special case, they are
 * objects that happen to have audio streams hanging off them.
 *
 * SPECIFIED BY macOS 10.12: the three types, the address structure, the listener shapes and the selectors
 * below are the reference's, with the values checked by `tests/api/macros.c` and the layout by
 * `tests/api/layout.c`. The selectors this component answers are listed in `AudioHardware.c`; the rest of
 * Apple's table — clock devices, boxes, taps, aggregates — is a later phase and answers
 * `kAudioHardwareUnknownPropertyError` until then.
 *
 * `kAudioObjectPropertyElementMaster` IS THE NAME IN 10.12. Later SDKs renamed it `...ElementMain` with the
 * same zero value; the manifest is the authority here, and it has only the older spelling.
 */
#ifndef AGORA_COREAUDIO_AUDIOHARDWAREBASE_H
#define AGORA_COREAUDIO_AUDIOHARDWAREBASE_H

#include <CoreAudio/CoreAudioTypes.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef UInt32 AudioObjectID;
typedef AudioObjectID AudioDeviceID;
typedef AudioObjectID AudioStreamID;

typedef UInt32 AudioObjectPropertySelector;
typedef UInt32 AudioObjectPropertyScope;
typedef UInt32 AudioObjectPropertyElement;
typedef AudioObjectPropertySelector AudioObjectPropertyID;
typedef AudioObjectPropertySelector AudioHardwarePropertyID;
typedef AudioObjectPropertySelector AudioDevicePropertyID;
typedef AudioObjectPropertySelector AudioStreamPropertyID;

struct AudioObjectPropertyAddress
{
	AudioObjectPropertySelector mSelector;
	AudioObjectPropertyScope    mScope;
	AudioObjectPropertyElement  mElement;
};
typedef struct AudioObjectPropertyAddress AudioObjectPropertyAddress;

/* The property listener, in both of Apple's shapes: a C callback and a block, each with the client data or
 * queue it needs. */
typedef OSStatus
(*AudioObjectPropertyListenerProc)(AudioObjectID                  inObjectID,
                                   UInt32                         inNumberAddresses,
                                   const AudioObjectPropertyAddress * inAddresses,
                                   void * _Nullable               inClientData);

typedef void
(^AudioObjectPropertyListenerBlock)(UInt32                         inNumberAddresses,
                                    const AudioObjectPropertyAddress * inAddresses);

/* A device's io callback: what came in, what should go out, and the times either side of it. */
typedef OSStatus
(*AudioDeviceIOProc)(AudioObjectID          inDevice,
                     const AudioTimeStamp * inNow,
                     const AudioBufferList * _Nullable inInputData,
                     const AudioTimeStamp * inInputTime,
                     AudioBufferList * _Nullable outOutputData,
                     const AudioTimeStamp * inOutputTime,
                     void * _Nullable      inClientData);

typedef void
(^AudioDeviceIOBlock)(const AudioTimeStamp * _Nullable inNow,
                      const AudioBufferList * _Nullable inInputData,
                      const AudioTimeStamp * _Nullable inInputTime,
                      AudioBufferList * _Nullable outOutputData,
                      const AudioTimeStamp * _Nullable inOutputTime);

typedef AudioDeviceIOProc AudioDeviceIOProcID;

/* The errors this door returns. The '\0'-free four-char spellings are Apple's, and `'nope'` for an
 * operation that exists but is not allowed is worth keeping exactly. */
CF_ENUM(OSStatus)
{
    kAudioHardwareNoError                       = 0,
    kAudioHardwareNotRunningError               = 'stop',
    kAudioHardwareUnspecifiedError              = 'what',
    kAudioHardwareUnknownPropertyError          = 'who?',
    kAudioHardwareBadPropertySizeError          = '!siz',
    kAudioHardwareIllegalOperationError         = 'nope',
    kAudioHardwareBadObjectError                = '!obj',
    kAudioHardwareBadDeviceError                = '!dev',
    kAudioHardwareBadStreamError                = '!str',
    kAudioHardwareUnsupportedOperationError     = 'unop',
    kAudioDeviceUnsupportedFormatError          = '!dat',
    kAudioDevicePermissionsError                = '!hog'
};

CF_ENUM(AudioObjectID)
{
    kAudioObjectUnknown                         = 0,
    kAudioObjectSystemObject                    = 1
};

CF_ENUM(AudioObjectPropertyScope)
{
    kAudioObjectPropertyScopeGlobal             = 'glob',
    kAudioObjectPropertyScopeInput              = 'inpt',
    kAudioObjectPropertyScopeOutput             = 'outp',
    kAudioObjectPropertyScopePlayThrough        = 'ptru',
    kAudioObjectPropertyScopeWildcard           = '****'
};

CF_ENUM(AudioObjectPropertyElement)
{
    kAudioObjectPropertyElementMaster           = 0,
    kAudioObjectPropertyElementWildcard         = 0xFFFFFFFF
};

/* The properties every object has, whatever it is. */
CF_ENUM(AudioObjectPropertySelector)
{
    kAudioObjectPropertyBaseClass               = 'bcls',
    kAudioObjectPropertyClass                   = 'clas',
    kAudioObjectPropertyOwner                   = 'stdv',
    kAudioObjectPropertyName                    = 'lnam',
    kAudioObjectPropertyManufacturer            = 'lmak',
    kAudioObjectPropertyElementCategoryName     = 'lcat',
    kAudioObjectPropertyElementName             = 'lchn',
    kAudioObjectPropertyElementNumberName       = 'lch#',
    kAudioObjectPropertyOwnedObjects            = 'ownd'
};

/* The system object's own questions. */
CF_ENUM(AudioHardwarePropertyID)
{
    kAudioHardwarePropertyDevices               = 'dev#',
    kAudioHardwarePropertyDefaultInputDevice    = 'dIn ',
    kAudioHardwarePropertyDefaultOutputDevice   = 'dOut',
    kAudioHardwarePropertyDefaultSystemOutputDevice = 'sOut',
    kAudioHardwarePropertyProcessIsAudible      = 'pmut',
    kAudioHardwarePropertyServiceRestarted      = 'srst'
};

/* A device, and the controls that are visible without opening it. */
CF_ENUM(AudioDevicePropertyID)
{
    kAudioDevicePropertyDeviceName              = 'name',
    kAudioDevicePropertyDeviceNameCFString      = 'lnam',
    kAudioDevicePropertyDeviceManufacturer      = 'makr',
    kAudioDevicePropertyDeviceManufacturerCFString = 'lmak',
    kAudioDevicePropertyDeviceUID               = 'uid ',
    kAudioDevicePropertyTransportType           = 'tran',
    kAudioDevicePropertyDeviceIsAlive           = 'livn',
    kAudioDevicePropertyDeviceIsRunning         = 'goin',
    kAudioDevicePropertyDeviceIsRunningSomewhere = 'gone',
    kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt',
    kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
    kAudioDevicePropertyStreams                 = 'stm#',
    kAudioDevicePropertyStreamConfiguration     = 'slay',
    kAudioDevicePropertyNominalSampleRate       = 'nsrt',
    kAudioDevicePropertyActualSampleRate        = 'asrt',
    kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#',
    kAudioDevicePropertyBufferFrameSize         = 'fsiz',
    kAudioDevicePropertyBufferFrameSizeRange    = 'fsz#',
    kAudioDevicePropertyVolumeScalar            = 'volm',
    kAudioDevicePropertyVolumeDecibels          = 'vold',
    kAudioDevicePropertyMute                    = 'mute',
    kAudioDevicePropertySafetyOffset            = 'saft',
    kAudioDevicePropertyLatency                 = 'ltnc',
    kAudioDevicePropertyHogMode                 = 'oink'
};

/* A stream: what it carries, and how. */
CF_ENUM(AudioStreamPropertyID)
{
    kAudioStreamPropertyVirtualFormat           = 'sfmt',
    kAudioStreamPropertyPhysicalFormat          = 'pft ',
    kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
    kAudioStreamPropertyAvailablePhysicalFormats = 'pfma',
    kAudioStreamPropertyTerminalType            = 'term',
    kAudioStreamPropertyStartingChannel         = 'schn',
    kAudioStreamPropertyLatency                 = 'ltnc',
    kAudioStreamPropertyDirection               = 'sdir',
    kAudioStreamPropertyIsActive                = 'sact',
    kAudioStreamPropertyOwningDevice            = 'ownd'
};

CF_ENUM(UInt32)
{
    kAudioStreamTerminalTypeUnknown             = 0,
    kAudioStreamTerminalTypeLine                = 'line',
    kAudioStreamTerminalTypeDigitalAudioInterface = 'spdf',
    kAudioStreamTerminalTypeSpeaker             = 'spkr',
    kAudioStreamTerminalTypeHeadphones          = 'hdph',
    kAudioStreamTerminalTypeMicrophone          = 'micr',
    kAudioStreamTerminalTypeHDMI                = 'hdmi'
};

CF_ENUM(UInt32)
{
    kAudioDeviceTransportTypeBuiltIn            = 'bltn',
    kAudioDeviceTransportTypeVirtual            = 'virt',
    kAudioDeviceTransportTypeUSB                = 'usb ',
    kAudioDeviceTransportTypeBluetooth          = 'blue',
    kAudioDeviceTransportTypeHDMI               = 'hdmi',
    kAudioDeviceTransportTypeDisplayPort        = 'dprt',
    kAudioDeviceTransportTypeAggregate          = 'aggr'
};

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_COREAUDIO_AUDIOHARDWAREBASE_H */
