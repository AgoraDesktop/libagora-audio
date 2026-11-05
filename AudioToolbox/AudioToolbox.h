/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AudioToolbox.h — the umbrella, listing what exists.
 *
 * Apple's 10.12 umbrella names about thirty headers. Listing the ones that are here and growing the list
 * with each phase is the honest form of the same door: a client that includes this header compiles, and a
 * client that needs `AudioQueue.h` finds out by not finding it, rather than by linking against a stub.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUDIOTOOLBOX_H
#define AGORA_AUDIOTOOLBOX_AUDIOTOOLBOX_H

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioFileStream.h>
#include <AudioToolbox/ExtendedAudioFile.h>
#include <AudioToolbox/AudioQueue.h>
#include <AudioToolbox/AudioServices.h>
#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioOutputUnit.h>

#endif /* AGORA_AUDIOTOOLBOX_AUDIOTOOLBOX_H */
