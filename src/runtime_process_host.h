#pragma once

#include <functional>

struct RuntimeProcessHost {
	std::function<void()> prime_process_logging;
};
