/**Copyright (C) Austin Hicks, 2014
This file is part of Libaudioverse, a library for 3D and environmental audio simulation, and is released under the terms of the Gnu General Public License Version 3 or (at your option) any later version.
A copy of the GPL, as well as other important copyright and licensing information, may be found in the file 'LICENSE' in the root of the Libaudioverse repository.  Should this file be missing or unavailable to you, see <http://www.gnu.org/licenses/>.*/

/**Handles functionality common to all objects: linking, allocating, and freeing, as well as parent-child relationships.*/
#include <libaudioverse/libaudioverse.h>
#include <libaudioverse/libaudioverse_properties.h>
#include <libaudioverse/private/memory.hpp>
#include <libaudioverse/private/node.hpp>
#include <libaudioverse/private/connections.hpp>
#include <libaudioverse/private/properties.hpp>
#include <libaudioverse/private/simulation.hpp>
#include <libaudioverse/private/macros.hpp>
#include <libaudioverse/private/metadata.hpp>
#include <libaudioverse/private/kernels.hpp>
#include <libaudioverse/private/buffer.hpp>
#include <algorithm>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>

namespace libaudioverse_implementation {

/**Given two nodes, determine if connecting an output of start to an input of end causes a cycle.*/
bool doesEdgePreserveAcyclicity(std::shared_ptr<Node> start, std::shared_ptr<Node> end) {
	//A cycle exists if end is directly or indirectly conneccted to an input of start.
	//To that end, we use recursion as follows.
	//if we are called with start==end, it's a cycle.
	if(start==end) return false;
	//Inductive step:
	//connecting start to end connects everything "behind" start to end,
	//so there's a cycle if end is already behind start.
	for(auto n: start->getDependencies()) {
		if(doesEdgePreserveAcyclicity(n, end) == false) return false;
	}
	return true;
}

Node::Node(int type, std::shared_ptr<Simulation> simulation, unsigned int numInputBuffers, unsigned int numOutputBuffers): ExternalObject(type) {
	this->simulation= simulation;
	//request properties from the metadata module.
	properties = makePropertyTable(type);
	//and events.
	events = makeEventTable(type);

	//Associate properties to this node:
	for(auto i: properties) {
		auto &prop = properties[i.first];
		prop.associateNode(this);
		prop.associateSimulation(simulation);
	}

	//Loop through callbacks, associating them with our simulation.
	//map iterators dont' give references, only operator[].
	for(auto i: events) {
		events[i.first].associateSimulation(simulation);
		events[i.first].associateNode(this);
	}

	//allocations can be done simply by redirecting through resize after our initialization step.
	resize(numInputBuffers, numOutputBuffers);
	
	//Block sizes never change:
	block_size = simulation->getBlockSize();
}

Node::~Node() {
	for(auto i: output_buffers) {
		freeArray(i);
	}
	for(auto i: input_buffers) {
		freeArray(i);
	}
}

void Node::tickProperties() {
	for(auto &i: properties) {
		i.second.tick();
	}
}

void Node::tick() {
	if(last_processed== simulation->getTickCount()) return; //we processed this tick already.
	//Incrementing this counter here prevents duplication of zeroing outputs if we're in the paused state.
	last_processed = simulation->getTickCount();
	zeroOutputBuffers(); //we always do this because sometimes we're not going to actually do anything else.
	if(getState() == Lav_NODESTATE_PAUSED) return; //nothing to do, for we are paused.
	tickProperties();
	willProcessParents();
	zeroInputBuffers();
	//tick all alive parents, collecting their outputs onto ours.
	//by using the getInputConnection and getInputConnectionCount functions, we allow subgraphs to override effectively.
	bool needsMixing = getProperty(Lav_NODE_CHANNEL_INTERPRETATION).getIntValue()==Lav_CHANNEL_INTERPRETATION_SPEAKERS;
	for(int i = 0; i < getInputConnectionCount(); i++) {
		getInputConnection(i)->add(needsMixing);
	}
	is_processing = true;
	num_input_buffers = input_buffers.size();
	num_output_buffers = output_buffers.size();
	process();
	auto &mulProp = getProperty(Lav_NODE_MUL), &addProp = getProperty(Lav_NODE_ADD);
	float** outputs =getOutputBufferArray();
	if(mulProp.needsARate()) {
		for(int i = 0; i < block_size; i++) {
			float mul = mulProp.getFloatValue(i);
			for(int j = 0; j < getOutputBufferCount(); j++) outputs[j][i]*=mul;
		}
	}
	else if(mulProp.getFloatValue() !=1.0) {
		for(int i = 0; i < getOutputBufferCount(); i++) {
			scalarMultiplicationKernel(block_size, mulProp.getFloatValue(), outputs[i], outputs[i]);
		}
	}
	if(addProp.needsARate()) {
		for(int i = 0; i < block_size; i++) {
		float add=addProp.getFloatValue(i);
			for(int j = 0; j < getOutputBufferCount(); j++) outputs[j][i]+=add;
		}
	}
	else if(addProp.getFloatValue() !=0.0) {
		for(int i = 0; i < getOutputBufferCount(); i++) {
			scalarAdditionKernel(block_size, addProp.getFloatValue(), outputs[i], outputs[i]);
		}
	}
	is_processing = false;
}

//cleans up stuff.
void Node::doMaintenance() {
	//nothing, for now. This is needed for the upcoming refactor.
}

/*Default Processing function.*/
void Node::process() {
}

void Node::zeroOutputBuffers() {
	float** outputBuffers=getOutputBufferArray();
	int outputBufferCount = getOutputBufferCount();
	for(int i = 0; i < outputBufferCount; i++) {
		memset(outputBuffers[i], 0, block_size*sizeof(float));
	}
}

void Node::zeroInputBuffers() {
	int inputBufferCount=getInputBufferCount();
	float** inputBuffers=getInputBufferArray();
	for(int i = 0; i < inputBufferCount; i++) {
		memset(inputBuffers[i], 0, sizeof(float)*simulation->getBlockSize());
	}
}

void Node::willProcessParents() {
}

int Node::getState() {
	return getProperty(Lav_NODE_STATE).getIntValue();
}

int Node::getOutputBufferCount() {
	return output_buffers.size();
}

float** Node::getOutputBufferArray() {
	//vectors are guaranteed to be contiguous in most if not all implementations as well as (possibly, no source handy) the C++11 standard.
	if(output_buffers.size())  return &output_buffers[0];
	else return nullptr;
}

int Node::getInputBufferCount() {
	return input_buffers.size();
}

float** Node::getInputBufferArray() {
	if(input_buffers.size()) return &input_buffers[0];
	else return nullptr;
}

int Node::getInputConnectionCount() {
	return input_connections.size();
}

int Node::getOutputConnectionCount() {
	return output_connections.size();
}

std::shared_ptr<InputConnection> Node::getInputConnection(int which) {
	if(which >= getInputConnectionCount() || which < 0) throw LavErrorException(Lav_ERROR_RANGE);
	return input_connections[which];
}

std::shared_ptr<OutputConnection> Node::getOutputConnection(int which) {
	if(which < 0 || which >= getOutputConnectionCount()) throw LavErrorException(Lav_ERROR_RANGE);
	return output_connections[which];
}

void Node::appendInputConnection(int start, int count) {
	input_connections.emplace_back(new InputConnection(simulation, this, start, count));
}

void Node::appendOutputConnection(int start, int count) {
	output_connections.emplace_back(new OutputConnection(simulation, this, start, count));
}

void Node::connect(int output, std::shared_ptr<Node> toNode, int input) {
	if(doesEdgePreserveAcyclicity(std::static_pointer_cast<Node>(this->shared_from_this()), toNode) == false) throw LavErrorException(Lav_ERROR_CAUSES_CYCLE);
	auto outputConnection =getOutputConnection(output);
	auto inputConnection = toNode->getInputConnection(input);
	makeConnection(outputConnection, inputConnection);
}

void Node::connectSimulation(int which) {
	auto outputConnection=getOutputConnection(which);
	auto inputConnection = simulation->getFinalOutputConnection();
	makeConnection(outputConnection, inputConnection);
}

void Node::connectProperty(int output, std::shared_ptr<Node> node, int slot) {
	if(doesEdgePreserveAcyclicity(std::static_pointer_cast<Node>(this->shared_from_this()), node) == false) throw LavErrorException(Lav_ERROR_CAUSES_CYCLE);
	auto &prop = node->getProperty(slot);
	auto conn = prop.getInputConnection();
	if(conn ==nullptr) throw LavErrorException(Lav_ERROR_CANNOT_CONNECT_TO_PROPERTY);
	auto outputConn =getOutputConnection(output);
	makeConnection(outputConn, conn);
}

void Node::disconnect(int which) {
	auto o =getOutputConnection(which);
	o->clear();
}

std::shared_ptr<Simulation> Node::getSimulation() {
	return simulation;
}

Property& Node::getProperty(int slot) {
	//first the forwarded case.
	if(forwarded_properties.count(slot) !=0) {
		auto n=std::get<0>(forwarded_properties[slot]).lock();
		auto s=std::get<1>(forwarded_properties[slot]);
		if(n) return n->getProperty(s);
		else throw LavErrorException(Lav_ERROR_INTERNAL); //better to crash here.
	}
	else if(properties.count(slot) == 0) throw LavErrorException(Lav_ERROR_RANGE);
	else return properties[slot];
}

void Node::forwardProperty(int ourProperty, std::shared_ptr<Node> toNode, int toProperty) {
	forwarded_properties[ourProperty] = std::make_tuple(toNode, toProperty);
}

void Node::stopForwardingProperty(int ourProperty) {
	if(forwarded_properties.count(ourProperty)) forwarded_properties.erase(ourProperty);
	else throw LavErrorException(Lav_ERROR_INTERNAL);
}

Event& Node::getEvent(int which) {
	if(events.count(which) == 0) throw LavErrorException(Lav_ERROR_RANGE);
	return events[which];
}

void Node::lock() {
	simulation->lock();
}

void Node::unlock() {
	simulation->unlock();
}

void Node::reset() {
}

//protected resize function.
void Node::resize(int newInputCount, int newOutputCount) {
	int oldInputCount = input_buffers.size();
	for(int i = oldInputCount-1; i >= newInputCount; i--) freeArray(input_buffers[i]);
	input_buffers.resize(newInputCount, nullptr);
	for(int i = oldInputCount; i < newInputCount; i++) input_buffers[i] = allocArray<float>(simulation->getBlockSize());

	int oldOutputCount = output_buffers.size();
	if(newOutputCount < oldOutputCount) { //we need to free some arrays.
		for(auto i = newOutputCount; i < oldOutputCount; i++) {
			freeArray(output_buffers[i]);
		}
	}
	//do the resize.
	output_buffers.resize(newOutputCount, nullptr);
	if(newOutputCount > oldOutputCount) { //we need to allocate some more arrays.
		for(auto i = oldOutputCount; i < newOutputCount; i++) {
			output_buffers[i] = allocArray<float>(simulation->getBlockSize());
		}
	}
}

std::set<std::shared_ptr<Node>> Node::getDependencies() {
	std::set<std::shared_ptr<Node>> retval;
	for(int i = 0; i < getInputConnectionCount(); i++) {
		auto j = getInputConnection(i)->getConnectedNodes();
		for(auto &p: j) {
			retval.insert(std::static_pointer_cast<Node>(p->shared_from_this()));
		}
	}
	for(auto &p: properties) {
		auto &prop = p.second;
		auto conn = prop.getInputConnection();
		if(conn) {
			for(auto n: conn->getConnectedNodes()) {
				retval.insert(std::static_pointer_cast<Node>(n->shared_from_this()));
			}
		}
	}
	return retval;
}

//LavSubgraphNode

SubgraphNode::SubgraphNode(int type, std::shared_ptr<Simulation> simulation): Node(type, simulation, 0, 0) {
}

void SubgraphNode::setInputNode(std::shared_ptr<Node> node) {
	subgraph_input= node;
}

void SubgraphNode::setOutputNode(std::shared_ptr<Node> node) {
	subgraph_output=node;
}

int SubgraphNode::getInputConnectionCount() {
	if(subgraph_input) return subgraph_input->getInputConnectionCount();
	else return 0;
}

std::shared_ptr<InputConnection> SubgraphNode::getInputConnection(int which) {
	if(which < 0|| which >= getInputConnectionCount()) throw LavErrorException(Lav_ERROR_RANGE);
	else return subgraph_input->getInputConnection(which);
}

int SubgraphNode::getOutputBufferCount() {
	if(subgraph_output) return subgraph_output->getOutputBufferCount();
	else return 0;
}

float** SubgraphNode::getOutputBufferArray() {
	if(subgraph_output) return subgraph_output->getOutputBufferArray();
	return nullptr;
}

void SubgraphNode::tick() {
	if(last_processed== simulation->getTickCount()) return;
	last_processed=simulation->getTickCount();
	if(getState() == Lav_NODESTATE_PAUSED) return;
	tickProperties();
	willProcessParents();
	if(subgraph_output == nullptr) return;
	subgraph_output->tick();
	//Handle our add and mul, on top of the output object of the subgraph.
	//We prefer this over forwarding because this allows the subgraph to change all internal volumes without them being overridden by the user.
	auto &mulProp = getProperty(Lav_NODE_MUL), &addProp = getProperty(Lav_NODE_ADD);
		float** outputs = getOutputBufferArray();	if(mulProp.needsARate()) {
		for(int i = 0; i < block_size; i++) {
			float mul = mulProp.getFloatValue(i);
			for(int j = 0; j < getOutputBufferCount(); j++) outputs[j][i]*=mul;
		}
	}
	else if(mulProp.getFloatValue() !=1.0) {
		for(int i = 0; i < getOutputBufferCount(); i++) {
			scalarMultiplicationKernel(block_size, mulProp.getFloatValue(), outputs[i], outputs[i]);
		}
	}
	if(addProp.needsARate()) {
		for(int i = 0; i < block_size; i++) {
		float add=addProp.getFloatValue(i);
			for(int j = 0; j < getOutputBufferCount(); j++) outputs[j][i]+=add;
		}
	}
	else if(addProp.getFloatValue() !=0.0) {
		for(int i = 0; i < getOutputBufferCount(); i++) {
			scalarAdditionKernel(block_size, addProp.getFloatValue(), outputs[i], outputs[i]);
		}
	}
}

//begin public api

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetSimulation(LavHandle handle, LavHandle* destination) {
	PUB_BEGIN
auto n = incomingObject<Node>(handle);
	*destination = outgoingObject(n->getSimulation());
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeConnect(LavHandle nodeHandle, int output, LavHandle destHandle, int input) {
	PUB_BEGIN
	auto node= incomingObject<Node>(nodeHandle);
	auto dest = incomingObject<Node>(destHandle);
	LOCK(*node);
	node->connect(output, dest, input);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeConnectSimulation(LavHandle nodeHandle, int output) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	node->connectSimulation(output);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeConnectProperty(LavHandle nodeHandle, int output, LavHandle otherHandle, int slot) {
	PUB_BEGIN
	auto n = incomingObject<Node>(nodeHandle);
	auto o = incomingObject<Node>(otherHandle);
	LOCK(*n);
	n->connectProperty(output, o, slot);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeDisconnect(LavHandle nodeHandle, int output) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	node->disconnect(output);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetInputConnectionCount(LavHandle nodeHandle, unsigned int* destination) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	*destination =node->getInputConnectionCount();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetOutputConnectionCount(LavHandle nodeHandle, unsigned int* destination) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	*destination = node->getOutputConnectionCount();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeReset(LavHandle nodeHandle) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	node->reset();
	PUB_END
}

//this is properties.
//this is here because properties do not "know" about objects and only objects have properties; also, it made properties.cpp have to "know" about simulations and objects.

//this works for getters and setters to lock the object and set a variable prop to be a pointer-like thing to a property.
#define PROP_PREAMBLE(n, s, t) auto node_ptr = incomingObject<Node>(n);\
LOCK(*node_ptr);\
auto &prop = node_ptr->getProperty((s));\
if(prop.getType() != (t)) {\
throw LavErrorException(Lav_ERROR_TYPE_MISMATCH);\
}

#define READONLY_CHECK if(prop.isReadOnly()) throw LavErrorException(Lav_ERROR_PROPERTY_IS_READ_ONLY);

Lav_PUBLIC_FUNCTION LavError Lav_nodeResetProperty(LavHandle nodeHandle, int slot) {
	PUB_BEGIN
	auto node_ptr = incomingObject<Node>(nodeHandle);
	LOCK(*node_ptr);
	auto prop = node_ptr->getProperty(slot);
	READONLY_CHECK
	prop.reset();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetIntProperty(LavHandle nodeHandle, int slot, int value) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT);
	READONLY_CHECK
	prop.setIntValue(value);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetFloatProperty(LavHandle nodeHandle, int slot, float value) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT);
	READONLY_CHECK
	prop.setFloatValue(value);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetDoubleProperty(LavHandle nodeHandle, int slot, double value) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_DOUBLE);
	READONLY_CHECK
	prop.setDoubleValue(value);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetStringProperty(LavHandle nodeHandle, int slot, char* value) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_STRING);
	READONLY_CHECK
	prop.setStringValue(value);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetFloat3Property(LavHandle nodeHandle, int slot, float v1, float v2, float v3) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT3);
	READONLY_CHECK
	prop.setFloat3Value(v1, v2, v3);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetFloat6Property(LavHandle nodeHandle, int slot, float v1, float v2, float v3, float v4, float v5, float v6) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT6);
	READONLY_CHECK
	prop.setFloat6Value(v1, v2, v3, v4, v5, v6);
	return Lav_ERROR_NONE;
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetIntProperty(LavHandle nodeHandle, int slot, int *destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT);
	*destination = prop.getIntValue();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloatProperty(LavHandle nodeHandle, int slot, float *destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT);
	*destination = prop.getFloatValue();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetDoubleProperty(LavHandle nodeHandle, int slot, double *destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_DOUBLE);
	*destination = prop.getDoubleValue();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetStringProperty(LavHandle nodeHandle, int slot, const char** destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_STRING);
	*destination = prop.getStringValue();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloat3Property(LavHandle nodeHandle, int slot, float* v1, float* v2, float* v3) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT3);
	auto val = prop.getFloat3Value();
	*v1 = val[0];
	*v2 = val[1];
	*v3 = val[2];
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloat6Property(LavHandle nodeHandle, int slot, float* v1, float* v2, float* v3, float* v4, float* v5, float* v6) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT6);
	auto val = prop.getFloat6Value();
	*v1 = val[0];
	*v2 = val[1];
	*v3 = val[2];
	*v4 = val[3];
	*v5 = val[4];
	*v6 = val[5];
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetIntPropertyRange(LavHandle nodeHandle, int slot, int* destination_lower, int* destination_upper) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT);
	*destination_lower = prop.getIntMin();
	*destination_upper = prop.getIntMax();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloatPropertyRange(LavHandle nodeHandle, int slot, float* destination_lower, float* destination_upper) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT);
	*destination_lower = prop.getFloatMin();
	*destination_upper = prop.getFloatMax();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetDoublePropertyRange(LavHandle nodeHandle, int slot, double* destination_lower, double* destination_upper) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_DOUBLE);
	*destination_lower = prop.getDoubleMin();
	*destination_upper = prop.getDoubleMax();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetPropertyType(LavHandle nodeHandle, int slot, int* destination) {
	PUB_BEGIN
	auto node= incomingObject<Node>(nodeHandle);
	LOCK(*node);
	auto &prop = node->getProperty(slot);
	*destination = prop.getType();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetPropertyName(LavHandle nodeHandle, int slot, char** destination) {
	PUB_BEGIN
	auto node_ptr = incomingObject<Node>(nodeHandle);
	LOCK(*node_ptr);
	auto prop = node_ptr->getProperty(slot);
	const char* n = prop.getName();
	char* dest = new char[strlen(n)+1]; //+1 for extra NULL.
	strcpy(dest, n);
	*destination = outgoingPointer<char>(std::shared_ptr<char>(dest,
	[](char* ptr) {delete[] ptr;}));
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetPropertyHasDynamicRange(LavHandle nodeHandle, int slot, int* destination) {
	PUB_BEGIN
	auto node = incomingObject<Node>(nodeHandle);
	LOCK(*node);
	auto &prop = node->getProperty(slot);
	*destination = prop.getHasDynamicRange();
	PUB_END
}

//array properties.

Lav_PUBLIC_FUNCTION LavError Lav_nodeReplaceFloatArrayProperty(LavHandle nodeHandle, int slot, unsigned int length, float* values) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT_ARRAY);
	READONLY_CHECK
	prop.replaceFloatArray(length, values);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeReadFloatArrayProperty(LavHandle nodeHandle, int slot, unsigned int index, float* destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT_ARRAY);
	*destination = prop.readFloatArray(index);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError  Lav_nodeWriteFloatArrayProperty(LavHandle nodeHandle, int slot, unsigned int start, unsigned int stop, float* values) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT_ARRAY);
	READONLY_CHECK
	prop.writeFloatArray(start, stop, values);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloatArrayPropertyDefault(LavHandle nodeHandle, int slot, unsigned int* destinationLength, float** destinationArray) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT_ARRAY);
	auto def = prop.getFloatArrayDefault();
	if(def.size() == 0) {
		*destinationLength = 0;
		*destinationArray = nullptr;
		return Lav_ERROR_NONE;
	}
	float* buff = new float[def.size()];
	std::copy(def.begin(), def.end(), buff);
	auto del = [](float* what){delete[] what;};
	auto outgoing_buff = std::shared_ptr<float>(buff, del);
	*destinationLength = def.size();
	*destinationArray = outgoingPointer<float>(outgoing_buff);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetFloatArrayPropertyLength(LavHandle nodeHandle, int slot, unsigned int* destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_FLOAT_ARRAY);
	*destination = prop.getFloatArrayLength();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeReplaceIntArrayProperty(LavHandle nodeHandle, int slot, unsigned int length, int* values) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT_ARRAY);
	READONLY_CHECK
	prop.replaceIntArray(length, values);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeReadIntArrayProperty(LavHandle nodeHandle, int slot, unsigned int index, int* destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT_ARRAY);
	*destination = prop.readIntArray(index);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError  Lav_nodeWriteIntArrayProperty(LavHandle nodeHandle, int slot, unsigned int start, unsigned int stop, int* values) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT_ARRAY);
	READONLY_CHECK
	prop.writeIntArray(start, stop, values);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetIntArrayPropertyDefault(LavHandle nodeHandle, int slot, unsigned int* destinationLength, int** destinationArray) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT_ARRAY);
	auto def = prop.getIntArrayDefault();
	if(def.size() == 0) {
		*destinationLength = 0;
		*destinationArray = nullptr;
		return Lav_ERROR_NONE;
	}
	int* buff = new int[def.size()];
	std::copy(def.begin(), def.end(), buff);
	auto del = [](int* what){delete[] what;};
	auto outgoing_buff = std::shared_ptr<int>(buff, del);
	*destinationLength = def.size();
	*destinationArray = outgoingPointer<int>(outgoing_buff);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetIntArrayPropertyLength(LavHandle nodeHandle, int slot, int* destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_INT_ARRAY);
	*destination = prop.getIntArrayLength();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetArrayPropertyLengthRange(LavHandle nodeHandle, int slot, unsigned int* destinationMin, unsigned int* destinationMax) {
	PUB_BEGIN
	auto ptr = incomingObject<Node>(nodeHandle);
	LOCK(*ptr);
	auto &prop = ptr->getProperty(slot);
	int type = prop.getType();
	if(type != Lav_PROPERTYTYPE_FLOAT_ARRAY || type != Lav_PROPERTYTYPE_INT_ARRAY) throw LavErrorException(Lav_ERROR_TYPE_MISMATCH);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetBufferProperty(LavHandle nodeHandle, int slot, LavHandle bufferHandle) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_BUFFER);
	auto buff=incomingObject<Buffer>(bufferHandle, true);
	prop.setBufferValue(buff);
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetBufferProperty(LavHandle nodeHandle, int slot, LavHandle* destination) {
	PUB_BEGIN
	PROP_PREAMBLE(nodeHandle, slot, Lav_PROPERTYTYPE_BUFFER);
	*destination = outgoingObject(prop.getBufferValue());
	PUB_END
}

