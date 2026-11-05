/* Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 *
 * An EMPTY <MacTypes.h>, deliberately. libagora-core's CFBase.h guards its own fallback typedefs
 * with !defined(__MACTYPES__) and then includes <MacTypes.h>; leaving this file empty is what makes
 * the fallback win. Defining __MACTYPES__ instead would have suppressed the typedefs the audio
 * headers need.
 */
