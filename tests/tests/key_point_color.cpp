#include <gtest/gtest.h>

#include "../../src/key_point_color.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

aegisub::keypoint::LabColor BaselineBgrToLab(
	unsigned char b,
	unsigned char g,
	unsigned char r) {
	double const X = (0.412453 * r + 0.357580 * g + 0.180423 * b) / 255.0;
	double const Y = (0.212671 * r + 0.715160 * g + 0.072169 * b) / 255.0;
	double const Z = (0.019334 * r + 0.119193 * g + 0.950227 * b) / 255.0;
	double const xr = X / 0.950456;
	double const yr = Y / 1.000;
	double const zr = Z / 1.088854;

	aegisub::keypoint::LabColor lab;
	if (yr > 0.008856)
		lab.l = 116.0 * std::pow(yr, 1.0 / 3.0) - 16.0;
	else
		lab.l = 903.3 * yr;

	double const fx = xr > 0.008856 ? std::pow(xr, 1.0 / 3.0) : 7.787 * xr + 16.0 / 116.0;
	double const fy = yr > 0.008856 ? std::pow(yr, 1.0 / 3.0) : 7.787 * yr + 16.0 / 116.0;
	double const fz = zr > 0.008856 ? std::pow(zr, 1.0 / 3.0) : 7.787 * zr + 16.0 / 116.0;

	lab.a = 500.0 * (fx - fy);
	lab.b = 200.0 * (fy - fz);
	return lab;
}

}  // namespace

TEST(key_point_color, optimized_conversion_matches_baseline) {
	for (int r = 0; r <= 255; r += 17) {
		for (int g = 0; g <= 255; g += 17) {
			for (int b = 0; b <= 255; b += 17) {
				auto const expected = BaselineBgrToLab(
					static_cast<unsigned char>(b),
					static_cast<unsigned char>(g),
					static_cast<unsigned char>(r));
				auto const actual = aegisub::keypoint::BgrToLab(
					static_cast<unsigned char>(b),
					static_cast<unsigned char>(g),
					static_cast<unsigned char>(r));

				EXPECT_DOUBLE_EQ(expected.l, actual.l);
				EXPECT_DOUBLE_EQ(expected.a, actual.a);
				EXPECT_DOUBLE_EQ(expected.b, actual.b);
			}
		}
	}
}

TEST(key_point_color, cached_matcher_matches_uncached_results) {
	constexpr unsigned char reference_b = 40;
	constexpr unsigned char reference_g = 80;
	constexpr unsigned char reference_r = 120;
	constexpr double tolerance_squared = 20.0 * 20.0;

	auto const reference = aegisub::keypoint::BgrToLab(reference_b, reference_g, reference_r);
	aegisub::keypoint::ColorMatcher matcher(
		reference_b,
		reference_g,
		reference_r,
		tolerance_squared);

	std::mt19937 random(0xA391U);
	struct Sample {
		unsigned char b;
		unsigned char g;
		unsigned char r;
		bool expected;
	};
	std::vector<Sample> samples;
	samples.reserve(50000);
	for (int i = 0; i < 50000; ++i) {
		auto const b = static_cast<unsigned char>(random());
		auto const g = static_cast<unsigned char>(random());
		auto const r = static_cast<unsigned char>(random());
		bool const expected = aegisub::keypoint::MatchesBgr(
			reference,
			b,
			g,
			r,
			tolerance_squared);
		EXPECT_EQ(expected, matcher.Matches(b, g, r));
		EXPECT_EQ(expected, matcher.Matches(b, g, r));
		samples.push_back({ b, g, r, expected });
	}
	for (auto it = samples.rbegin(); it != samples.rend(); ++it)
		EXPECT_EQ(it->expected, matcher.Matches(it->b, it->g, it->r));
}

TEST(key_point_color, sampled_transfer_lut_results_stay_within_the_exact_match_margin) {
	std::mt19937 random(0x41B2U);
	for (int i = 0; i < 100000; ++i) {
		auto const reference_b = static_cast<unsigned char>(random());
		auto const reference_g = static_cast<unsigned char>(random());
		auto const reference_r = static_cast<unsigned char>(random());
		auto const b = static_cast<unsigned char>(random());
		auto const g = static_cast<unsigned char>(random());
		auto const r = static_cast<unsigned char>(random());
		auto const reference = aegisub::keypoint::BgrToLab(
			reference_b,
			reference_g,
			reference_r);
		auto const exact = aegisub::keypoint::BgrToLab(b, g, r);
		auto const approximate = aegisub::keypoint::ApproximateBgrToLab(b, g, r);
		double const distance_error = std::abs(
			aegisub::keypoint::DistanceSquared(exact, reference)
			- aegisub::keypoint::DistanceSquared(approximate, reference));
		EXPECT_LE(distance_error, aegisub::keypoint::approximate_squared_distance_error_margin);

		double const tolerance = static_cast<double>(random() & 0xFFU);
		double const tolerance_squared = tolerance * tolerance;
		EXPECT_EQ(
			aegisub::keypoint::MatchesBgr(reference, b, g, r, tolerance_squared),
			aegisub::keypoint::MatchesBgrFast(reference, b, g, r, tolerance_squared));
	}
}

TEST(key_point_color, reference_color_always_matches_zero_tolerance) {
	aegisub::keypoint::ColorMatcher matcher(7, 91, 203, 0.0);
	EXPECT_TRUE(matcher.Matches(7, 91, 203));
}
