#pragma once

#include "core/chain/node.hpp"

namespace tuxedo {

class DSPNode : public Node {
public:
	explicit DSPNode(Node *previous = nullptr, double latency_seconds = 0.10);
	~DSPNode() override = default;

	void set_should_continue(bool should_continue);
	double seconds_buffered() const;

protected:
	double max_buffered_seconds() const override { return latency_seconds_; }

private:
	double latency_seconds_ = 0.10;
};

} // namespace tuxedo
