#pragma once

#include <atomic>
#include <cstdint>

class OperationTokenSource {
public:
	struct Token {
		uint64_t generation = 0;
		OperationTokenSource const* source = nullptr;

		bool IsValid() const {
			return source && source->IsCurrent(generation);
		}

		friend bool operator==(Token const& lhs, Token const& rhs) = default;
	};

	Token Issue() const {
		return { generation_.load(std::memory_order_acquire), this };
	}

	void Supersede() {
		generation_.fetch_add(1, std::memory_order_release);
	}

	uint64_t CurrentGeneration() const {
		return generation_.load(std::memory_order_acquire);
	}

	bool IsCurrent(uint64_t expected_generation) const {
		return generation_.load(std::memory_order_acquire) == expected_generation;
	}

private:
	std::atomic<uint64_t> generation_{ 0 };
};
