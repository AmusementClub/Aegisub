#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace aegisub::skia::audio {

struct DeviceRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	bool IsValid() const noexcept { return width > 0 && height > 0; }
	friend bool operator==(DeviceRect const&, DeviceRect const&) = default;
};

struct FrameViewportRequest {
	int logical_width = 0;
	int logical_height = 0;
	double content_scale = 1.0;
	int timeline_height = 0;
	int scrollbar_height = 15;
	int scroll_left = 0;
	int duration_ms = 0;
	double milliseconds_per_logical_pixel = 0.0;
};

struct FrameViewport {
	int target_width = 0;
	int target_height = 0;
	DeviceRect timeline;
	DeviceRect content;
	DeviceRect scrollbar;
	int scroll_left = 0;
	int logical_audio_width = 1;
	double first_column_exact = 0.0;
	std::uint64_t first_column = 0;
	double first_column_offset = 0.0;
	std::uint32_t visible_column_count = 0;
	double milliseconds_per_column = 0.0;

	bool IsValid() const noexcept;
};

int AudioZoomFactor(int zoom_level) noexcept;
double AudioMillisecondsPerLogicalPixel(int zoom_level) noexcept;
int AudioScrollLeftAfterZoom(
	int scroll_left,
	int client_width,
	double old_milliseconds_per_pixel,
	double new_milliseconds_per_pixel,
	double anchor_time_ms = -1.0) noexcept;
FrameViewport BuildFrameViewport(FrameViewportRequest const& request) noexcept;
int MousePositionMsForClientPoint(
	FrameViewport const& viewport,
	int logical_x,
	int logical_y,
	double content_scale,
	double milliseconds_per_logical_pixel) noexcept;

enum class CursorSource : std::uint8_t {
	None,
	Mouse,
	Playback,
};

struct CursorPlacement {
	CursorSource source = CursorSource::None;
	int position_ms = -1;
	float device_x = 0.f;

	bool IsActive() const noexcept { return source != CursorSource::None; }
};

CursorPlacement BuildCursorPlacement(
	FrameViewport const& viewport,
	int mouse_position_ms,
	int playback_position_ms) noexcept;
char const *CursorSourceName(CursorSource source) noexcept;

struct ScrollbarGeometry {
	float selection_x = 0.f;
	float selection_width = 0.f;
	float load_x = 0.f;
	float load_width = 0.f;
	float thumb_x = 0.f;
	float thumb_width = 0.f;
	float nominal_thumb_width = 0.f;
	bool selection_visible = false;
	bool load_visible = false;
	bool valid = false;
};

ScrollbarGeometry BuildScrollbarGeometry(
	float track_width,
	float minimum_thumb_width,
	float load_marker_width,
	int total,
	int page,
	int position,
	int load_position,
	int selection_start,
	int selection_length) noexcept;

std::chrono::nanoseconds PresentationFrameInterval(int display_refresh_rate) noexcept;

/// Timeline scale-mark granularity. Mirrors the legacy AudioDisplayTimeline
/// Scale enum one-for-one so both renderers pick the same marks at any zoom.
enum class TimelineScale : std::uint8_t {
	Millisecond,
	Centisecond,
	Decisecond,
	Second,
	Decasecond,
	Minute,
	Decaminute,
	Hour,
	Decahour,
};

struct TimelineScalePlan {
	TimelineScale scale = TimelineScale::Second;
	/// Mark index multiplied by this gives the mark time in milliseconds.
	double minor_divisor = 1000.0;
	/// A mark is major when its index % major_modulo == 0.
	int major_modulo = 10;
	bool valid = false;

	friend bool operator==(TimelineScalePlan const&, TimelineScalePlan const&) = default;
};

/// Pick the scale-mark plan for a zoom level, keyed on milliseconds per pixel.
/// Uses the legacy pixels-per-second thresholds, including the modulo-6 majors
/// at Decasecond and Decaminute (so majors land on whole minutes and hours).
TimelineScalePlan BuildTimelineScalePlan(double milliseconds_per_pixel) noexcept;

struct TimelineMark {
	/// Absolute minor-mark index; mark time is index * minor_divisor.
	std::int64_t index = 0;
	double time_ms = 0.0;
	/// Pixel offset from the left edge of the timeline, in device pixels.
	double x = 0.0;
	bool major = false;

	friend bool operator==(TimelineMark const&, TimelineMark const&) = default;
};

/// Enumerate the visible scale marks left-to-right. Like legacy, this walks
/// every minor mark from the first one at or after the scroll position and
/// keeps going until one lands past the right edge, so the mark past the edge
/// is included and marks are not clamped to the audio duration.
std::vector<TimelineMark> BuildTimelineMarks(
	TimelineScalePlan const& plan,
	double scroll_left,
	double width,
	double milliseconds_per_pixel);

/// Stateful timeline label text builder. Legacy suppresses the hour and minute
/// fields while they are unchanged from the previous label, so labels must be
/// formatted in left-to-right order through one instance per repaint.
class TimelineLabelFormatter final {
	TimelineScale scale = TimelineScale::Second;
	int last_hour = -1;
	int last_minute = -1;

public:
	/// duration_ms is the audio duration, used only for legacy's "hide hours on
	/// short audio" test. That test compares a millisecond duration against
	/// 3600, so it only engages below 3.6s; it is reproduced verbatim because
	/// it is what makes the first label of a normal file carry a "h:mm:" prefix.
	TimelineLabelFormatter(TimelineScale scale, int duration_ms) noexcept;

	/// Format the label for one major mark. Must be called in left-to-right
	/// order: each call updates the suppression state for the next one.
	std::string Format(double mark_time_ms);
};

enum class FrameStyle : std::uint8_t {
	Normal,
	Inactive,
	Selected,
	Primary,
};

struct TimeStyleRange {
	int start_ms = 0;
	int end_ms = 0;
	FrameStyle style = FrameStyle::Normal;
};

struct DeviceStyleSpan {
	float x = 0.f;
	float width = 0.f;
	FrameStyle style = FrameStyle::Normal;

	friend bool operator==(DeviceStyleSpan const&, DeviceStyleSpan const&) = default;
};

bool IsValidDeviceStyleSpan(
	float content_x,
	float content_width,
	float span_x,
	float span_width) noexcept;

std::vector<DeviceStyleSpan> BuildDeviceStyleSpans(
	std::vector<TimeStyleRange> const& ranges,
	FrameViewport const& viewport);

enum class SpectrumScaleMode {
	LegacyLinear,
	FrequencyCurve,
};

struct SpectrumBand {
	std::uint32_t first = 0;
	std::uint32_t last = 0;
	float fraction = 0.f;

	friend bool operator==(SpectrumBand const&, SpectrumBand const&) = default;
};

struct SpectrumBandPlanRequest {
	std::uint32_t bin_count = 0;
	int output_height = 0;
	int sample_rate = 0;
	SpectrumScaleMode mode = SpectrumScaleMode::LegacyLinear;
	float frequency_reference_position = 1.f / 3.f;
};

struct SpectrumBandPlan {
	std::uint64_t revision = 0;
	std::uint32_t bin_count = 0;
	int output_height = 0;
	bool interpolated = false;
	std::vector<SpectrumBand> bands;

	bool IsValid() const noexcept;
};

float SpectrumFrequencyReferenceForPreset(int preset) noexcept;
SpectrumBandPlan BuildSpectrumBandPlan(SpectrumBandPlanRequest const& request);

}
