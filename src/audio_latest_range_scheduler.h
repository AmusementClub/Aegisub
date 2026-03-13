#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class AudioLatestRangeScheduler {
public:
	using Callback = std::function<void(size_t, size_t, uint64_t)>;

	explicit AudioLatestRangeScheduler(Callback callback);
	~AudioLatestRangeScheduler();

	void Request(size_t first, size_t last);
	void Invalidate();
	uint64_t CurrentGeneration() const;
	bool IsCurrent(uint64_t request_generation) const;

private:
	class Impl;
	std::unique_ptr<Impl> impl;
};
