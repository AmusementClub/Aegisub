#include "skia_audio_frame_model.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace aegisub::skia::audio {
namespace {

constexpr int kMaximumDeviceDimension = 32768;
constexpr std::uint32_t kMaximumSpectrumBins = 4096;
constexpr int kMaximumSpectrumHeight = 32768;

int ScaleBoundary(int logical, double scale) noexcept {
	if (logical <= 0)
		return 0;
	auto const value = std::lround(static_cast<double>(logical) * scale);
	return static_cast<int>(std::clamp<long>(value, 0, kMaximumDeviceDimension));
}

std::uint64_t HashCombine(std::uint64_t seed, std::uint64_t value) noexcept {
	seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
	return seed;
}

std::uint32_t FloatBits(float value) noexcept {
	return std::bit_cast<std::uint32_t>(value);
}

std::uint64_t SpectrumRevision(SpectrumBandPlanRequest const& request, bool interpolated) noexcept {
	std::uint64_t revision = 0xcbf29ce484222325ULL;
	revision = HashCombine(revision, request.bin_count);
	revision = HashCombine(revision, static_cast<std::uint64_t>(request.output_height));
	revision = HashCombine(revision, static_cast<std::uint64_t>(request.sample_rate));
	revision = HashCombine(revision, static_cast<std::uint64_t>(request.mode));
	revision = HashCombine(revision, FloatBits(request.frequency_reference_position));
	revision = HashCombine(revision, interpolated ? 1 : 0);
	return revision ? revision : 1;
}

bool ValidSpectrumRequest(SpectrumBandPlanRequest const& request) noexcept {
	return request.bin_count >= 4
		&& request.bin_count <= kMaximumSpectrumBins
		&& std::has_single_bit(request.bin_count)
		&& request.output_height > 0
		&& request.output_height <= kMaximumSpectrumHeight
		&& request.sample_rate > 0
		&& std::isfinite(request.frequency_reference_position)
		&& request.frequency_reference_position > 0.f
		&& request.frequency_reference_position < 1.f;
}

}

bool FrameViewport::IsValid() const noexcept {
	return target_width > 0
		&& target_height > 0
		&& content.IsValid()
		&& scroll_left >= 0
		&& logical_audio_width > 0
		&& std::isfinite(first_column_exact)
		&& std::isfinite(first_column_offset)
		&& first_column_offset <= 0.0
		&& first_column_offset > -1.0
		&& visible_column_count > 0
		&& std::isfinite(milliseconds_per_column)
		&& milliseconds_per_column > 0.0;
}

int AudioZoomFactor(int zoom_level) noexcept {
	int factor = 100;
	if (zoom_level > 0) {
		if (zoom_level > (std::numeric_limits<int>::max() - factor) / 25)
			return std::numeric_limits<int>::max();
		factor += 25 * zoom_level;
	}
	else if (zoom_level < 0) {
		if (zoom_level >= -5)
			factor += 10 * zoom_level;
		else if (zoom_level >= -11)
			factor = 50 + (zoom_level + 5) * 5;
		else
			factor = 20 + zoom_level + 11;
		if (factor <= 0)
			factor = 1;
	}
	return factor;
}

double AudioMillisecondsPerLogicalPixel(int zoom_level) noexcept {
	return 2000.0 / AudioZoomFactor(zoom_level);
}

FrameViewport BuildFrameViewport(FrameViewportRequest const& request) noexcept {
	FrameViewport viewport;
	if (request.logical_width <= 0
		|| request.logical_height <= 0
		|| request.logical_width > kMaximumDeviceDimension
		|| request.logical_height > kMaximumDeviceDimension
		|| !std::isfinite(request.content_scale)
		|| request.content_scale < 1.0
		|| request.content_scale > 8.0
		|| request.timeline_height < 0
		|| request.scrollbar_height < 0
		|| request.duration_ms < 0
		|| !std::isfinite(request.milliseconds_per_logical_pixel)
		|| request.milliseconds_per_logical_pixel <= 0.0) {
		return viewport;
	}

	viewport.target_width = ScaleBoundary(request.logical_width, request.content_scale);
	viewport.target_height = ScaleBoundary(request.logical_height, request.content_scale);
	auto const timeline_bottom = std::min(
		viewport.target_height,
		ScaleBoundary(request.timeline_height, request.content_scale));
	auto const scrollbar_height = std::min(
		viewport.target_height - timeline_bottom,
		ScaleBoundary(request.scrollbar_height, request.content_scale));
	auto const scrollbar_top = viewport.target_height - scrollbar_height;
	if (viewport.target_width <= 0 || scrollbar_top <= timeline_bottom)
		return {};

	viewport.timeline = { 0, 0, viewport.target_width, timeline_bottom };
	viewport.content = {
		0,
		timeline_bottom,
		viewport.target_width,
		scrollbar_top - timeline_bottom,
	};
	viewport.scrollbar = { 0, scrollbar_top, viewport.target_width, scrollbar_height };

	auto const audio_width_exact = request.duration_ms / request.milliseconds_per_logical_pixel;
	if (!std::isfinite(audio_width_exact)
		|| audio_width_exact > static_cast<double>(std::numeric_limits<int>::max())) {
		return {};
	}
	viewport.logical_audio_width = std::max(1, static_cast<int>(audio_width_exact));
	auto const maximum_scroll = std::max(0, viewport.logical_audio_width - request.logical_width);
	viewport.scroll_left = std::clamp(request.scroll_left, 0, maximum_scroll);

	viewport.first_column_exact = viewport.scroll_left * request.content_scale;
	if (!std::isfinite(viewport.first_column_exact)
		|| viewport.first_column_exact > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
		return {};
	}
	viewport.first_column = static_cast<std::uint64_t>(std::floor(viewport.first_column_exact));
	viewport.first_column_offset = static_cast<double>(viewport.first_column) - viewport.first_column_exact;
	auto const columns = std::ceil(viewport.target_width - viewport.first_column_offset);
	if (!std::isfinite(columns)
		|| columns <= 0.0
		|| columns > std::numeric_limits<std::uint32_t>::max()) {
		return {};
	}
	viewport.visible_column_count = static_cast<std::uint32_t>(columns);
	viewport.milliseconds_per_column = request.milliseconds_per_logical_pixel / request.content_scale;
	return viewport.IsValid() ? viewport : FrameViewport {};
}

