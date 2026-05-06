#include "core/channel_layout.hpp"

namespace tuxedo {
namespace channel_layout {

uint64_t default_for_channels(uint32_t channels) {
	switch(channels) {
		case 1: return kMono;
		case 2: return kStereo;
		case 3: return kFrontLeft | kFrontRight | kFrontCenter;
		case 4: return kFrontLeft | kFrontRight | kBackLeft | kBackRight;
		case 5: return kFrontLeft | kFrontRight | kFrontCenter | kBackLeft | kBackRight;
		case 6: return kFrontLeft | kFrontRight | kFrontCenter | kLfe | kSideLeft | kSideRight;
		case 7: return kFrontLeft | kFrontRight | kFrontCenter | kLfe | kBackCenter | kSideLeft | kSideRight;
		case 8: return kFrontLeft | kFrontRight | kFrontCenter | kLfe | kBackLeft | kBackRight | kSideLeft | kSideRight;
		default: return 0;
	}
}

std::vector<uint64_t> ordered_channels(uint64_t layout, uint32_t channels) {
	std::vector<uint64_t> out;
	out.reserve(channels);

	if(layout == 0) layout = default_for_channels(channels);

	static constexpr uint64_t kOrder[] = {
		kFrontLeft,
		kFrontRight,
		kFrontCenter,
		kLfe,
		kBackLeft,
		kBackRight,
		kFrontLeftOfCenter,
		kFrontRightOfCenter,
		kBackCenter,
		kSideLeft,
		kSideRight,
	};

	for(uint64_t flag : kOrder) {
		if(layout & flag) out.push_back(flag);
	}

	while(out.size() < channels) {
		if(out.empty()) out.push_back(kFrontLeft);
		else if(out.size() == 1) out.push_back(kFrontRight);
		else out.push_back(0);
	}
	if(out.size() > channels) out.resize(channels);
	return out;
}

} // namespace channel_layout
} // namespace tuxedo
