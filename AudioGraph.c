/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * AudioGraph.c — nodes, connections, and one pull that goes all the way down.
 *
 * THE GRAPH INSTALLS CALLBACKS; IT DOES NOT RENDER. `AUGraphConnectNodeInput` sets the destination unit's
 * input callback to a function that renders the source node, and `AUGraphStart` sets the output node's callback
 * to one that renders whatever feeds it. So the graph has no loop, no buffer of its own and no clock: the
 * device calls the output unit, the output unit calls the graph's pull, and the pull recurses up the
 * connections until it reaches a node that was given a callback of its own.
 *
 * THAT IS ALSO WHY IT CANNOT CYCLE: a connection from a node to itself, or a loop of them, would recurse until
 * the stack ran out, so `AUGraphConnectNodeInput` refuses a connection that would close a cycle, with
 * `kAUGraphErr_InvalidConnection`. Apple's own documentation says a graph is a tree; this one makes it true.
 *
 * THE NODE NUMBERS ARE INDICES INTO A FIXED TABLE, starting at 0 for the first node added.
 */

#include <AudioToolbox/AUGraph.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include "AudioToolbox_internal.h"

#include <stdlib.h>
#include <string.h>

#define AGORA_GRAPH_MAX_NODES 16
#define AGORA_GRAPH_MAX_CONNECTIONS 32

typedef struct {
	bool                      active;
	AUNode                    id;
	AudioComponentDescription desc;
	AudioUnit                 unit;
	bool                      has_callback;
	AURenderCallbackStruct    callback;
} graph_node;

typedef struct {
	bool    active;
	AUNode  source;
	UInt32  source_output;
	AUNode  dest;
	UInt32  dest_input;
} graph_connection;

struct OpaqueAUGraph {
	graph_node       nodes[AGORA_GRAPH_MAX_NODES];
	UInt32           node_count;
	graph_connection connections[AGORA_GRAPH_MAX_CONNECTIONS];
	UInt32           connection_count;
	bool             open;
	bool             initialized;
	bool             running;
};

/* What a connection's callback carries: the graph, and the node to render. */
typedef struct {
	AUGraph graph;
	AUNode  node;
} graph_pull;

/* The callback installed on a node's input: render the node that feeds it. */
static OSStatus fn_pull_source(void *refCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                               UInt32 bus, UInt32 frames, AudioBufferList *ioData)
{
	graph_pull *pull = refCon;
	graph_node *node = &pull->graph->nodes[pull->node];

	if (node->unit == NULL) {
		return kAUGraphErr_InvalidAudioUnit;
	}

	/* `bus` is the SOURCE's output bus when a graph renders through a connection, so it is passed through: a
	 * unit with several outputs (there are none yet) would need it. */
	return AudioUnitRender(node->unit, flags, timestamp, bus, frames, ioData);
}

static graph_node *fn_node(AUGraph graph, AUNode id)
{
	if (graph == NULL) {
		return NULL;
	}

	if (id < 0 || (UInt32)id >= graph->node_count) {
		return NULL;
	}

	return graph->nodes[id].active ? &graph->nodes[id] : NULL;
}

static graph_connection *fn_connection(AUGraph graph, AUNode dest, UInt32 input)
{
	UInt32 i;

	for (i = 0; i < graph->connection_count; i++) {
		if (graph->connections[i].active && graph->connections[i].dest == dest
		    && graph->connections[i].dest_input == input) {
			return &graph->connections[i];
		}
	}

	return NULL;
}

/* Is there a path FORWARD from `from` to `to`? Connecting `src -> dest` closes a ring exactly when `dest`
 * already reaches `src`, so this is asked with the destination first. The direction is the whole bug this
 * function once had: walking the connections backwards found no ring, installed one, and the pull then
 * recursed until the stack ran out. */
static bool fn_path_exists(AUGraph graph, AUNode from, AUNode to, UInt32 depth)
{
	UInt32 i;

	if (from == to) {
		return true;
	}
	if (depth > AGORA_GRAPH_MAX_NODES) {
		return true;	/* the table is already broken; refusing is the safe answer */
	}

	for (i = 0; i < graph->connection_count; i++) {
		graph_connection *connection = &graph->connections[i];

		if (connection->active && connection->source == from
		    && fn_path_exists(graph, connection->dest, to, depth + 1)) {
			return true;
		}
	}

	return false;
}

