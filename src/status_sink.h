#pragma once

#include <string>

namespace agi {

class StatusSink {
public:
	virtual ~StatusSink() = default;
	virtual void ShowStatus(std::string const& message, int timeout_ms = 10000) = 0;
};

class NullStatusSink final : public StatusSink {
public:
	void ShowStatus(std::string const&, int) override { }
};

}
