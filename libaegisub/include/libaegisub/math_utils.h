#pragma once

#include <numeric>
#include <utility>

namespace agi::util {
inline std::pair<int, int> reduce_ratio(int numerator, int denominator) noexcept {
	auto divisor = std::gcd(numerator, denominator);
	if (divisor == 0)
		return {numerator, denominator};
	return {numerator / divisor, denominator / divisor};
}
}
