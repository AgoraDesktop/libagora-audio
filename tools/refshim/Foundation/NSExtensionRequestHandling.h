/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * The one protocol `AUAudioUnit.h` imports Foundation for: how an app extension answers a request
 * from its host. It is declared and never used here — this library does not load extensions, and the
 * reference header only names it — so an empty conforming protocol is the whole shim.
 */
#ifndef __AGORA_REFSHIM_NSEXTENSIONREQUESTHANDLING__
#define __AGORA_REFSHIM_NSEXTENSIONREQUESTHANDLING__

#include <Foundation/Foundation.h>

@protocol NSExtensionRequestHandling <NSObject>
@end

#endif
