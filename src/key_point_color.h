#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace aegisub::keypoint {

inline constexpr double approximate_squared_distance_error_margin = 16.0;

inline double LabTransfer(double value) noexcept {
	constexpr double epsilon = 0.008856;
	constexpr double offset = 16.0 / 116.0;
	constexpr double exponent = 1.0 / 3.0;
	return value > epsilon
		? std::pow(value, exponent)
		: 7.787 * value + offset;
}

struct LabColor {
	double l = 0.0;
	double a = 0.0;
	double b = 0.0;
};

inline LabColor BgrToLab(unsigned char b, unsigned char g, unsigned char r) noexcept {
	double const X = (0.412453 * r + 0.357580 * g + 0.180423 * b) / 255.0;
	double const Y = (0.212671 * r + 0.715160 * g + 0.072169 * b) / 255.0;
	double const Z = (0.019334 * r + 0.119193 * g + 0.950227 * b) / 255.0;
	double const xr = X / 0.950456;
	double const yr = Y / 1.000;
	double const zr = Z / 1.088854;

	double const fx = LabTransfer(xr);
	double const fy = LabTransfer(yr);
	double const fz = LabTransfer(zr);
	constexpr double epsilon = 0.008856;
	return {
		yr > epsilon ? 116.0 * fy - 16.0 : 903.3 * yr,
		500.0 * (fx - fy),
		200.0 * (fy - fz)
	};
}

inline double ApproximateLabTransfer(double value) noexcept {
	// The Y channel can exceed one by a few ppm because its coefficients round up.
	constexpr std::size_t segments = 4096;
	constexpr double maximum = 1.00001;
	static std::array<double, segments + 1> const table = [] {
		std::array<double, segments + 1> result{};
		for (std::size_t i = 0; i <= segments; ++i)
			result[i] = LabTransfer(maximum * static_cast<double>(i) / segments);
		return result;
	}();

	value = std::clamp(value, 0.0, maximum);
	double const position = value * (static_cast<double>(segments) / maximum);
	std::size_t const index = std::min(
		static_cast<std::size_t>(position),
		segments - 1);
	double const fraction = position - static_cast<double>(index);
	return std::lerp(table[index], table[index + 1], fraction);
}

inline LabColor ApproximateBgrToLab(unsigned char b, unsigned char g, unsigned char r) noexcept {
	double const X = (0.412453 * r + 0.357580 * g + 0.180423 * b) / 255.0;
	double const Y = (0.212671 * r + 0.715160 * g + 0.072169 * b) / 255.0;
	double const Z = (0.019334 * r + 0.119193 * g + 0.950227 * b) / 255.0;
	double const xr = X / 0.950456;
	double const yr = Y / 1.000;
	double const zr = Z / 1.088854;
	double const fx = ApproximateLabTransfer(xr);
	double const fy = ApproximateLabTransfer(yr);
	double const fz = ApproximateLabTransfer(zr);
	constexpr double epsilon = 0.008856;
	return {
		yr > epsilon ? 116.0 * fy - 16.0 : 903.3 * yr,
		500.0 * (fx - fy),
		200.0 * (fy - fz)
	};
}

inline double DistanceSquared(LabColor const& left, LabColor const& right) noexcept {
	double const delta_l = left.l - right.l;
	double const delta_a = left.a - right.a;
	double const delta_b = left.b - right.b;
	return delta_l * delta_l + delta_a * delta_a + delta_b * delta_b;
}

inline bool MatchesBgr(
	LabColor const& reference,
	unsigned char b,
	unsigned char g,
	unsigned char r,
	double tolerance_squared) noexcept {
	return DistanceSquared(BgrToLab(b, g, r), reference) <= tolerance_squared;
}

inline bool MatchesBgrFast(
	LabColor const& reference,
	unsigned char b,
	unsigned char g,
	unsigned char r,
	double tolerance_squared) noexcept {
	// Linear interpolation is below 5e-6 from the transfer function: the cbrt
	// branch has |f''| <= 587 and the segment width is below 0.000245. Applied
	// to the legal Lab component ranges, squared-distance error is below 10.2.
	// Keep a wider margin and use the exact path for values inside it.
	double const approximate_distance =
		DistanceSquared(ApproximateBgrToLab(b, g, r), reference);
	if (approximate_distance + approximate_squared_distance_error_margin <= tolerance_squared)
		return true;
	if (approximate_distance - approximate_squared_distance_error_margin > tolerance_squared)
		return false;
	return MatchesBgr(reference, b, g, r, tolerance_squared);
}

class ColorMatcher {
	static constexpr std::uint32_t invalid_key = 0x01000000U;
	// A key-point scan typically sees a small palette; keep setup below 1 KiB.
	static constexpr std::size_t cache_bits = 6;
	static constexpr std::size_t cache_size = std::size_t{ 1 } << cache_bits;

	struct CacheEntry {
		std::uint32_t key = invalid_key;
		bool matches = false;
	};

	LabColor reference;
	double tolerance_squared;
	std::array<CacheEntry, cache_size> cache{};

	static std::uint32_t PackBgr(unsigned char b, unsigned char g, unsigned char r) noexcept {
		return static_cast<std::uint32_t>(b)
			| (static_cast<std::uint32_t>(g) << 8)
			| (static_cast<std::uint32_t>(r) << 16);
	}

	static std::size_t CacheIndex(std::uint32_t key) noexcept {
		return static_cast<std::size_t>((key * 0x9E3779B1U) >> (32 - cache_bits));
	}

public:
	ColorMatcher(
		unsigned char reference_b,
		unsigned char reference_g,
		unsigned char reference_r,
		double tolerance_squared) noexcept
	: reference(BgrToLab(reference_b, reference_g, reference_r))
	, tolerance_squared(tolerance_squared) {
		auto const key = PackBgr(reference_b, reference_g, reference_r);
		cache[CacheIndex(key)] = { key, true };
	}

	bool Matches(unsigned char b, unsigned char g, unsigned char r) noexcept {
		auto const key = PackBgr(b, g, r);
		auto& entry = cache[CacheIndex(key)];
		if (entry.key == key)
			return entry.matches;

		bool const matches = MatchesBgrFast(reference, b, g, r, tolerance_squared);
		entry = { key, matches };
		return matches;
	}

};

}  // namespace aegisub::keypoint
