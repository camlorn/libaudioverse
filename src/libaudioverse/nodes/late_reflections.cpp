/**Copyright (C) Austin Hicks, 2014
This file is part of Libaudioverse, a library for 3D and environmental audio simulation, and is released under the terms of the Gnu General Public License Version 3 or (at your option) any later version.
A copy of the GPL, as well as other important copyright and licensing information, may be found in the file 'LICENSE' in the root of the Libaudioverse repository.  Should this file be missing or unavailable to you, see <http://www.gnu.org/licenses/>.*/

#include <math.h>
#include <stdlib.h>
#include <libaudioverse/libaudioverse.h>
#include <libaudioverse/libaudioverse_properties.h>
#include <libaudioverse/private/node.hpp>
#include <libaudioverse/private/simulation.hpp>
#include <libaudioverse/private/properties.hpp>
#include <libaudioverse/private/data.hpp>
#include <libaudioverse/private/dspmath.hpp>
#include <libaudioverse/implementations/feedback_delay_network.hpp>
#include <libaudioverse/implementations/sin_osc.hpp>
#include <libaudioverse/implementations/delayline.hpp>
#include <libaudioverse/private/macros.hpp>
#include <libaudioverse/private/memory.hpp>
#include <libaudioverse/private/kernels.hpp>
#include <libaudioverse/implementations/biquad.hpp>
#include <limits>
#include <algorithm>
#include <random>