OSStatus NewAUGraph(AUGraph *outGraph)
{
	AUGraph graph;

	if (outGraph == NULL) {
		return kAudio_ParamError;
	}

	graph = calloc(1, sizeof(*graph));
	if (graph == NULL) {
		return kAudio_MemFullError;
	}

	*outGraph = graph;
	return 0;
}

OSStatus DisposeAUGraph(AUGraph inGraph)
{
	UInt32 i;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	if (inGraph->running) {
		AUGraphStop(inGraph);
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].unit != NULL) {
			AudioComponentInstanceDispose(inGraph->nodes[i].unit);
		}
	}

	free(inGraph);
	return 0;
}

OSStatus AUGraphAddNode(AUGraph inGraph, const AudioComponentDescription *inDesc, AUNode *outNode)
{
	AudioComponent component;
	UInt32 slot = AGORA_GRAPH_MAX_NODES;
	UInt32 i;

	if (inGraph == NULL || inDesc == NULL || outNode == NULL) {
		return kAudio_ParamError;
	}

	if (inGraph->initialized) {
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	for (i = 0; i < AGORA_GRAPH_MAX_NODES; i++) {
		if (!inGraph->nodes[i].active) {
			slot = i;
			break;
		}
	}

	if (slot == AGORA_GRAPH_MAX_NODES) {
		return kAudio_MemFullError;
	}

	component = AudioComponentFindNext(NULL, inDesc);
	if (component == NULL) {
		return kAUGraphErr_InvalidAudioUnit;
	}

	memset(&inGraph->nodes[slot], 0, sizeof(inGraph->nodes[slot]));
	inGraph->nodes[slot].active = true;
	inGraph->nodes[slot].id = (AUNode)slot;
	inGraph->nodes[slot].desc = *inDesc;

	if (AudioComponentInstanceNew(component, &inGraph->nodes[slot].unit) != 0) {
		inGraph->nodes[slot].active = false;
		return kAUGraphErr_InvalidAudioUnit;
	}

	if ((UInt32)slot >= inGraph->node_count) {
		inGraph->node_count = (UInt32)slot + 1;
	}

	*outNode = (AUNode)slot;
	return 0;
}

OSStatus AUGraphRemoveNode(AUGraph inGraph, AUNode inNode)
{
	graph_node *node = fn_node(inGraph, inNode);
	UInt32 i;

	if (node == NULL) {
		return kAUGraphErr_NodeNotFound;
	}

	if (inGraph->initialized) {
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	/* A node that is still wired is a graph with a hole in it: disconnect first, as Apple requires. */
	for (i = 0; i < inGraph->connection_count; i++) {
		if (inGraph->connections[i].active
		    && (inGraph->connections[i].source == inNode || inGraph->connections[i].dest == inNode)) {
			return kAUGraphErr_InvalidConnection;
		}
	}

	if (node->unit != NULL) {
		AudioComponentInstanceDispose(node->unit);
	}

	node->active = false;
	node->unit = NULL;
	node->desc = (AudioComponentDescription){ 0, 0, 0, 0, 0 };

	return 0;
}

OSStatus AUGraphGetNodeCount(AUGraph inGraph, UInt32 *outNumberOfNodes)
{
	UInt32 i;
	UInt32 count = 0;

	if (inGraph == NULL || outNumberOfNodes == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active) {
			count++;
		}
	}

	*outNumberOfNodes = count;
	return 0;
}

OSStatus AUGraphGetIndNode(AUGraph inGraph, UInt32 inIndex, AUNode *outNode)
{
	UInt32 i;
	UInt32 seen = 0;

	if (inGraph == NULL || outNode == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active) {
			if (seen == inIndex) {
				*outNode = inGraph->nodes[i].id;
				return 0;
			}
			seen++;
		}
	}

	return kAUGraphErr_NodeNotFound;
}

OSStatus AUGraphNodeInfo(AUGraph inGraph, AUNode inNode, AudioComponentDescription *outDesc,
                         AudioUnit *outAudioUnit)
{
	graph_node *node = fn_node(inGraph, inNode);

	if (node == NULL) {
		return kAUGraphErr_NodeNotFound;
	}

	if (outDesc != NULL) {
		*outDesc = node->desc;
	}
	if (outAudioUnit != NULL) {
		*outAudioUnit = node->unit;
	}

	return 0;
}

OSStatus AUGraphOpen(AUGraph inGraph)
{
	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	inGraph->open = true;
	return 0;
}

