#pragma once

#include <chrono>
#include <cstdint>
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
