#pragma once

#include "grid_core/subtitle_grid_renderer_contract.h"

#include <cstdint>
#include <functional>
#include <memory>

class GridColumnPainter;
class wxDC;
class wxFont;
class wxRegion;
class wxSize;
class wxWindow;

namespace aegisub::grid {

class SkiaGridRenderer;

/// Owns runtime selection and permanent per-window failure suppression.
class SubtitleGridRendererSlot final {
	SubtitleGridRendererState state_;
	std::unique_ptr<SkiaGridRenderer> skia_;
	unsigned long long generation_ = 1;
	bool fallback_refresh_queued_ = false;
	bool raster_policy_refresh_pending_ = false;
	bool raster_policy_refresh_queued_ = false;
	bool monitor_initialized_ = false;
	std::uintptr_t monitor_token_ = 0;

public:
	SubtitleGridRendererSlot();
	~SubtitleGridRendererSlot();

	SubtitleGridRendererSlot(SubtitleGridRendererSlot const&) = delete;
	SubtitleGridRendererSlot& operator=(SubtitleGridRendererSlot const&) = delete;

	bool UsesSkia() const noexcept;
	bool SupportsContentScale(double content_scale) const noexcept;
	unsigned long long Generation() const noexcept { return generation_; }

	bool Paint(
		wxWindow& window,
		wxDC& target,
		wxSize const& size,
		wxRegion const& update_region,
		wxFont const& font,
		std::function<void(GridColumnPainter&)> const& paint,
		double plan_ms);

	std::unique_ptr<GridColumnPainter> CreateMeasurementPainter(wxFont const& font);
	void InvalidateSystemFonts() noexcept;
	void InvalidateRasterPolicy() noexcept;
};

} // namespace aegisub::grid
