#include "skia_audio_frame_model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace aegisub::skia::audio {
namespace {

constexpr int kMaximumDeviceDimension = 32768;
constexpr std::uint32_t kMaximumSpectrumBins = 4096;
constexpr int kMaximumSpectrumHeight = 32768;
// The scale thresholds keep minor marks at least ~3px apart, so a full-width
// pass over a 32768px target needs well under 11k marks. This only guards
// against a pathological milliseconds_per_pixel producing an endless loop.
constexpr std::size_t kMaximumTimelineMarks = 16384;

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

int AudioScrollLeftAfterZoom(
	int scroll_left,
	int client_width,
	double old_milliseconds_per_pixel,
	double new_milliseconds_per_pixel,
	double anchor_time_ms) noexcept {
	if (client_width <= 0
		|| !std::isfinite(old_milliseconds_per_pixel)
		|| old_milliseconds_per_pixel <= 0.0
		|| !std::isfinite(new_milliseconds_per_pixel)
		|| new_milliseconds_per_pixel <= 0.0) {
		return scroll_left;
	}

	double anchor_x = client_width / 2.0;
	if (std::isfinite(anchor_time_ms) && anchor_time_ms >= 0.0)
		anchor_x = anchor_time_ms / old_milliseconds_per_pixel - scroll_left;
	else
		anchor_time_ms = (scroll_left + anchor_x) * old_milliseconds_per_pixel;

	auto const value = anchor_time_ms / new_milliseconds_per_pixel - anchor_x;
	if (!std::isfinite(value))
		return scroll_left;
	return static_cast<int>(std::clamp(
		value,
		static_cast<double>(std::numeric_limits<int>::min()),
		static_cast<double>(std::numeric_limits<int>::max())));
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

int MousePositionMsForClientPoint(
	FrameViewport const& viewport,
	int logical_x,
	int logical_y,
	double content_scale,
	double milliseconds_per_logical_pixel) noexcept {
	if (!viewport.IsValid()
		|| !std::isfinite(content_scale)
		|| content_scale < 1.0
		|| !std::isfinite(milliseconds_per_logical_pixel)
		|| milliseconds_per_logical_pixel <= 0.0) {
		return -1;
	}

	auto const logical_width = static_cast<int>(std::ceil(viewport.target_width / content_scale));
	auto const logical_height = static_cast<int>(std::ceil(viewport.target_height / content_scale));
	auto const timeline_bottom = static_cast<int>(std::ceil(
		(viewport.timeline.y + viewport.timeline.height) / content_scale));
	auto const scrollbar_top = static_cast<int>(std::floor(viewport.scrollbar.y / content_scale));
	if (logical_x < 0 || logical_x >= logical_width
		|| logical_y < 0 || logical_y >= logical_height
		|| logical_y < timeline_bottom || logical_y >= scrollbar_top) {
		return -1;
	}

	auto const value = (viewport.scroll_left + logical_x) * milliseconds_per_logical_pixel;
	return static_cast<int>(std::clamp(
		value,
		0.0,
		static_cast<double>(std::numeric_limits<int>::max())));
}

CursorPlacement BuildCursorPlacement(
	FrameViewport const& viewport,
	int mouse_position_ms,
	int playback_position_ms) noexcept {
	CursorPlacement placement;
	if (!viewport.IsValid())
		return placement;

	if (playback_position_ms >= 0) {
		placement.source = CursorSource::Playback;
		placement.position_ms = playback_position_ms;
	}
	else if (mouse_position_ms >= 0) {
		placement.source = CursorSource::Mouse;
		placement.position_ms = mouse_position_ms;
	}
	else {
		return placement;
	}

	auto const device_x = viewport.content.x
		+ placement.position_ms / viewport.milliseconds_per_column
		- viewport.first_column_exact;
	if (!std::isfinite(device_x)
		|| device_x < std::numeric_limits<float>::lowest()
		|| device_x > std::numeric_limits<float>::max()) {
		return {};
	}
	placement.device_x = static_cast<float>(device_x);
	return placement;
}

char const *CursorSourceName(CursorSource source) noexcept {
	switch (source) {
		case CursorSource::None: return "none";
		case CursorSource::Mouse: return "mouse";
		case CursorSource::Playback: return "playback";
	}
	return "none";
}

ScrollbarGeometry BuildScrollbarGeometry(
	float track_width,
	float minimum_thumb_width,
	float load_marker_width,
	int total,
	int page,
	int position,
	int load_position,
	int selection_start,
	int selection_length) noexcept {
	ScrollbarGeometry geometry;
	if (!std::isfinite(track_width)
		|| !std::isfinite(minimum_thumb_width)
		|| !std::isfinite(load_marker_width)
		|| track_width <= 0.f
		|| minimum_thumb_width <= 0.f
		|| load_marker_width <= 0.f
		|| total <= 0) {
		return geometry;
	}

	page = std::clamp(page, 1, total);
	position = std::clamp(position, 0, total - page);
	auto const nominal_width = std::floor(
		track_width * static_cast<float>(page) / static_cast<float>(total));
	auto const nominal_x = std::floor(
		track_width * static_cast<float>(position) / static_cast<float>(total));
	geometry.nominal_thumb_width = nominal_width;
	geometry.thumb_width = std::max(minimum_thumb_width, nominal_width);
	geometry.thumb_x = nominal_x - std::floor((geometry.thumb_width - nominal_width) * 0.5f);

	if (selection_start >= 0 && selection_length > 0) {
		geometry.selection_x = std::floor(
			track_width * static_cast<float>(selection_start) / static_cast<float>(total));
		geometry.selection_width = std::floor(
			track_width * static_cast<float>(selection_length) / static_cast<float>(total));
		geometry.selection_visible = geometry.selection_width > 0.f;
	}
	if (load_position > 0 && load_position < total) {
		auto const loaded_x = std::floor(
			track_width * static_cast<float>(load_position) / static_cast<float>(total));
		geometry.load_x = loaded_x - load_marker_width;
		geometry.load_width = load_marker_width;
		geometry.load_visible = true;
	}
	geometry.valid = true;
	return geometry;
}

std::chrono::nanoseconds PresentationFrameInterval(int display_refresh_rate) noexcept {
	if (display_refresh_rate < 24 || display_refresh_rate > 1000)
		display_refresh_rate = 60;
	return std::chrono::nanoseconds { 1'000'000'000LL / display_refresh_rate };
}

TimelineScalePlan BuildTimelineScalePlan(double milliseconds_per_pixel) noexcept {
	TimelineScalePlan plan;
	if (!std::isfinite(milliseconds_per_pixel) || milliseconds_per_pixel <= 0.0)
		return plan;

	// Thresholds transcribed from AudioDisplayTimeline::ChangeZoom so both
	// renderers pick the same scale, and therefore the same major-mark spacing,
	// at every zoom level. Note the modulo is 6 (not 10) at Decasecond and
	// Decaminute so major marks land on whole minutes and whole hours.
	auto const px_sec = 1000.0 / milliseconds_per_pixel;
	if (px_sec > 3000) {
		plan.scale = TimelineScale::Millisecond;
		plan.minor_divisor = 1.0;
		plan.major_modulo = 10;
	}
	else if (px_sec > 300) {
		plan.scale = TimelineScale::Centisecond;
		plan.minor_divisor = 10.0;
		plan.major_modulo = 10;
	}
	else if (px_sec > 30) {
		plan.scale = TimelineScale::Decisecond;
		plan.minor_divisor = 100.0;
		plan.major_modulo = 10;
	}
	else if (px_sec > 3) {
		plan.scale = TimelineScale::Second;
		plan.minor_divisor = 1000.0;
		plan.major_modulo = 10;
	}
	else if (px_sec > 1.0 / 3.0) {
		plan.scale = TimelineScale::Decasecond;
		plan.minor_divisor = 10000.0;
		plan.major_modulo = 6;
	}
	else if (px_sec > 1.0 / 9.0) {
		plan.scale = TimelineScale::Minute;
		plan.minor_divisor = 60000.0;
		plan.major_modulo = 10;
	}
	else if (px_sec > 1.0 / 90.0) {
		plan.scale = TimelineScale::Decaminute;
		plan.minor_divisor = 600000.0;
		plan.major_modulo = 6;
	}
	else {
		plan.scale = TimelineScale::Hour;
		plan.minor_divisor = 3600000.0;
		plan.major_modulo = 10;
	}
	plan.valid = true;
	return plan;
}

std::vector<TimelineMark> BuildTimelineMarks(
	TimelineScalePlan const& plan,
	double scroll_left,
	double width,
	double milliseconds_per_pixel) {
	std::vector<TimelineMark> marks;
	if (!plan.valid
		|| plan.minor_divisor <= 0.0
		|| plan.major_modulo <= 0
		|| !std::isfinite(scroll_left)
		|| !std::isfinite(width)
		|| !std::isfinite(milliseconds_per_pixel)
		|| milliseconds_per_pixel <= 0.0
		|| width <= 0.0)
		return marks;

	scroll_left = std::max(0.0, scroll_left);

	// Figure out the first scale mark to show, matching the legacy rounding:
	// truncate towards zero, then step forward one mark if that landed left of
	// the visible time.
	auto const ms_left = scroll_left * milliseconds_per_pixel;
	auto index = static_cast<std::int64_t>(ms_left / plan.minor_divisor);
	if (index * plan.minor_divisor < ms_left)
		index += 1;

	// The legacy loop is a do/while on the mark position, so it always emits at
	// least one mark and keeps going one mark past the right edge. It is not
	// clamped to the audio duration either: marks continue for the full widget
	// width even past the end of the audio.
	marks.reserve(static_cast<std::size_t>(
		std::min(width / std::max(1.0, plan.minor_divisor / milliseconds_per_pixel) + 4.0, 1024.0)));
	double position = 0.0;
	do {
		position = index * plan.minor_divisor / milliseconds_per_pixel - scroll_left;
		marks.push_back({
			index,
			index * plan.minor_divisor,
			position,
			index % plan.major_modulo == 0,
		});
		index += 1;
	} while (position < width && marks.size() < kMaximumTimelineMarks);
	return marks;
}

TimelineLabelFormatter::TimelineLabelFormatter(TimelineScale scale, int duration_ms) noexcept
: scale(scale) {
	// Verbatim legacy quirk: the test is against a millisecond duration but the
	// constant reads as seconds, so it only fires for audio shorter than 3.6s.
	// Keeping it means a normal file leaves last_hour at -1, which is what makes
	// the leftmost label of every repaint carry the full "h:mm:" prefix.
	if (duration_ms < 3600)
		last_hour = 0;
}

std::string TimelineLabelFormatter::Format(double mark_time_ms) {
	// Legacy computes the mark time in seconds, then splits it, so the seconds
	// component keeps its fractional part for the sub-second scales.
	auto const mark_time = mark_time_ms / 1000.0;
	auto const mark_hour = static_cast<int>(mark_time / 3600);
	auto const mark_minute = static_cast<int>(mark_time / 60) % 60;
	auto const mark_second = mark_time - mark_hour * 3600.0 - mark_minute * 60.0;

	std::ostringstream out;
	auto const changed_hour = mark_hour != last_hour;
	auto const changed_minute = mark_minute != last_minute;
	if (changed_hour) {
		out << mark_hour << ':' << std::setfill('0') << std::setw(2) << mark_minute << ':';
		last_hour = mark_hour;
		last_minute = mark_minute;
	}
	else if (changed_minute) {
		out << mark_minute << ':';
		last_minute = mark_minute;
	}

	// fmt_wx("%02d", double) truncates towards zero; "%02.Nf" is fixed-point
	// with a minimum field width of 2 (which the "0" flag zero-fills).
	out << std::setfill('0');
	if (scale >= TimelineScale::Decisecond)
		out << std::setw(2) << static_cast<std::int64_t>(mark_second);
	else
		out << std::fixed
			<< std::setprecision(scale == TimelineScale::Centisecond ? 1 : 2)
			<< std::setw(2) << mark_second;
	return out.str();
}

bool IsValidDeviceStyleSpan(
	float content_x,
	float content_width,
	float span_x,
	float span_width) noexcept {
	if (!std::isfinite(content_x)
		|| !std::isfinite(content_width)
		|| !std::isfinite(span_x)
		|| !std::isfinite(span_width)
		|| content_width <= 0.f
		|| span_width <= 0.f) {
		return false;
	}

	auto const content_right = static_cast<double>(content_x) + content_width;
	auto const span_right = static_cast<double>(span_x) + span_width;
	auto const magnitude = std::max({
		1.0,
		std::abs(static_cast<double>(content_x)),
		std::abs(content_right),
	});
	auto const tolerance = std::max(
		0.001,
		8.0 * std::numeric_limits<float>::epsilon() * magnitude);
	return static_cast<double>(span_x) >= static_cast<double>(content_x) - tolerance
		&& span_right <= content_right + tolerance;
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

	struct StyleEvent {
		double time = 0.0;
		FrameStyle style = FrameStyle::Normal;
		int delta = 0;
	};
	std::vector<StyleEvent> events;
	events.reserve(ranges.size() * 2);
	for (auto const& range : ranges) {
		if (range.end_ms <= range.start_ms)
			continue;
		auto const start = std::max<double>(visible_first_ms, range.start_ms);
		auto const end = std::min<double>(visible_last_ms, range.end_ms);
		if (end > start && range.style != FrameStyle::Normal) {
			events.push_back({ start, range.style, 1 });
			events.push_back({ end, range.style, -1 });
		}
	}
	std::sort(events.begin(), events.end(), [](StyleEvent const& left, StyleEvent const& right) {
		return left.time < right.time;
	});

	constexpr auto style_count = static_cast<std::size_t>(FrameStyle::Primary) + 1;
	std::array<std::size_t, style_count> active {};
	auto const current_style = [&active] {
		for (auto index = active.size(); index-- > 1; )
			if (active[index])
				return static_cast<FrameStyle>(index);
		return FrameStyle::Normal;
	};
	auto append_span = [&](double start, double end) {
		if (!(end > start))
			return;
		auto const x1 = static_cast<float>(viewport.content.x
			+ (start - visible_first_ms) / viewport.milliseconds_per_column);
		auto const x2 = static_cast<float>(viewport.content.x
			+ (end - visible_first_ms) / viewport.milliseconds_per_column);
		if (!(x2 > x1))
			return;
		auto const style = current_style();
		if (!result.empty() && result.back().style == style
			&& std::abs(result.back().x + result.back().width - x1) < 0.001f) {
			result.back().width = x2 - result.back().x;
		}
		else {
			result.push_back({ x1, x2 - x1, style });
		}
	};

	double position = visible_first_ms;
	std::size_t event_index = 0;
	while (event_index < events.size()) {
		auto const event_time = events[event_index].time;
		append_span(position, event_time);
		while (event_index < events.size() && events[event_index].time == event_time) {
			auto const style_index = static_cast<std::size_t>(events[event_index].style);
			if (style_index < active.size()) {
				if (events[event_index].delta > 0)
					++active[style_index];
				else if (active[style_index])
					--active[style_index];
			}
			++event_index;
		}
		position = event_time;
	}
	append_span(position, visible_last_ms);
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
