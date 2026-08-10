#pragma once

#include "skia_grid_text_backend.h"

#include <functional>
#include <memory>
#include <string>

class GridColumnPainter;
class wxDC;
class wxFont;
class wxRegion;
class wxSize;

namespace aegisub::grid {

struct SkiaGridFrameTrace {
	bool valid = false;
	double plan_ms = 0.0;
	double paint_ms = 0.0;
	double text_layout_ms = 0.0;
	double text_raster_ms = 0.0;
	double geometry_raster_ms = 0.0;
	double copy_ms = 0.0;
	double present_ms = 0.0;
	double total_ms = 0.0;
	unsigned long long dirty_rectangles = 0;
	unsigned long long dirty_pixels = 0;
	unsigned long long copied_bytes = 0;
	GridTextBackendStats text_cache;
};

struct SkiaGridRendererStats {
	unsigned long long frame_attempts = 0;
	unsigned long long frames_presented = 0;
	unsigned long long surface_allocations = 0;
	unsigned long long copied_bytes = 0;
	unsigned long long fallback_count = 0;
	SkiaGridFrameTrace last_frame;
};

struct SkiaGridPresentationContext {
	std::uintptr_t monitor_token = 0;
	bool opaque_target = true;
	bool one_to_one_present = true;
	bool clear_type_requested = false;
};

/// Persistent CPU-raster renderer and wx presentation bridge.
class SkiaGridRenderer final {
	struct Impl;
	std::unique_ptr<Impl> impl_;

public:
	SkiaGridRenderer();
	~SkiaGridRenderer();

	SkiaGridRenderer(SkiaGridRenderer const&) = delete;
	SkiaGridRenderer& operator=(SkiaGridRenderer const&) = delete;

	bool Paint(
		wxDC& target,
		wxSize const& size,
		wxRegion const& update_region,
		SkiaGridPresentationContext const& presentation,
		wxFont const& font,
		std::function<void(GridColumnPainter&)> const& paint,
		double plan_ms,
		std::string& error);

	std::unique_ptr<GridColumnPainter> CreateMeasurementPainter(
		wxFont const& font,
		std::string& error);
	void InvalidateSystemFonts() noexcept;
	void InvalidateRasterPolicy() noexcept;
	SkiaGridRendererStats Stats() const noexcept;
};

} // namespace aegisub::grid