std::vector<DeviceStyleSpan> BuildDeviceStyleSpans(
	std::vector<TimeStyleRange> const& ranges,
	FrameViewport const& viewport) {
	std::vector<DeviceStyleSpan> result;
	if (!viewport.IsValid() || viewport.milliseconds_per_column <= 0.0)
		return result;

	// Use the same exact logical scroll origin as the content request. Keeping
	// the calculation in milliseconds until the final conversion avoids style
	// seams at fractional-DPI boundaries.
	// The request's logical/physical conversion is represented by the exact
	// first column and physical milliseconds-per-column pair.
	auto const visible_first_ms = viewport.first_column_exact * viewport.milliseconds_per_column;
	auto const visible_last_ms = visible_first_ms
		+ static_cast<double>(viewport.content.width) * viewport.milliseconds_per_column;
	if (!std::isfinite(visible_first_ms) || !std::isfinite(visible_last_ms)
		|| visible_last_ms <= visible_first_ms)
		return result;

	std::vector<double> points { visible_first_ms, visible_last_ms };
	for (auto const& range : ranges) {
		if (range.end_ms <= range.start_ms)
			continue;
		auto const start = std::max<double>(visible_first_ms, range.start_ms);
		auto const end = std::min<double>(visible_last_ms, range.end_ms);
		if (end > start) {
			points.push_back(start);
			points.push_back(end);
		}
	}
	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end()), points.end());

	auto const style_at = [&ranges](double time) {
		FrameStyle style = FrameStyle::Normal;
		for (auto const& range : ranges) {
			if (range.start_ms <= time && time < range.end_ms
				&& static_cast<std::uint8_t>(range.style) > static_cast<std::uint8_t>(style))
				style = range.style;
		}
		return style;
	};
	for (std::size_t i = 1; i < points.size(); ++i) {
		auto const start = points[i - 1];
		auto const end = points[i];
		if (!(end > start))
			continue;
		auto const x1 = static_cast<float>(viewport.content.x
			+ (start - visible_first_ms) / viewport.milliseconds_per_column);
		auto const x2 = static_cast<float>(viewport.content.x
			+ (end - visible_first_ms) / viewport.milliseconds_per_column);
		if (!(x2 > x1))
			continue;
		auto const style = style_at((start + end) * 0.5);
		if (!result.empty() && result.back().style == style
			&& std::abs(result.back().x + result.back().width - x1) < 0.001f) {
			result.back().width = x2 - result.back().x;
		}
		else {
			result.push_back({ x1, x2 - x1, style });
		}
	}
	return result;
}

bool SpectrumBandPlan::IsValid() const noexcept {
	if (!revision
		|| bin_count < 4
		|| bin_count > kMaximumSpectrumBins
		|| !std::has_single_bit(bin_count)
		|| output_height <= 0
		|| bands.size() != static_cast<std::size_t>(output_height)) {
		return false;
	}
	return std::all_of(bands.begin(), bands.end(), [&](SpectrumBand const& band) {
		return band.first <= band.last
			&& band.last < bin_count
			&& std::isfinite(band.fraction)
			&& band.fraction >= 0.f
			&& band.fraction <= 1.f;
	});
}

float SpectrumFrequencyReferenceForPreset(int preset) noexcept {
	constexpr float positions[] = { 0.001f, 0.125f, 0.333f, 0.425f, 0.999f };
	return positions[std::clamp(preset, 0, 4)];
}