namespace libaudioverse_implementation {

/**Algorithm explanation:
This algorithm consists of a FDN and two highshelf filters inserted in the feedback:
fdn->mid_highshelf->high_highshelf->modulatable_allpasses->fdn

We compute individual gains for each line, using math taken from Physical Audio Processing by JOS.

These gains are the "low band", and two shelving filters are then applied to shape the remaining two bands.

Unfortunately, the biquad formulas for lowshelf are unstable at low frequencies, something which needs debugging.
Consequently, we have to start from the lowest band and move up with highshelves.

The delay lines have coprime lengths.

In order to increase the accuracy of panning, only 16 unique delay line lengths are used.
Each delay is copied to fill a range, namely order/16, of adjacent lines.
*/

//The order must be 16.
const int order= 16;

//Used for computing the delay line lengths.
//A set of coprime integers.
int coprimes[] = {
	3, 4, 5, 7,
	9, 11, 13, 16,
	17, 19, 23, 27,
	29, 31, 35, 37
};

class LateReflectionsNode: public Node {
	public:
	LateReflectionsNode(std::shared_ptr<Simulation> simulation);
	~LateReflectionsNode();
	virtual void process();
	void recompute();
	void amplitudeModulationFrequencyChanged();
	void delayModulationFrequencyChanged();
	void allpassModulationFrequencyChanged();
	void allpassEnabledChanged();
	void normalizeOscillators();
	void reset() override;
	FeedbackDelayNetwork<InterpolatedDelayLine> fdn;
	float* delays = nullptr;
	float *gains;
	float* output_frame=nullptr, *next_input_frame =nullptr;
	float* fdn_matrix = nullptr;
	//Filters for the band separation.
	BiquadFilter** highshelves; //Shapes from mid to high band.
	BiquadFilter** midshelves; //Shapes from low to mid band.
	//The modulatable allpasses.
	BiquadFilter** allpasses;
	//used for amplitude modulation.
	SinOsc **amplitude_modulators;
	//Used to optimize by allowing us to use SSE.
	float* amplitude_modulation_buffer;
	//Modulate the internal delay lines.
	SinOsc **delay_modulators;
	//Allpass modulators.
	SinOsc **allpass_modulators;
	//used to reduce panning effects introduced by the varying delay line lengths.
	InterpolatedDelayLine **pan_reducers;
};

LateReflectionsNode::LateReflectionsNode(std::shared_ptr<Simulation> simulation):
Node(Lav_OBJTYPE_LATE_REFLECTIONS_NODE, simulation, order, order),
fdn(order, 1.0f, simulation->getSr()) {
	for(int i=0; i < order; i++) {
		appendInputConnection(i, 1);
		appendOutputConnection(i, 1);
	}
	fdn_matrix=allocArray<float>(order*order);
	//get a hadamard.
	hadamard(order, fdn_matrix);
	//feed the fdn the initial matrix.
	fdn.setMatrix(fdn_matrix);
	gains=allocArray<float>(order);
	output_frame=allocArray<float>(order);
	next_input_frame=allocArray<float>(order);
	delays=allocArray<float>(order);
	//range for hf and lf.
	double nyquist=simulation->getSr()/2.0;
	getProperty(Lav_LATE_REFLECTIONS_HF_REFERENCE).setFloatRange(0.0, nyquist);
	getProperty(Lav_LATE_REFLECTIONS_LF_REFERENCE).setFloatRange(0.0, nyquist);
	//allocate the filters.
	highshelves=new BiquadFilter*[order];
	midshelves = new BiquadFilter*[order];
	allpasses=new BiquadFilter*[order];
	for(int i = 0; i < order; i++) {
		highshelves[i] = new BiquadFilter(simulation->getSr());
		midshelves[i] = new BiquadFilter(simulation->getSr());
		allpasses[i] = new BiquadFilter(simulation->getSr());
	}
	amplitude_modulators = new SinOsc*[order]();
	delay_modulators=new SinOsc*[order];
	allpass_modulators = new SinOsc*[order];
	for(int i = 0; i < order; i++) {
		amplitude_modulators[i] = new SinOsc(simulation->getSr());
		amplitude_modulators[i]->setPhase((float)i/order);
		delay_modulators[i] = new SinOsc(simulation->getSr());
		delay_modulators[i]->setPhase((float)i/order);
		allpass_modulators[i] = new SinOsc(simulation->getSr());
		allpass_modulators[i]->setPhase((float)i/order);
	}
	amplitude_modulation_buffer=allocArray<float>(simulation->getBlockSize());
	pan_reducers=new InterpolatedDelayLine*[order]();
	for(int i=0; i < order; i++) {
		pan_reducers[i] = new InterpolatedDelayLine(1.0f, simulation->getSr());
	}
	//initial configuration.
	recompute();
}

std::shared_ptr<Node> createLateReflectionsNode(std::shared_ptr<Simulation> simulation) {
	std::shared_ptr<LateReflectionsNode> retval = std::shared_ptr<LateReflectionsNode>(new LateReflectionsNode(simulation), ObjectDeleter(simulation));
	simulation->associateNode(retval);
	return retval;
}

LateReflectionsNode::~LateReflectionsNode() {
	freeArray(gains);
	freeArray(output_frame);
	freeArray(next_input_frame);
	freeArray(delays);
	freeArray(amplitude_modulation_buffer);
	for(int i=0; i < order; i++) {
		delete highshelves[i];
		delete midshelves[i];
		delete amplitude_modulators[i];
		delete delay_modulators[i];
		delete allpass_modulators[i];
		delete pan_reducers[i];
	}
	delete[] highshelves;
	delete[] midshelves;
	delete[] amplitude_modulators;
	delete[] delay_modulators;
	delete[] allpass_modulators;
	delete[] pan_reducers;
}

double t60ToGain(double t60, double lineLength) {
	double dbPerSec= -60.0/t60;
	//Db decrease for one circulation of the delay line.
	double dbPerPeriod = dbPerSec*lineLength;
	//convert to a gain.
	return pow(10, dbPerPeriod/20.0);
}

void LateReflectionsNode::recompute() {
	float density = getProperty(Lav_LATE_REFLECTIONS_DENSITY).getFloatValue();
	float t60=getProperty(Lav_LATE_REFLECTIONS_T60).getFloatValue();
	float t60_high =getProperty(Lav_LATE_REFLECTIONS_HF_T60).getFloatValue();
	float t60_low =getProperty(Lav_LATE_REFLECTIONS_LF_T60).getFloatValue();
	float hf_reference=getProperty(Lav_LATE_REFLECTIONS_HF_REFERENCE).getFloatValue();
	float lf_reference = getProperty(Lav_LATE_REFLECTIONS_LF_REFERENCE).getFloatValue();
	//The base delay is the amount we are delaying all delay lines by.
	float baseDelay = 0.003+(1.0f-density)*0.025;
	//Approximate delay line lengths using powers of primes.
	for(int i = 0; i < 16; i+=1) {
		//0, 4, 8, 12, 1, 5, 9, 13...
		int prime= coprimes[(i%4)*4+i/4];
		//use change of base.
		double powerApprox = log(baseDelay*simulation->getSr())/log(prime);
		int neededPower=round(powerApprox);
		double delayInSamples = pow(prime, neededPower);
		double delay=delayInSamples/simulation->getSr();
		delay = std::min(delay, 1.0);
		delays[i] = delay;
	}
	//The following two lines were determined experimentaly, and greatly reduce metallicness.
	//This is probably because, by default, the shortest and longest delay line are adjacent and this node  is typically used with panners at the input and output.
	std::swap(delays[0], delays[15]);
	std::swap(delays[1], delays[14]);
	fdn.setDelays(delays);
	//configure the gains.
	for(int i= 0; i < order; i++) {
		gains[i] = t60ToGain(t60_low, delays[i]);
	}
	//Configure the filters.
	for(int i = 0; i < order; i++) {
		//We get the mid and high t60 gains, and turn them into db.
		double highGain=t60ToGain(t60_high, delays[i]);
		double midGain=t60ToGain(t60, delays[i]);
		double midDb=scalarToDb(midGain, gains[i]);
		double highDb = scalarToDb(highGain, midGain);
		//Careful reading of the audio eq cookbook reveals that when s=1, q is always sqrt(2).
		//We add a very tiny bit to help against numerical error.
		highshelves[i]->configure(Lav_BIQUAD_TYPE_HIGHSHELF, hf_reference, highDb, 1/sqrt(2.0)+1e-4);
		midshelves[i]->configure(Lav_BIQUAD_TYPE_HIGHSHELF, lf_reference, midDb, 1.0/sqrt(2.0)+1e-4);
	}
	//Finally, bake the gains into the fdn matrix:
	hadamard(order, fdn_matrix);
	for(int i=0; i < order; i++) {
		for(int j = 0; j < order; j++) {
			fdn_matrix[i*order+j]*=gains[i];
		}
	}
	fdn.setMatrix(fdn_matrix);
	//Reduce the panning effect.
	//Explanation: the first sample of output should reach all of the 16 outputs at the same time, before degrading normally.
	//This offset basically helps the reflections feel more "centered" when all channels are fed by the source.
	//We add one sample here so that we never have a delay of 0, which reduces some possible compatability issues with delay lines.
	double panReductionDelay = *std::max_element(delays, delays+order)+1.0/simulation->getSr();
	for(int i=0; i < order; i++) {
		double neededDelay = panReductionDelay-delays[i];
		pan_reducers[i]->setDelay(neededDelay);
	}
}

void LateReflectionsNode::amplitudeModulationFrequencyChanged() {
	float freq=getProperty(Lav_LATE_REFLECTIONS_AMPLITUDE_MODULATION_FREQUENCY).getFloatValue();
	for(int i = 0; i < order; i++) {
		amplitude_modulators[i]->setFrequency(freq);
	}
}

void LateReflectionsNode::delayModulationFrequencyChanged() {
	float freq=getProperty(Lav_LATE_REFLECTIONS_DELAY_MODULATION_FREQUENCY).getFloatValue();
	for(int i = 0; i < order; i++) {
		delay_modulators[i]->setFrequency(freq);
	}
}

void LateReflectionsNode::allpassModulationFrequencyChanged() {
	float freq = getProperty(Lav_LATE_REFLECTIONS_ALLPASS_MODULATION_FREQUENCY).getFloatValue();
	for(int i= 0; i < order; i++) {
		allpass_modulators[i]->setFrequency(freq);
	}
}

void LateReflectionsNode::allpassEnabledChanged() {
	for(int i= 0; i < order; i++) {
		allpasses[i]->clearHistories();
	}
}

void LateReflectionsNode::normalizeOscillators() {
	for(int i = 0; i < order; i++) {
		amplitude_modulators[i]->normalize();
		delay_modulators[i]->normalize();
	}
}

void LateReflectionsNode::process() {
	if(werePropertiesModified(this,
	Lav_LATE_REFLECTIONS_T60, Lav_LATE_REFLECTIONS_DENSITY, Lav_LATE_REFLECTIONS_HF_T60,
	Lav_LATE_REFLECTIONS_LF_T60, Lav_LATE_REFLECTIONS_HF_REFERENCE, Lav_LATE_REFLECTIONS_LF_REFERENCE
	)) recompute();
	if(werePropertiesModified(this, Lav_LATE_REFLECTIONS_AMPLITUDE_MODULATION_FREQUENCY)) amplitudeModulationFrequencyChanged();
	if(werePropertiesModified(this, Lav_LATE_REFLECTIONS_DELAY_MODULATION_FREQUENCY)) delayModulationFrequencyChanged();
	if(werePropertiesModified(this, Lav_LATE_REFLECTIONS_ALLPASS_ENABLED)) allpassEnabledChanged();
	if(werePropertiesModified(this, Lav_LATE_REFLECTIONS_ALLPASS_MODULATION_FREQUENCY)) allpassModulationFrequencyChanged();
	normalizeOscillators();
	float amplitudeModulationDepth = getProperty(Lav_LATE_REFLECTIONS_AMPLITUDE_MODULATION_DEPTH).getFloatValue();
	float delayModulationDepth = getProperty(Lav_LATE_REFLECTIONS_DELAY_MODULATION_DEPTH).getFloatValue();
	float allpassMinFreq=getProperty(Lav_LATE_REFLECTIONS_ALLPASS_MINFREQ).getFloatValue();
	float allpassMaxFreq = getProperty(Lav_LATE_REFLECTIONS_ALLPASS_MAXFREQ).getFloatValue();
	float allpassQ=getProperty(Lav_LATE_REFLECTIONS_ALLPASS_Q).getFloatValue();
	bool allpassEnabled = getProperty(Lav_LATE_REFLECTIONS_ALLPASS_ENABLED).getIntValue() == 1;
	float allpassDelta= (allpassMaxFreq-allpassMinFreq)/2.0f;
	//we move delta upward and delta downward of this point.
	//consequently, we therefore range from the min to the max.
	float allpassModulationStart =allpassMinFreq+allpassDelta;
	for(int i= 0; i < block_size; i++) {
		//We modulate the delay lines first.
		for(int modulating = 0; modulating < 16; modulating++) {
			float delay = delays[modulating];
			delay =delay+delay*delayModulationDepth*delay_modulators[modulating]->tick();
			delay = std::min(delay, 1.0f);
			fdn.setDelay(modulating, delay);
		}
		//Prepare the allpasses, if enabled.
		if(allpassEnabled) {
			for(int modulating =0; modulating < order; modulating++) {
				allpasses[modulating]->configure(Lav_BIQUAD_TYPE_ALLPASS, allpassModulationStart+allpassDelta*allpass_modulators[modulating]->tick(), 0.0, allpassQ);
			}
		}
		//If disabled, the modulators are advanced later.
		//Get the fdn's output.
		fdn.computeFrame(output_frame);
		for(int j= 0; j < order; j++) output_buffers[j][i] = output_frame[j];
		for(int j=0; j < order; j++)  {
			//Through the highshelf, then the lowshelf.
			output_frame[j] = midshelves[j]->tick(highshelves[j]->tick(gains[j]*output_frame[j]));
			//and maybe through the allpass
			if(allpassEnabled) output_frame[j] = allpasses[j]->tick(output_frame[j]);
		}
		//Gains are baked into the fdn matrix.
		//bring in the inputs.
		for(int j = 0; j < order; j++) next_input_frame[j] = input_buffers[j][i];
		fdn.advance(next_input_frame, output_frame);
	}
	//appluy the amplitude modulation, if it's needed.
	if(amplitudeModulationDepth!=0.0f) {
		for(int output = 0; output < num_output_buffers; output++) {
			float* output_buffer=output_buffers[output];
			SinOsc& osc= *amplitude_modulators[output];
			//get  A sine wave.
			osc.fillBuffer(block_size, amplitude_modulation_buffer);
			//Implement 1.0-amplitudeModulationDepth/2+amplitudeModulationDepth*oscillatorValue.
			scalarMultiplicationKernel(block_size, amplitudeModulationDepth, amplitude_modulation_buffer, amplitude_modulation_buffer);
			scalarAdditionKernel(block_size, 1.0f-amplitudeModulationDepth/2.0f, amplitude_modulation_buffer, amplitude_modulation_buffer);
			//Apply the modulation.
			multiplicationKernel(block_size, amplitude_modulation_buffer, output_buffer, output_buffer);
		}
	}
	//Advance modulators for anything we aren't modulating:
	//We do this so that the same parameters always produce the same reverb, even after transitioning through multiple presets.
	//Without the following, the modulators for different stages can get out of phase with each other.
	if(allpassEnabled == false) {
		for(int i=0; i < order; i++)allpass_modulators[i]->skipSamples(block_size);
	}
	if(amplitudeModulationDepth == 0.0f) {
		for(int i = 0; i < 16; i++) {
			amplitude_modulators[i]->skipSamples(block_size);
		}
	}
	//Apply the pan reduction:
	for(int i = 0; i < order; i++) {
		auto &line = *pan_reducers[i];
		for(int j = 0; j < block_size; j++) {
			output_buffers[i][j] = line.tick(output_buffers[i][j]);
		}
	}
}

void LateReflectionsNode::reset() {
	fdn.reset();
	for(int i = 0; i < order; i++) {
		midshelves[i]->clearHistories();
		highshelves[i]->clearHistories();
		allpasses[i]->clearHistories();
		amplitude_modulators[i]->setPhase((float)i/order);
		delay_modulators[i]->setPhase((float)i/order);
		allpass_modulators[i]->setPhase((float)i/order);
	}
}

//begin public api

Lav_PUBLIC_FUNCTION LavError Lav_createLateReflectionsNode(LavHandle simulationHandle, LavHandle* destination) {
	PUB_BEGIN
	auto simulation = incomingObject<Simulation>(simulationHandle);
	LOCK(*simulation);
	auto retval = createLateReflectionsNode(simulation);
	*destination = outgoingObject<Node>(retval);
	PUB_END
}

}