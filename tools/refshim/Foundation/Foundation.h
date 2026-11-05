/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * A stand-in for Foundation's *declarations*, for parsing Apple's AUv3 headers. NOT part of the
 * library: nothing in AudioToolbox/ includes it, and it is never installed.
 *
 * WHY IT EXISTS AT ALL: `AUParameters.h` imports Foundation because a parameter tree is made of
 * `NSString`s and `NSArray`s, and `AUAudioUnit.h` imports `NSExtensionRequestHandling` because an
 * AUv3 may be vended by an app extension. This library's own headers import the real Foundation
 * from `build-prefix`; only the reference parse — which must depend on nothing in this tree — sees
 * this file. So the classes below are declared, never implemented, and the manifest records their
 * *names in signatures*, which is all it ever needed.
 *
 * THE MACROS ARE THE POINT, not the classes: `NS_ENUM`, `NS_OPTIONS`, `NS_CLASS_AVAILABLE` and the
 * rest are defined to the vocabulary clang already understands, so Apple's text parses unchanged.
 */
#ifndef __AGORA_REFSHIM_FOUNDATION__
#define __AGORA_REFSHIM_FOUNDATION__

#include <CoreFoundation/CFBase.h>

typedef signed char BOOL;
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef double NSTimeInterval;

#define NS_ENUM(_type, _name) CF_ENUM(_type, _name)
#define NS_OPTIONS(_type, _name) CF_ENUM(_type, _name)
#define NS_CLASS_AVAILABLE(...)
#define NS_AVAILABLE(...)
#define NS_DESIGNATED_INITIALIZER
#define NS_UNAVAILABLE __attribute__((unavailable))
#define NS_NONATOMIC_IOSONLY nonatomic
#define NS_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define NS_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")

typedef NSUInteger NSKeyValueObservingOptions;

@class NSString, NSArray, NSNumber, NSDictionary, NSError, NSData;

@protocol NSObject
@end

__attribute__((objc_root_class))
@interface NSObject <NSObject>
@end

@interface NSString : NSObject
@end

@interface NSNumber : NSObject
@end

@protocol NSSecureCoding <NSObject>
@end

@protocol NSFastEnumeration <NSObject>
@end

@interface NSArray<__covariant ObjectType> : NSObject <NSSecureCoding, NSFastEnumeration>
@end

@interface NSDictionary<__covariant KeyType, __covariant ObjectType> : NSObject <NSSecureCoding, NSFastEnumeration>
@end

@interface NSError : NSObject <NSSecureCoding>
@end

#endif