SpectrumBandPlan BuildSpectrumBandPlan(SpectrumBandPlanRequest const& request) {
	SpectrumBandPlan plan;
	if (!ValidSpectrumRequest(request))
		return plan;

	plan.bin_count = request.bin_count;
	plan.output_height = request.output_height;
	plan.interpolated = request.output_height > static_cast<int>(request.bin_count);
	plan.revision = SpectrumRevision(request, plan.interpolated);
	plan.bands.resize(static_cast<std::size_t>(request.output_height));

	if (request.mode == SpectrumScaleMode::LegacyLinear) {
		if (plan.interpolated) {
			for (int y = 0; y < request.output_height; ++y) {
				auto const ideal = static_cast<double>(y + 1) / request.output_height * request.bin_count;
				auto const floor_value = std::floor(ideal);
				auto const lower = std::clamp(
					static_cast<int>(floor_value),
					0,
					static_cast<int>(request.bin_count) - 1);
				auto const upper = std::clamp(
					static_cast<int>(std::ceil(ideal)),
					0,
					static_cast<int>(request.bin_count) - 1);
				plan.bands[static_cast<std::size_t>(y)] = {
					static_cast<std::uint32_t>(lower),
					static_cast<std::uint32_t>(upper),
					static_cast<float>(ideal - floor_value),
				};
			}
		}
		else {
			for (int y = 0; y < request.output_height; ++y) {
				auto const first = std::max(0, static_cast<int>(request.bin_count) * y / request.output_height);
				auto const last = std::min(
					static_cast<int>(request.bin_count) - 1,
					static_cast<int>(request.bin_count) * (y + 1) / request.output_height);
				plan.bands[static_cast<std::size_t>(y)] = {
					static_cast<std::uint32_t>(first),
					static_cast<std::uint32_t>(last),
					0.f,
				};
			}
		}
		return plan;
	}

	auto const bin_count = static_cast<int>(request.bin_count);
	constexpr int minimum_band = 1;
	auto maximum_band = std::min(
		bin_count,
		static_cast<int>(std::floor(bin_count * 20000.0f / (request.sample_rate * 0.5f))));
	if (maximum_band <= minimum_band + 1)
		maximum_band = std::min(bin_count, minimum_band + 2);
	if (maximum_band <= minimum_band)
		return {};

	auto const scale_log = std::log(static_cast<float>(maximum_band) / minimum_band);
	auto const reference_band = std::clamp(
		bin_count * 1000.0f / (request.sample_rate * 0.5f),
		1.0f,
		static_cast<float>(maximum_band - 1));
	auto const linear_reference = minimum_band
		+ (maximum_band - minimum_band) * request.frequency_reference_position;
	auto const log_reference = minimum_band
		* std::exp(request.frequency_reference_position * scale_log);
	auto const denominator = log_reference - linear_reference;
	float log_ratio = denominator != 0.f
		? (reference_band - linear_reference) / denominator
		: 0.f;
	log_ratio = std::clamp(log_ratio, 0.f, 1.f);

	auto mapped_band = [&](float position) {
		auto const linear = minimum_band + position * (maximum_band - minimum_band);
		auto const logarithmic = minimum_band * std::exp(position * scale_log);
		return std::clamp(
			linear + log_ratio * (logarithmic - linear),
			static_cast<float>(minimum_band),
			static_cast<float>(maximum_band - 1));
	};

	if (plan.interpolated) {
		for (int y = 0; y < request.output_height; ++y) {
			auto const band = mapped_band(static_cast<float>(y + 1) / request.output_height);
			auto const lower = std::clamp(static_cast<int>(std::floor(band)), 0, bin_count - 1);
			auto const upper = std::clamp(static_cast<int>(std::ceil(band)), 0, bin_count - 1);
			plan.bands[static_cast<std::size_t>(y)] = {
				static_cast<std::uint32_t>(lower),
				static_cast<std::uint32_t>(upper),
				band - std::floor(band),
			};
		}
	}
	else {
		for (int y = 0; y < request.output_height; ++y) {
			auto const previous = y == 0
				? static_cast<float>(minimum_band)
				: mapped_band(static_cast<float>(y) / request.output_height);
			auto const current = mapped_band(static_cast<float>(y + 1) / request.output_height);
			auto const next = y + 2 <= request.output_height
				? mapped_band(static_cast<float>(y + 2) / request.output_height)
				: static_cast<float>(maximum_band);
			auto first = static_cast<int>(std::floor((previous + current) * 0.5f));
			auto last = static_cast<int>(std::floor((current + next) * 0.5f));
			first = std::clamp(first, 0, bin_count - 2);
			last = std::clamp(last, first + 1, bin_count - 1);
			plan.bands[static_cast<std::size_t>(y)] = {
				static_cast<std::uint32_t>(first),
				static_cast<std::uint32_t>(last),
				0.f,
			};
		}
	}

	return plan.IsValid() ? plan : SpectrumBandPlan {};
}

}
