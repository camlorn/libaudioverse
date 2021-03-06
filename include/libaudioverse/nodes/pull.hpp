/* Copyright 2016 Libaudioverse Developers. See the COPYRIGHT
file at the top-level directory of this distribution.

Licensed under the mozilla Public License, version 2.0 <LICENSE.MPL2 or
https://www.mozilla.org/en-US/MPL/2.0/> or the Gbnu General Public License, V3 or later
<LICENSE.GPL3 or http://www.gnu.org/licenses/>, at your option. All files in the project
carrying such notice may not be copied, modified, or distributed except according to those terms. */
#pragma once
#include "../libaudioverse.h"
#include "../private/node.hpp"
#include <speex_resampler_cpp.hpp>
#include <memory>

namespace libaudioverse_implementation {

class Server;

class PullNode: public Node {
	public:
	PullNode(std::shared_ptr<Server> sim, unsigned int inputSr, unsigned int channels);
	~PullNode();
	void process();
	unsigned int input_sr = 0, channels = 0;
	std::shared_ptr<speex_resampler_cpp::Resampler> resampler = nullptr;
	float* incoming_buffer = nullptr, *resampled_buffer = nullptr;
	LavPullNodeAudioCallback callback = nullptr;
	void* callback_userdata = nullptr;
};

std::shared_ptr<Node> createPullNode(std::shared_ptr<Server> server, unsigned int inputSr, unsigned int channels);
}