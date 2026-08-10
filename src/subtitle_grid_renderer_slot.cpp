#include "subtitle_grid_renderer_slot.h"

#include "grid_column_painter.h"
#include "options.h"
#include "perf_trace.h"
#include "skia_grid_renderer.h"
#include "skia_runtime/skia_runtime_feature.h"

#include <libaegisub/log.h>

#include <wx/font.h>
#include <wx/region.h>
#include <wx/weakref.h>
#include <wx/window.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <exception>
#include <string>

namespace aegisub::grid {
namespace {

bool RuntimeRequested() noexcept {
	return aegisub::skia::ResolveRuntimeFeatureEnabled(
		std::getenv("AEGISUB_ENABLE_SKIA_SUBTITLE_GRID"),
		OPT_GET("Subtitle/Grid/Skia/Enabled")->GetBool());
}

bool ClearTypeRequested() noexcept {
#ifdef _WIN32
	return OPT_GET("Subtitle/Grid/Skia/ClearType")->GetBool();
#else
	return false;
#endif
}

bool CoversCompleteTarget(wxRegion const& region, wxSize const& size) {
	return size.GetWidth() > 0 && size.GetHeight() > 0
		&& region.Contains(wxRect(wxPoint(0, 0), size)) == wxInRegion;
}

std::uintptr_t CurrentMonitorToken(wxWindow& window) noexcept {
#ifdef _WIN32
	auto const hwnd = reinterpret_cast<HWND>(window.GetHandle());
	return reinterpret_cast<std::uintptr_t>(
		hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr);
#else
	(void)window;
	return 0;
#endif
}

} // namespace

SubtitleGridRendererSlot::SubtitleGridRendererSlot()
: state_(RuntimeRequested()) {
	if (state_.Requested() == SubtitleGridRendererBackend::Skia)
		skia_ = std::make_unique<SkiaGridRenderer>();
	LOG_I("subtitle/grid/renderer")
		<< "compiled=1 requested=" << SubtitleGridRendererBackendName(state_.Requested())
		<< " active=" << SubtitleGridRendererBackendName(state_.Active())
		<< " presentation=skia-cpu-raster";
}

SubtitleGridRendererSlot::~SubtitleGridRendererSlot() = default;

bool SubtitleGridRendererSlot::UsesSkia() const noexcept {
	return state_.Active() == SubtitleGridRendererBackend::Skia && skia_ != nullptr;
}

bool SubtitleGridRendererSlot::SupportsContentScale(double content_scale) const noexcept {
#ifdef _WIN32
	constexpr bool LogicalCoordinatesAreDevicePixels = true;
#else
	constexpr bool LogicalCoordinatesAreDevicePixels = false;
#endif
	return SupportsSkiaGridContentScale(
		content_scale, LogicalCoordinatesAreDevicePixels);
}

bool SubtitleGridRendererSlot::Paint(
	wxWindow& window,
	wxDC& target,
	wxSize const& size,
	wxRegion const& update_region,
	wxFont const& font,
	std::function<void(GridColumnPainter&)> const& paint,
	double plan_ms) {
	if (!UsesSkia())
		return false;

	auto const current_monitor = CurrentMonitorToken(window);
	if (!monitor_initialized_) {
		monitor_initialized_ = true;
		monitor_token_ = current_monitor;
	}
	else if (current_monitor != monitor_token_) {
		if (state_.Presented())
			raster_policy_refresh_pending_ = true;
		else
			monitor_token_ = current_monitor;
	}

	if (raster_policy_refresh_pending_) {
		if (!CoversCompleteTarget(update_region, size)) {
			if (!raster_policy_refresh_queued_) {
				raster_policy_refresh_queued_ = true;
				wxWeakRef<wxWindow> weak_window(&window);
				window.CallAfter([weak_window] {
					if (weak_window)
						weak_window->Refresh(false);
				});
			}
			// Keep the retained frame under one raster policy until the queued
			// complete repaint can adopt the new display/session parameters.
			return true;
		}
		monitor_token_ = current_monitor;
		skia_->InvalidateRasterPolicy();
		raster_policy_refresh_pending_ = false;
		raster_policy_refresh_queued_ = false;
	}

	std::string error;
	bool presented = false;
	try {
		presented = skia_->Paint(
			target,
			size,
			update_region,
			{monitor_token_, true, true, ClearTypeRequested()},
			font,
			paint,
			plan_ms,
			error);
	}
	catch (std::exception const& exception) {
		error = exception.what();
	}
	catch (...) {
		error = "unknown subtitle grid Skia failure";
	}
	if (presented) {
		state_.MarkPresented();
		if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
			auto const trace = skia_->Stats().last_frame;
			LOG_I("subtitle/grid/skia_frame")
				<< "plan_ms=" << trace.plan_ms
				<< " paint_ms=" << trace.paint_ms
				<< " text_layout_ms=" << trace.text_layout_ms
				<< " text_raster_ms=" << trace.text_raster_ms
				<< " geometry_raster_ms=" << trace.geometry_raster_ms
				<< " copy_ms=" << trace.copy_ms
				<< " present_ms=" << trace.present_ms
				<< " total_ms=" << trace.total_ms
				<< " dirty_rectangles=" << trace.dirty_rectangles
				<< " dirty_pixels=" << trace.dirty_pixels
				<< " copied_bytes=" << trace.copied_bytes
				<< " layout_hits=" << trace.text_cache.layout_hits
				<< " layout_misses=" << trace.text_cache.layout_misses
				<< " layout_evictions=" << trace.text_cache.layout_evictions
				<< " layout_entries=" << trace.text_cache.layout_entries
				<< " layout_bytes=" << trace.text_cache.layout_bytes
				<< " text_antialias=" << (trace.text_cache.raster_policy_known
					? (trace.text_cache.clear_type_active ? "cleartype" : "grayscale")
					: "unknown");
		}
		return true;
	}

	if (error.empty())
		error = "subtitle grid Skia frame was not presented";
	auto const action = state_.MarkFailed(error);
	if (action == SubtitleGridRendererFailureAction::QueueWxFullRefresh) {
		++generation_;
		LOG_W("subtitle/grid/renderer")
			<< "Skia renderer failed; switching this grid to wx: " << error;
		if (!fallback_refresh_queued_) {
			fallback_refresh_queued_ = true;
			wxWeakRef<wxWindow> weak_window(&window);
			window.CallAfter([weak_window] {
				if (weak_window)
					weak_window->Refresh(false);
			});
		}
	}
	// The failed retained frame is deliberately not mixed with wx drawing.
	// The queued paint owns the complete next frame using the wx backend.
	return true;
}

std::unique_ptr<GridColumnPainter> SubtitleGridRendererSlot::CreateMeasurementPainter(
	wxFont const& font) {
	if (!UsesSkia())
		return {};
	std::string error;
	auto painter = skia_->CreateMeasurementPainter(font, error);
	if (painter)
		return painter;

	if (error.empty())
		error = "subtitle grid Skia text measurement backend is unavailable";
	if (state_.MarkFailed(error) == SubtitleGridRendererFailureAction::QueueWxFullRefresh) {
		++generation_;
		LOG_W("subtitle/grid/renderer")
			<< "Skia measurement failed; switching this grid to wx: " << error;
	}
	return {};
}

void SubtitleGridRendererSlot::InvalidateSystemFonts() noexcept {
	if (skia_)
		skia_->InvalidateSystemFonts();
}

void SubtitleGridRendererSlot::InvalidateRasterPolicy() noexcept {
	if (skia_)
		raster_policy_refresh_pending_ = true;
}

} // namespace aegisub::grid
