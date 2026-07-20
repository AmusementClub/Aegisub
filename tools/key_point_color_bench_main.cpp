#include "key_point_color.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using LabColor = aegisub::keypoint::LabColor;

struct BgrColor {
	unsigned char b;
	unsigned char g;
	unsigned char r;
};

struct LutAccuracy {
	double maximum_squared_distance_error = 0.0;
	std::size_t exact_fallbacks = 0;
	std::size_t decision_mismatches = 0;
};

enum class MatchMode {
	Exact,
	Fast,
	Cached
};

LabColor BaselineBgrToLab(BgrColor color) {
	double const X = (0.412453 * color.r + 0.357580 * color.g + 0.180423 * color.b) / 255.0;
	double const Y = (0.212671 * color.r + 0.715160 * color.g + 0.072169 * color.b) / 255.0;
	double const Z = (0.019334 * color.r + 0.119193 * color.g + 0.950227 * color.b) / 255.0;
	double const xr = X / 0.950456;
	double const yr = Y / 1.000;
	double const zr = Z / 1.088854;

	LabColor lab;
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

LabColor CbrtBgrToLab(BgrColor color) {
	double const X = (0.412453 * color.r + 0.357580 * color.g + 0.180423 * color.b) / 255.0;
	double const Y = (0.212671 * color.r + 0.715160 * color.g + 0.072169 * color.b) / 255.0;
	double const Z = (0.019334 * color.r + 0.119193 * color.g + 0.950227 * color.b) / 255.0;
	double const xr = X / 0.950456;
	double const yr = Y / 1.000;
	double const zr = Z / 1.088854;
	constexpr double epsilon = 0.008856;
	auto const transfer = [](double value) {
		return value > epsilon
			? std::cbrt(value)
			: 7.787 * value + 16.0 / 116.0;
	};
	double const fx = transfer(xr);
	double const fy = transfer(yr);
	double const fz = transfer(zr);
	return {
		yr > epsilon ? 116.0 * fy - 16.0 : 903.3 * yr,
		500.0 * (fx - fy),
		200.0 * (fy - fz)
	};
}

std::vector<BgrColor> MakePaletteWorkload(std::size_t count, std::size_t palette_size) {
	std::mt19937 random(0xC010A5U + static_cast<unsigned>(palette_size));
	std::vector<BgrColor> palette(palette_size);
	for (auto& color : palette) {
		color = {
			static_cast<unsigned char>(random()),
			static_cast<unsigned char>(random()),
			static_cast<unsigned char>(random())
		};
	}

	std::vector<BgrColor> colors;
	colors.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		colors.push_back(palette[i % palette.size()]);
	return colors;
}

std::vector<BgrColor> MakeRandomWorkload(std::size_t count) {
	std::mt19937 random(0x51A7E5U);
	std::vector<BgrColor> colors(count);
	for (auto& color : colors) {
		color = {
			static_cast<unsigned char>(random()),
			static_cast<unsigned char>(random()),
			static_cast<unsigned char>(random())
		};
	}
	return colors;
}

template<typename Work>
double MedianNanosecondsPerColor(Work&& work, std::size_t color_count, int runs, double& checksum) {
	std::vector<double> samples;
	samples.reserve(runs);
	for (int warmup = 0; warmup < 3; ++warmup)
		checksum += work();
	for (int run = 0; run < runs; ++run) {
		auto const begin = Clock::now();
		checksum += work();
		auto const end = Clock::now();
		samples.push_back(
			std::chrono::duration<double, std::nano>(end - begin).count()
			/ static_cast<double>(color_count));
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

template<typename Convert>
double ConvertAll(std::vector<BgrColor> const& colors, Convert&& convert) {
	double checksum = 0.0;
	for (auto const color : colors) {
		auto const lab = convert(color);
		checksum += lab.l + lab.a * 0.25 + lab.b * 0.125;
	}
	return checksum;
}

double MatchAllUncached(std::vector<BgrColor> const& colors) {
	constexpr double tolerance_squared = 20.0 * 20.0;
	auto const reference = aegisub::keypoint::BgrToLab(40, 80, 120);
	std::uint64_t matches = 0;
	for (auto const color : colors) {
		matches += aegisub::keypoint::MatchesBgr(
			reference,
			color.b,
			color.g,
			color.r,
			tolerance_squared);
	}
	return static_cast<double>(matches);
}

double MatchAllFastUncached(std::vector<BgrColor> const& colors) {
	constexpr double tolerance_squared = 20.0 * 20.0;
	auto const reference = aegisub::keypoint::BgrToLab(40, 80, 120);
	std::uint64_t matches = 0;
	for (auto const color : colors) {
		matches += aegisub::keypoint::MatchesBgrFast(
			reference,
			color.b,
			color.g,
			color.r,
			tolerance_squared);
	}
	return static_cast<double>(matches);
}

double MatchAllCached(std::vector<BgrColor> const& colors) {
	constexpr double tolerance_squared = 20.0 * 20.0;
	aegisub::keypoint::ColorMatcher matcher(40, 80, 120, tolerance_squared);
	std::uint64_t matches = 0;
	for (auto const color : colors)
		matches += matcher.Matches(color.b, color.g, color.r);
	return static_cast<double>(matches);
}

double MatchRepeated(
	std::vector<BgrColor> const& colors,
	std::size_t repetitions,
	MatchMode mode) {
	constexpr double tolerance_squared = 20.0 * 20.0;
	std::uint64_t matches = 0;
	for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
		if (mode == MatchMode::Cached) {
			aegisub::keypoint::ColorMatcher matcher(40, 80, 120, tolerance_squared);
			for (auto const color : colors)
				matches += matcher.Matches(color.b, color.g, color.r);
		}
		else {
			auto const reference = aegisub::keypoint::BgrToLab(40, 80, 120);
			for (auto const color : colors) {
				if (mode == MatchMode::Exact) {
					matches += aegisub::keypoint::MatchesBgr(
						reference, color.b, color.g, color.r, tolerance_squared);
				}
				else {
					matches += aegisub::keypoint::MatchesBgrFast(
						reference, color.b, color.g, color.r, tolerance_squared);
				}
			}
		}
	}
	return static_cast<double>(matches);
}

LutAccuracy MeasureLutAccuracy(std::vector<BgrColor> const& colors) {
	LutAccuracy result;
	for (std::size_t i = 0; i < colors.size(); ++i) {
		auto const candidate = colors[i];
		auto const reference_color = colors[(i * 65537U + 17U) % colors.size()];
		auto const reference = aegisub::keypoint::BgrToLab(
			reference_color.b,
			reference_color.g,
			reference_color.r);
		double const exact_distance = aegisub::keypoint::DistanceSquared(
			aegisub::keypoint::BgrToLab(candidate.b, candidate.g, candidate.r),
			reference);
		double const approximate_distance = aegisub::keypoint::DistanceSquared(
			aegisub::keypoint::ApproximateBgrToLab(candidate.b, candidate.g, candidate.r),
			reference);
		result.maximum_squared_distance_error = std::max(
			result.maximum_squared_distance_error,
			std::abs(exact_distance - approximate_distance));

		double const tolerance = static_cast<double>(i & 0xFFU);
		double const tolerance_squared = tolerance * tolerance;
		bool const conclusive =
			approximate_distance + aegisub::keypoint::approximate_squared_distance_error_margin <= tolerance_squared
			|| approximate_distance - aegisub::keypoint::approximate_squared_distance_error_margin > tolerance_squared;
		result.exact_fallbacks += !conclusive;
		bool const exact_match = exact_distance <= tolerance_squared;
		bool const fast_match = aegisub::keypoint::MatchesBgrFast(
			reference,
			candidate.b,
			candidate.g,
			candidate.r,
			tolerance_squared);
		result.decision_mismatches += exact_match != fast_match;
	}
	return result;
}

int RunExhaustiveLutValidation() {
	double max_l_error = 0.0;
	double max_a_error = 0.0;
	double max_b_error = 0.0;
	LabColor minimum{};
	minimum.l = minimum.a = minimum.b = std::numeric_limits<double>::max();
	LabColor maximum{};
	maximum.l = maximum.a = maximum.b = std::numeric_limits<double>::lowest();

	for (unsigned int r = 0; r <= 255; ++r) {
		for (unsigned int g = 0; g <= 255; ++g) {
			for (unsigned int b = 0; b <= 255; ++b) {
				auto const exact = aegisub::keypoint::BgrToLab(
					static_cast<unsigned char>(b),
					static_cast<unsigned char>(g),
					static_cast<unsigned char>(r));
				auto const approximate = aegisub::keypoint::ApproximateBgrToLab(
					static_cast<unsigned char>(b),
					static_cast<unsigned char>(g),
					static_cast<unsigned char>(r));
				max_l_error = std::max(max_l_error, std::abs(exact.l - approximate.l));
				max_a_error = std::max(max_a_error, std::abs(exact.a - approximate.a));
				max_b_error = std::max(max_b_error, std::abs(exact.b - approximate.b));
				minimum.l = std::min(minimum.l, exact.l);
				minimum.a = std::min(minimum.a, exact.a);
				minimum.b = std::min(minimum.b, exact.b);
				maximum.l = std::max(maximum.l, exact.l);
				maximum.a = std::max(maximum.a, exact.a);
				maximum.b = std::max(maximum.b, exact.b);
			}
		}
	}

	double const conservative_bound =
		2.0 * ((maximum.l - minimum.l) * max_l_error
			+ (maximum.a - minimum.a) * max_a_error
			+ (maximum.b - minimum.b) * max_b_error)
		+ max_l_error * max_l_error
		+ max_a_error * max_a_error
		+ max_b_error * max_b_error;
	std::cout << "Exhaustive LUT validation (16,777,216 BGR colors)\n"
		<< "  max Lab errors: L=" << std::setprecision(9) << max_l_error
		<< ", a=" << max_a_error << ", b=" << max_b_error << "\n"
		<< "  conservative squared-distance bound: " << conservative_bound << "\n"
		<< "  configured margin: "
		<< aegisub::keypoint::approximate_squared_distance_error_margin << "\n";
	return conservative_bound <= aegisub::keypoint::approximate_squared_distance_error_margin ? 0 : 1;
}

void PrintMatchScenario(
	std::string const& name,
	std::vector<BgrColor> const& colors,
	int runs,
	double& checksum) {
	double const uncached = MedianNanosecondsPerColor(
		[&] { return MatchAllUncached(colors); },
		colors.size(),
		runs,
		checksum);
	double const fast_uncached = MedianNanosecondsPerColor(
		[&] { return MatchAllFastUncached(colors); },
		colors.size(),
		runs,
		checksum);
	double const cached = MedianNanosecondsPerColor(
		[&] { return MatchAllCached(colors); },
		colors.size(),
		runs,
		checksum);

	std::cout << std::left << std::setw(14) << name
		<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << uncached
		<< std::setw(14) << fast_uncached
		<< std::setw(14) << cached
		<< std::setw(12) << std::setprecision(2) << fast_uncached / cached << "x"
		<< std::setw(12) << uncached / cached << "x\n";
}

void PrintShortMatchScenario(
	std::size_t sample_count,
	std::size_t palette_size,
	int runs,
	double& checksum) {
	constexpr std::size_t repetitions = 8192;
	auto const colors = MakePaletteWorkload(sample_count, palette_size);
	std::size_t const total_colors = colors.size() * repetitions;
	double const exact = MedianNanosecondsPerColor(
		[&] { return MatchRepeated(colors, repetitions, MatchMode::Exact); },
		total_colors,
		runs,
		checksum);
	double const fast = MedianNanosecondsPerColor(
		[&] { return MatchRepeated(colors, repetitions, MatchMode::Fast); },
		total_colors,
		runs,
		checksum);
	double const cached = MedianNanosecondsPerColor(
		[&] { return MatchRepeated(colors, repetitions, MatchMode::Cached); },
		total_colors,
		runs,
		checksum);

	std::string const label = std::to_string(sample_count) + "/" + std::to_string(palette_size);
	std::cout << std::left << std::setw(14) << label
		<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << exact
		<< std::setw(14) << fast
		<< std::setw(14) << cached
		<< std::setw(12) << fast / cached << "x"
		<< std::setw(12) << exact / cached << "x\n";
}

}  // namespace

int main(int argc, char** argv) {
	if (argc > 1 && std::string(argv[1]) == "--exhaustive-lut") {
		return RunExhaustiveLutValidation();
	}
	constexpr std::size_t color_count = 1U << 18;
	constexpr int runs = 9;
	double checksum = 0.0;
	auto const random_colors = MakeRandomWorkload(color_count);
	auto const cold_color = random_colors.front();
	auto const cold_reference = aegisub::keypoint::BgrToLab(40, 80, 120);
	auto const cold_begin = Clock::now();
	bool const cold_match = aegisub::keypoint::MatchesBgrFast(
		cold_reference,
		cold_color.b,
		cold_color.g,
		cold_color.r,
		20.0 * 20.0);
	auto const cold_end = Clock::now();
	double const cold_lut_microseconds =
		std::chrono::duration<double, std::micro>(cold_end - cold_begin).count();
	auto const lut_accuracy = MeasureLutAccuracy(random_colors);

	double const baseline = MedianNanosecondsPerColor(
		[&] { return ConvertAll(random_colors, BaselineBgrToLab); },
		random_colors.size(),
		runs,
		checksum);
	double const optimized = MedianNanosecondsPerColor(
		[&] {
			return ConvertAll(random_colors, [](BgrColor color) {
				return aegisub::keypoint::BgrToLab(color.b, color.g, color.r);
			});
		},
		random_colors.size(),
		runs,
		checksum);
	double const lut = MedianNanosecondsPerColor(
		[&] {
			return ConvertAll(random_colors, [](BgrColor color) {
				return aegisub::keypoint::ApproximateBgrToLab(color.b, color.g, color.r);
			});
		},
		random_colors.size(),
		runs,
		checksum);
	double const cbrt = MedianNanosecondsPerColor(
		[&] { return ConvertAll(random_colors, CbrtBgrToLab); },
		random_colors.size(),
		runs,
		checksum);

	std::cout << "Key-point Lab benchmark (median of " << runs << " runs, "
		<< color_count << " colors)\n\n";
	std::cout << "Conversion throughput\n";
	std::cout << "  baseline: " << std::fixed << std::setprecision(2) << baseline << " ns/color\n";
	std::cout << "  reuse fy: " << optimized << " ns/color\n";
	std::cout << "  speedup:  " << baseline / optimized << "x\n\n";
	std::cout << "  std::cbrt: " << cbrt << " ns/color\n";
	std::cout << "  transfer LUT: " << lut << " ns/color ("
		<< optimized / lut << "x vs reuse fy)\n\n";
	std::cout << "  cold LUT initialization + first match: "
		<< std::setprecision(2) << cold_lut_microseconds << " us"
		<< " (match=" << (cold_match ? "true" : "false") << ")\n\n";
	std::cout << "LUT validation\n";
	std::cout << "  sampled max squared-distance error: " << std::setprecision(6)
		<< lut_accuracy.maximum_squared_distance_error << "\n";
	std::cout << "  sampled exact fallbacks:    " << lut_accuracy.exact_fallbacks
		<< " / " << color_count << "\n";
	std::cout << "  sampled decision mismatches:" << lut_accuracy.decision_mismatches << "\n\n";

	std::cout << "Exact match pipeline\n";
	std::cout << std::left << std::setw(14) << "workload"
		<< std::right << std::setw(14) << "exact ns"
		<< std::setw(14) << "LUT ns"
		<< std::setw(14) << "cached ns"
		<< std::setw(12) << "cache gain"
		<< std::setw(12) << "total gain\n";
	PrintMatchScenario("1 color", MakePaletteWorkload(color_count, 1), runs, checksum);
	PrintMatchScenario("4 colors", MakePaletteWorkload(color_count, 4), runs, checksum);
	PrintMatchScenario("16 colors", MakePaletteWorkload(color_count, 16), runs, checksum);
	PrintMatchScenario("256 colors", MakePaletteWorkload(color_count, 256), runs, checksum);
	PrintMatchScenario("random", random_colors, runs, checksum);

	std::cout << "\nShort scans (matcher construction included)\n";
	std::cout << std::left << std::setw(14) << "samples/colors"
		<< std::right << std::setw(14) << "exact ns"
		<< std::setw(14) << "LUT ns"
		<< std::setw(14) << "cached ns"
		<< std::setw(12) << "cache gain"
		<< std::setw(12) << "total gain\n";
	PrintShortMatchScenario(1, 1, runs, checksum);
	PrintShortMatchScenario(4, 4, runs, checksum);
	PrintShortMatchScenario(16, 4, runs, checksum);
	PrintShortMatchScenario(64, 16, runs, checksum);

	std::cout << "\nchecksum: " << std::setprecision(6) << checksum << "\n";
	return 0;
}