OSStatus AUGraphClose(AUGraph inGraph)
{
	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	if (inGraph->running) {
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	inGraph->open = false;
	return 0;
}

OSStatus AUGraphConnectNodeInput(AUGraph inGraph, AUNode inSourceNode, UInt32 inSourceOutputNumber,
                                 AUNode inDestNode, UInt32 inDestInputNumber)
{
	graph_node *source = fn_node(inGraph, inSourceNode);
	graph_node *dest = fn_node(inGraph, inDestNode);
	graph_connection *connection;
	AURenderCallbackStruct callback;
	graph_pull *pull;
	UInt32 i;

	if (source == NULL || dest == NULL) {
		return kAUGraphErr_NodeNotFound;
	}

	if (inGraph->running) {
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	/* A CYCLE IS NOT A TOPOLOGY THIS GRAPH HAS: the pull would recurse until the stack ran out. */
	if (fn_path_exists(inGraph, inDestNode, inSourceNode, 0)) {
		return kAUGraphErr_InvalidConnection;
	}

	connection = fn_connection(inGraph, inDestNode, inDestInputNumber);
	if (connection == NULL) {
		for (i = 0; i < AGORA_GRAPH_MAX_CONNECTIONS; i++) {
			if (!inGraph->connections[i].active) {
				connection = &inGraph->connections[i];
				inGraph->connections[i].active = true;
				if (i >= inGraph->connection_count) {
					inGraph->connection_count = i + 1;
				}
				break;
			}
		}
	}

	if (connection == NULL) {
		return kAudio_MemFullError;
	}

	connection->source = inSourceNode;
	connection->source_output = inSourceOutputNumber;
	connection->dest = inDestNode;
	connection->dest_input = inDestInputNumber;

	/* The pull: the destination's input callback renders the source. */
	pull = calloc(1, sizeof(*pull));
	if (pull == NULL) {
		connection->active = false;
		return kAudio_MemFullError;
	}
	pull->graph = inGraph;
	pull->node = inSourceNode;

	callback.inputProc = fn_pull_source;
	callback.inputProcRefCon = pull;

	{
		OSStatus status = AudioUnitSetProperty(dest->unit, kAudioUnitProperty_SetRenderCallback,
		                                       (dest->desc.componentType == kAudioUnitType_Mixer
		                                        ? kAudioUnitScope_Input : kAudioUnitScope_Global),
		                                       inDestInputNumber, &callback, sizeof(callback));

		if (status != 0) {
			free(pull);
			connection->active = false;
			return kAUGraphErr_InvalidConnection;
		}
	}

	return 0;
}

OSStatus AUGraphDisconnectNodeInput(AUGraph inGraph, AUNode inDestNode, UInt32 inDestInputNumber)
{
	graph_connection *connection;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	connection = fn_connection(inGraph, inDestNode, inDestInputNumber);
	if (connection == NULL) {
		return kAUGraphErr_InvalidConnection;
	}

	connection->active = false;
	return 0;
}

OSStatus AUGraphSetNodeInputCallback(AUGraph inGraph, AUNode inDestNode, UInt32 inDestInputNumber,
                                     const AURenderCallbackStruct *inInputCallback)
{
	graph_node *node = fn_node(inGraph, inDestNode);

	if (node == NULL) {
		return kAUGraphErr_NodeNotFound;
	}

	if (inInputCallback == NULL) {
		return kAudio_ParamError;
	}

	/* A node fed by a connection cannot also be fed by a callback: the connection owns that input. */
	if (fn_connection(inGraph, inDestNode, inDestInputNumber) != NULL) {
		return kAUGraphErr_InvalidConnection;
	}

	node->callback = *inInputCallback;
	node->has_callback = true;

	{
		AudioUnitScope scope = (node->desc.componentType == kAudioUnitType_Mixer)
		                     ? kAudioUnitScope_Input : kAudioUnitScope_Global;

		if (AudioUnitSetProperty(node->unit, kAudioUnitProperty_SetRenderCallback, scope,
		                         inDestInputNumber, inInputCallback, sizeof(*inInputCallback)) != 0) {
			return kAUGraphErr_InvalidAudioUnit;
		}
	}

	return 0;
}

/* The callback the output node is given when the graph starts: render whatever feeds it. */
static OSStatus fn_graph_output(void *refCon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                                UInt32 bus, UInt32 frames, AudioBufferList *ioData)
{
	graph_pull *pull = refCon;
	graph_node *output = &pull->graph->nodes[pull->node];
	graph_connection *connection = fn_connection(pull->graph, pull->node, 0);

	if (connection != NULL) {
		graph_node *source = fn_node(pull->graph, connection->source);

		if (source == NULL) {
			return kAUGraphErr_NodeNotFound;
		}

		return AudioUnitRender(source->unit, flags, timestamp, connection->source_output, frames, ioData);
	}

	if (output->has_callback && output->callback.inputProc != NULL) {
		return output->callback.inputProc(output->callback.inputProcRefCon, flags, timestamp, bus, frames, ioData);
	}

	/* Nothing feeds the output: silence is the graph's answer, not an error. */
	memset(ioData->mBuffers[0].mData, 0,
	       (size_t)frames * ioData->mBuffers[0].mNumberChannels * sizeof(float));
	return 0;
}

OSStatus AUGraphInitialize(AUGraph inGraph)
{
	UInt32 i;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	if (!inGraph->open) {
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active && AudioUnitInitialize(inGraph->nodes[i].unit) != 0) {
			return kAUGraphErr_CannotDoInCurrentContext;
		}
	}

	inGraph->initialized = true;
	return 0;
}

OSStatus AUGraphUninitialize(AUGraph inGraph)
{
	UInt32 i;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active) {
			AudioUnitUninitialize(inGraph->nodes[i].unit);
		}
	}

	inGraph->initialized = false;
	return 0;
}

