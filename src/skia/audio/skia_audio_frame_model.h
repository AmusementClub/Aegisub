#pragma once

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
FrameViewport BuildFrameViewport(FrameViewportRequest const& request) noexcept;

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
