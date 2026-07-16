#pragma once

#include "skia_audio_content.h"
#include "skia_audio_display_contract.h"
#include "skia_audio_frame_model.h"
#include "../skia_gl_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aegisub::skia::audio {

struct PresenterMetrics {
	std::uint64_t frame_attempts = 0;
	std::uint64_t surface_acquisitions = 0;
	std::uint64_t submits = 0;
	std::uint64_t content_tiles_drawn = 0;
	std::uint64_t content_tiles_skipped = 0;
	std::uint64_t content_cache_hits = 0;
	std::uint64_t content_cache_misses = 0;
	std::uint64_t content_uploads = 0;
	std::uint64_t content_upload_bytes = 0;
	std::uint64_t content_evictions = 0;
	std::uint64_t palette_uploads = 0;
	std::size_t content_cache_entries = 0;
	std::size_t content_cache_bytes = 0;
	std::size_t content_cache_budget_bytes = 0;
};

struct SpectrumPalette {
	std::uint64_t revision = 0;
	// SkColor-compatible AARRGGBB entries. Linear sampling between these 256
	// points preserves presentation changes without re-uploading power tiles.
	std::array<std::uint32_t, 256> colors {};
};

struct ContentFrame {
	ContentGeneration generation;
	ContentKind kind = ContentKind::Waveform;
	std::uint64_t first_column = 0;
	float x = 0.f;
	float y = 0.f;
	float width = 0.f;
	float height = 0.f;
	float first_column_offset = 0.f;
	float amplitude = 1.f;
	std::uint32_t background_color = 0xFF182230;
	std::uint32_t waveform_peak_color = 0xFF2A9D8F;
	std::uint32_t waveform_average_color = 0xFFE9C46A;
	std::uint32_t waveform_zero_color = 0xFF8CA0B3;
	bool draw_waveform_average = true;
	std::shared_ptr<SpectrumPalette const> spectrum_palette;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	std::vector<std::shared_ptr<ContentTile const>> tiles;
};

// Backend-only fixed-frame presenter used by P3.3. It owns a Ganesh device for
// one externally current GL context and retains the wrapped back-buffer surface
// while its exact context/size/FBO key remains unchanged.
class Presenter final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit Presenter(FailureInjection failure_injection);
	~Presenter();

	Presenter(Presenter const&) = delete;
	Presenter& operator=(Presenter const&) = delete;

	bool RenderDiagnosticFrame(SkiaGlContextToken context, FrameTarget const& target);
	bool RenderContentFrame(SkiaGlContextToken context, FrameTarget const& target, ContentFrame const& frame);
	void SetContentCacheBudget(std::size_t budget_bytes);
	void Fail(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept;
	void Release(SkiaGlContextToken context) noexcept;
	void Abandon() noexcept;

	SkiaGlDeviceHealth Health() const noexcept;
	SkiaGlDeviceFailure LastFailure() const noexcept;
	PresenterMetrics Metrics() const noexcept;
	std::string TakeFailureLogMessage();
};

}
