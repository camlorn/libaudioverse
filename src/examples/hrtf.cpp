/* Copyright 2016 Libaudioverse Developers. See the COPYRIGHT
file at the top-level directory of this distribution.

Licensed under the mozilla Public License, version 2.0 <LICENSE.MPL2 or
https://www.mozilla.org/en-US/MPL/2.0/> or the Gbnu General Public License, V3 or later
<LICENSE.GPL3 or http://www.gnu.org/licenses/>, at your option. All files in the project
carrying such notice may not be copied, modified, or distributed except according to those terms. */

/**Demonstrates the hrtf node.*/
#include <libaudioverse/libaudioverse.h>
#include <libaudioverse/libaudioverse_properties.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ERRCHECK(x) do {\
if((x) != Lav_ERROR_NONE) {\
	printf(#x " errored: %i", (x));\
	Lav_shutdown();\
	return 1;\
}\
} while(0)\

int main(int argc, char** args) {
	if(argc != 3) {
		printf("Usage: %s <sound file> <hrtf file>", args[0]);
		return 1;
	}
	LavHandle server;
	LavHandle bufferNode, hrtfNode, limit;
	ERRCHECK(Lav_initialize());
	ERRCHECK(Lav_createServer(44100, 1024, &server));
	ERRCHECK(Lav_serverSetOutputDevice(server, "default", 2, 2));
	ERRCHECK(Lav_createBufferNode(server, &bufferNode));
	LavHandle buffer;
	ERRCHECK(Lav_createBuffer(server, &buffer));
	ERRCHECK(Lav_bufferLoadFromFile(buffer, args[1]));
	ERRCHECK(Lav_nodeSetBufferProperty(bufferNode, Lav_BUFFER_BUFFER, buffer));
	ERRCHECK(Lav_nodeSetIntProperty(bufferNode, Lav_BUFFER_LOOPING, 1));
	ERRCHECK(Lav_createHrtfNode(server, args[2], &hrtfNode));
	ERRCHECK(Lav_nodeConnect(bufferNode, 0, hrtfNode, 0));
	ERRCHECK(Lav_createHardLimiterNode(server, 2, &limit));
	ERRCHECK(Lav_nodeConnect(hrtfNode, 0, limit, 0));
	ERRCHECK(Lav_nodeConnectServer(limit, 0));
	int shouldContinue = 1;
	printf("Enter pairs of numbers separated by whitespace, where the first is azimuth (anything) and the second\n"
"is elevation (-90 to 90).\n"
"Input q to quit.\n");
	char command[512] = "";
	float elev = 0, az = 0;
	int elevOrAz = 0;
	while(shouldContinue) {
		scanf("%s", command);
		if(command[0] == 'q') {
			shouldContinue = 0;
			continue;
		}
		if(elevOrAz == 0) {
			sscanf(command, "%f", &az);
			elevOrAz = 1;
			continue;
		}
		else if(elevOrAz == 1) {
			sscanf(command, "%f", &elev);
			ERRCHECK(Lav_nodeSetFloatProperty(hrtfNode, Lav_PANNER_ELEVATION, elev));
			ERRCHECK(Lav_nodeSetFloatProperty(hrtfNode, Lav_PANNER_AZIMUTH, az));
			elevOrAz = 0;
			continue;
		}
	}
	Lav_shutdown();
	return 0;
}
