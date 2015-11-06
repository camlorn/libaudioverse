/**Copyright (C) Austin Hicks, 2014
This file is part of Libaudioverse, a library for 3D and environmental audio simulation, and is released under the terms of the Gnu General Public License Version 3 or (at your option) any later version.
A copy of the GPL, as well as other important copyright and licensing information, may be found in the file 'LICENSE' in the root of the Libaudioverse repository.  Should this file be missing or unavailable to you, see <http://www.gnu.org/licenses/>.*/
#pragma once
#include "sin_osc.hpp"
#include "../private/constants.hpp"
#include <vector>

namespace libaudioverse_implementation {

/**An additive square wave.
This is the most perfect and slowest method of making square waves, with absolutely no error.

The formula for a square wave is as follows:
sin(f)+sin(3f)/3+sin(5f)/5...
Where sin denotes sine waves.
*/

class AdditiveSquare {
	public:
	AdditiveSquare(float _sr);
	double tick();
	void reset();
	void setFrequency(float frequency);
	float getFrequency();
	void setPhase(double phase);
	double getPhase();
	//0 means autoadjust.
	void setHarmonics(int harmonics);
	int getHarmonics();
	private:
	void readjustHarmonics();
	std::vector<SinOsc> oscillators;
	SinOsc* oscillators_array; //Avoid taking the address of the first item of the vector repeatedly.
	int harmonics = 0, adjusted_harmonics = 0;
	float frequency = 100;
	float sr;
};

inline AdditiveSquare::AdditiveSquare(float _sr): sr(_sr) {
	//These trigger the recomputation logic.
	setHarmonics(0);
	setFrequency(100);
	readjustHarmonics();
}

inline double AdditiveSquare::tick() {
	double sum = 0.0;
	for(int i = 0; i < adjusted_harmonics; i++) sum += oscillators_array[i].tick()/(2*(i+1)-1);
	//4/PI comes from the Wikipedia definition of square wave. The second constant accounts for the Gibbs phenomenon.
	//The final term was derived experimentally, by figuring out what the maximum and minimum look like.
	//Without it, we overshoot very slightly, which is worse than undershooting very slightly.
	return sum*(4.0/PI)*(1.0/(1.0+2.0*WILBRAHAM_GIBBS))*(1.0/1.08013);
}

inline void AdditiveSquare::reset() {
	for(auto &i: oscillators) i.reset();
}

inline void AdditiveSquare::setFrequency(float frequency) {
	this->frequency = frequency;
	readjustHarmonics();
	for(int i = 0; i < adjusted_harmonics; i++) {
		oscillators_array[i].setFrequency(frequency*(2*(i+1)-1));
	}
}

inline float AdditiveSquare::getFrequency() {
	return frequency;
}

inline void AdditiveSquare::setPhase(double phase) {
	for(int i = 0; i < adjusted_harmonics; i++) oscillators_array[i].setPhase((2*(i+1)-1)*phase);
}

inline double AdditiveSquare::getPhase() {
	return oscillators_array[0].getPhase();
}

inline void AdditiveSquare::setHarmonics(int harmonics) {
	this->harmonics = harmonics;
	readjustHarmonics();
}

inline int AdditiveSquare::getHarmonics() {
	return harmonics;
}

inline void AdditiveSquare::readjustHarmonics() {
	int newHarmonics;
	if(harmonics == 0) {
		//Number of harmonics we can get between 0 and nyquist.
		newHarmonics = (int)((sr/2)/frequency);
		if(newHarmonics == 0) newHarmonics = 1;
	}
	else newHarmonics = harmonics;
	oscillators.resize(newHarmonics, SinOsc(sr));
	oscillators_array = &oscillators[0];
	//Partial setPhase.
	double p = getPhase();
	for(int i = adjusted_harmonics; i < newHarmonics; i++) oscillators_array[i].setPhase((2*(i+1)-1)*p);
	adjusted_harmonics = newHarmonics;
}

}