//callback setup/configure/retrieval.
Lav_PUBLIC_FUNCTION LavError Lav_nodeGetEventHandler(LavHandle nodeHandle, int event, LavEventCallback *destination) {
	PUB_BEGIN
	auto ptr = incomingObject<Node>(nodeHandle);
	LOCK(*ptr);
	*destination = ptr->getEvent(event).getExternalHandler();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeGetEventUserDataPointer(LavHandle nodeHandle, int event, void** destination) {
	PUB_BEGIN
	auto ptr = incomingObject<Node>(nodeHandle);
	LOCK(*ptr);
	*destination = ptr->getEvent(event).getUserData();
	PUB_END
}

Lav_PUBLIC_FUNCTION LavError Lav_nodeSetEvent(LavHandle nodeHandle, int event, LavEventCallback handler, void* userData) {
	PUB_BEGIN
	auto ptr = incomingObject<Node>(nodeHandle);
	LOCK(*ptr);
	auto &ev = ptr->getEvent(event);
	if(handler) {
		ev.setHandler([=](Node* o, void* d) { handler(o->externalObjectHandle, d);});
		ev.setExternalHandler(handler);
		ev.setUserData(userData);
	} else {
		ev.setHandler(std::function<void(Node*, void*)>());
		ev.setExternalHandler(nullptr);
	}
	PUB_END
}

}