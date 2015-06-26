/**Copyright (C) Austin Hicks, 2014
This file is part of Libaudioverse, a library for 3D and environmental audio simulation, and is released under the terms of the Gnu General Public License Version 3 or (at your option) any later version.
A copy of the GPL, as well as other important copyright and licensing information, may be found in the file 'LICENSE' in the root of the Libaudioverse repository.  Should this file be missing or unavailable to you, see <http://www.gnu.org/licenses/>.*/
#pragma once

namespace libaudioverse_implementation {

//Apparently, C's mod is in fact not the discrete math operation.
//This function handles that.
int ringmodi(int dividend, int divisor);
float ringmodf(float dividend, float divisor);
double ringmod(double dividend,double divisor);

//Work with decibals.
//These two are for gain conversions, and assume 0 db is 1.0.
double gainToDb(double gain);
double dbToGain(double gain);

double scalarToDb(double scalar, double reference);
double dbToScalar(double db, double reference);

//Run euclid's algorithm on two integers:
//Only works on positive integers.
int greatestCommonDivisor(int a, int b);

}