#include "core/chain/dsp_node.hpp"

namespace tuxedo {

DSPNode::DSPNode(Node *previous, double latency_seconds)
: Node(previous), latency_seconds_(latency_seconds) {}

void DSPNode::set_should_continue(bool should_continue) {
	if(should_continue) return;
	request_stop();
}

double DSPNode::seconds_buffered() const {
	return const_cast<DSPNode *>(this)->Node::seconds_buffered();
}

} // namespace tuxedo
