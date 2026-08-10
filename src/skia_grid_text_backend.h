#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace agi { struct Color; }
class SkRegion;
class SkSurface;
class wxFont;

namespace aegisub::grid {

struct SkiaGridTextRenderTarget {
	// Native CPU raster target when the platform text backend can render into
	// the same pixels as Skia. Null selects the portable Skia compositor.
	void *platform_raster_target = nullptr;
	std::uintptr_t monitor_token = 0;
	bool opaque_target = true;
	bool one_to_one_present = true;
	bool clear_type_requested = false;
};

struct GridTextBackendStats {
	unsigned long long layout_hits = 0;
	unsigned long long layout_misses = 0;
	unsigned long long layout_evictions = 0;
	std::size_t layout_entries = 0;
	std::size_t layout_bytes = 0;
	double layout_ms = 0.0;
	double raster_ms = 0.0;
	bool raster_policy_known = false;
	bool clear_type_active = false;
};

/// Native text authority used by the Skia CPU grid painter.
///
/// On Windows the implementation is DirectWrite from layout through glyph
/// coverage. Other platforms use SkShaper with their native font manager.
class SkiaGridTextBackend {
public:
	virtual ~SkiaGridTextBackend() = default;

	virtual bool SetRenderTarget(
		SkiaGridTextRenderTarget const& target, std::string& error) = 0;
	virtual bool SetFont(wxFont const& font, std::string& error) = 0;
	virtual bool Measure(
		std::wstring_view text, int& width, int& height, std::string& error) = 0;
	virtual bool Draw(
		SkSurface& surface,
		SkRegion const& clip,
		std::wstring_view text,
		int x,
		int top,
		agi::Color const& color,
		std::string& error) = 0;
	virtual void InvalidateSystemFonts() noexcept = 0;
	virtual void InvalidateRasterPolicy() noexcept = 0;
	virtual char const *Name() const noexcept = 0;
	virtual GridTextBackendStats Stats() const noexcept = 0;
};

std::unique_ptr<SkiaGridTextBackend> CreateSkiaGridTextBackend(std::string& error);

} // namespace aegisub::grid
