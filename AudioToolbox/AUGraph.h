/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioToolbox/AUGraph.h — units wired to each other.
 *
 * A GRAPH IS A PULLING TREE, NOT A SCHEDULER. Its nodes are units, its connections say which unit feeds which
 * input, and the only node with a clock is the output one: starting the graph attaches the output unit to the
 * device, and everything else is rendered on demand, back up the connections. That is why there is no thread
 * here either — the device has the only one.
 *
 * SPECIFIED BY macOS 10.12: the node type, the connection and render-callback structures, the errors and every
 * prototype are the reference's. The names are Apple's too, which is why it is `NewAUGraph`/`DisposeAUGraph`
 * rather than the `AUGraphNew`/`AUGraphDispose` the other doors would suggest.
 *
 * THE LEGACY DOORS ARE NOT DECLARED: `AUGraphNewNode` and `AUGraphNewNodeSubGraph` take a Carbon
 * `ComponentDescription`, and sub-graphs are a second topology for a feature nobody here has asked for.
 */
#ifndef AGORA_AUDIOTOOLBOX_AUGRAPH_H
#define AGORA_AUDIOTOOLBOX_AUGRAPH_H

#include <AudioToolbox/AudioUnit.h>

CF_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C"
{
#endif

typedef struct OpaqueAUGraph *   AUGraph;
typedef SInt32                   AUNode;

struct AudioUnitNodeConnection
{
	AUNode  sourceNode;
	UInt32  sourceOutputNumber;
	AUNode  destNode;
	UInt32  destInputNumber;
};
typedef struct AudioUnitNodeConnection AudioUnitNodeConnection;

struct AUNodeRenderCallback
{
	AUNode                  destNode;
	AudioUnitElement        destInputNumber;
	AURenderCallbackStruct  cback;
};
typedef struct AUNodeRenderCallback AUNodeRenderCallback;

CF_ENUM(OSStatus)
{
	kAUGraphErr_NodeNotFound              = -10860,
	kAUGraphErr_InvalidConnection         = -10861,
	kAUGraphErr_OutputNodeErr             = -10862,
	kAUGraphErr_CannotDoInCurrentContext  = -10863,
	kAUGraphErr_InvalidAudioUnit          = -10864
};

extern OSStatus
NewAUGraph(AUGraph _Nullable * _Nonnull outGraph);

extern OSStatus
DisposeAUGraph(AUGraph inGraph);

extern OSStatus
AUGraphAddNode(AUGraph inGraph, const AudioComponentDescription * inDesc, AUNode * outNode);

extern OSStatus
AUGraphRemoveNode(AUGraph inGraph, AUNode inNode);

extern OSStatus
AUGraphGetNodeCount(AUGraph inGraph, UInt32 * outNumberOfNodes);

extern OSStatus
AUGraphGetIndNode(AUGraph inGraph, UInt32 inIndex, AUNode * outNode);

extern OSStatus
AUGraphNodeInfo(AUGraph inGraph, AUNode inNode, AudioComponentDescription * _Nullable outDesc,
                AudioUnit _Nullable * _Nullable outAudioUnit);

extern OSStatus
AUGraphOpen(AUGraph inGraph);

extern OSStatus
AUGraphClose(AUGraph inGraph);

extern OSStatus
AUGraphConnectNodeInput(AUGraph inGraph, AUNode inSourceNode, UInt32 inSourceOutputNumber,
                        AUNode inDestNode, UInt32 inDestInputNumber);

extern OSStatus
AUGraphDisconnectNodeInput(AUGraph inGraph, AUNode inDestNode, UInt32 inDestInputNumber);

extern OSStatus
AUGraphSetNodeInputCallback(AUGraph inGraph, AUNode inDestNode, UInt32 inDestInputNumber,
                            const AURenderCallbackStruct * inInputCallback);

extern OSStatus
AUGraphInitialize(AUGraph inGraph);

extern OSStatus
AUGraphUninitialize(AUGraph inGraph);

extern OSStatus
AUGraphStart(AUGraph inGraph);

extern OSStatus
AUGraphStop(AUGraph inGraph);

extern OSStatus
AUGraphUpdate(AUGraph inGraph, Boolean * _Nullable outIsUpdated);

extern OSStatus
AUGraphIsOpen(AUGraph inGraph, Boolean * outIsOpen);

extern OSStatus
AUGraphIsInitialized(AUGraph inGraph, Boolean * outIsInitialized);

extern OSStatus
AUGraphIsRunning(AUGraph inGraph, Boolean * outIsRunning);

extern OSStatus
AUGraphGetNumberOfConnections(AUGraph inGraph, UInt32 * outNumberOfConnections);

extern OSStatus
AUGraphClearConnections(AUGraph inGraph);

#if defined(__cplusplus)
}
#endif

CF_ASSUME_NONNULL_END

#endif /* AGORA_AUDIOTOOLBOX_AUGRAPH_H */