OSStatus AUGraphStart(AUGraph inGraph)
{
	UInt32 i;
	graph_node *output = NULL;
	graph_pull *pull;
	AURenderCallbackStruct callback;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	if (inGraph->running) {
		return 0;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active && inGraph->nodes[i].desc.componentType == kAudioUnitType_Output) {
			output = &inGraph->nodes[i];
			break;
		}
	}

	if (output == NULL) {
		return kAUGraphErr_OutputNodeErr;
	}

	pull = calloc(1, sizeof(*pull));
	if (pull == NULL) {
		return kAudio_MemFullError;
	}
	pull->graph = inGraph;
	pull->node = output->id;

	callback.inputProc = fn_graph_output;
	callback.inputProcRefCon = pull;

	if (AudioUnitSetProperty(output->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0,
	                         &callback, sizeof(callback)) != 0) {
		free(pull);
		return kAUGraphErr_InvalidAudioUnit;
	}

	if (AudioOutputUnitStart(output->unit) != 0) {
		free(pull);
		return kAUGraphErr_CannotDoInCurrentContext;
	}

	inGraph->running = true;
	return 0;
}

OSStatus AUGraphStop(AUGraph inGraph)
{
	UInt32 i;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->node_count; i++) {
		if (inGraph->nodes[i].active && inGraph->nodes[i].desc.componentType == kAudioUnitType_Output) {
			AudioOutputUnitStop(inGraph->nodes[i].unit);
		}
	}

	inGraph->running = false;
	return 0;
}

OSStatus AUGraphUpdate(AUGraph inGraph, Boolean *outIsUpdated)
{
	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	/* Connections take effect when they are made, so an update has nothing left to do — which is what the
	 * out-parameter says rather than pretending otherwise. */
	if (outIsUpdated != NULL) {
		*outIsUpdated = false;
	}

	return 0;
}

OSStatus AUGraphIsOpen(AUGraph inGraph, Boolean *outIsOpen)
{
	if (inGraph == NULL || outIsOpen == NULL) {
		return kAudio_ParamError;
	}

	*outIsOpen = inGraph->open;
	return 0;
}

OSStatus AUGraphIsInitialized(AUGraph inGraph, Boolean *outIsInitialized)
{
	if (inGraph == NULL || outIsInitialized == NULL) {
		return kAudio_ParamError;
	}

	*outIsInitialized = inGraph->initialized;
	return 0;
}

OSStatus AUGraphIsRunning(AUGraph inGraph, Boolean *outIsRunning)
{
	if (inGraph == NULL || outIsRunning == NULL) {
		return kAudio_ParamError;
	}

	*outIsRunning = inGraph->running;
	return 0;
}

OSStatus AUGraphGetNumberOfConnections(AUGraph inGraph, UInt32 *outNumberOfConnections)
{
	UInt32 i;
	UInt32 count = 0;

	if (inGraph == NULL || outNumberOfConnections == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->connection_count; i++) {
		if (inGraph->connections[i].active) {
			count++;
		}
	}

	*outNumberOfConnections = count;
	return 0;
}

OSStatus AUGraphClearConnections(AUGraph inGraph)
{
	UInt32 i;

	if (inGraph == NULL) {
		return kAudio_ParamError;
	}

	for (i = 0; i < inGraph->connection_count; i++) {
		inGraph->connections[i].active = false;
	}

	return 0;
}
