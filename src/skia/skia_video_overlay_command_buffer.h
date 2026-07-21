#pragma once

#include "skia_video_overlay_bounds.h"

#include "../video_overlay_draw_context.h"

#include <cstddef>
#include <functional>
#include <memory>

SkiaOverlayDeviceBounds PlanSkiaOverlayDeviceBounds(
	SkiaOverlayLogicalBounds const& logical_bounds,
	float device_scale,
	int canvas_width,
	int canvas_height) noexcept;

SkiaOverlayDeviceBounds AlignSkiaOverlayDeviceBoundsForAllocation(
	SkiaOverlayDeviceBounds const& device_bounds,
	int canvas_width,
	int canvas_height,
	int alignment = 32) noexcept;

SkiaOverlayDeviceBounds SelectSkiaOverlayBackingBounds(
	SkiaOverlayDeviceBounds const& required_bounds,
	SkiaOverlayDeviceBounds const& reusable_bounds,
	int canvas_width,
	int canvas_height,
	int alignment = 32,
	int guard = 64) noexcept;

class SkiaVideoOverlayCommandBuffer final {
	struct Impl;
	std::unique_ptr<Impl> impl;

	friend class SkiaVideoOverlayRecorder;

public:
	SkiaVideoOverlayCommandBuffer();
	~SkiaVideoOverlayCommandBuffer();
	SkiaVideoOverlayCommandBuffer(SkiaVideoOverlayCommandBuffer&&) noexcept;
	SkiaVideoOverlayCommandBuffer& operator=(SkiaVideoOverlayCommandBuffer&&) noexcept;

	SkiaVideoOverlayCommandBuffer(SkiaVideoOverlayCommandBuffer const&) = delete;
	SkiaVideoOverlayCommandBuffer& operator=(SkiaVideoOverlayCommandBuffer const&) = delete;

	bool Empty() const noexcept;
	bool HasNormalContent() const noexcept;
	bool HasInvertContent() const noexcept;
	std::size_t CommandCount() const noexcept;
	SkiaOverlayLogicalBounds NormalBounds() const noexcept;
	SkiaOverlayLogicalBounds InvertBounds() const noexcept;
	SkiaOverlayLogicalBounds CombinedBounds() const noexcept;
	bool EquivalentTo(SkiaVideoOverlayCommandBuffer const& other) const noexcept;
	void Replay(VideoOverlayDrawContext& target) const;
};

using SkiaVideoOverlayTextMeasurer = std::function<wxSize(
	std::string const&,
	VideoOverlayTextStyle const&)>;

class SkiaVideoOverlayRecorder final : public VideoOverlayDrawContext {
	SkiaVideoOverlayCommandBuffer buffer;
	SkiaVideoOverlayTextMeasurer text_measurer;
	float device_scale = 1.0f;

	wxColour line_colour = *wxWHITE;
	float line_alpha = 1.0f;
	int line_width = 1;
	wxColour fill_colour = *wxWHITE;
	float fill_alpha = 1.0f;
	bool invert = false;

public:
	SkiaVideoOverlayRecorder(
		SkiaVideoOverlayTextMeasurer text_measurer,
		float device_scale);

	SkiaVideoOverlayCommandBuffer TakeBuffer() noexcept;

	void SetLineColour(wxColour const& colour, float alpha = 1.0f, int width = 1) override;
	void SetFillColour(wxColour const& colour, float alpha = 1.0f) override;
	void SetInvert() override;
	void ClearInvert() override;

	void DrawLine(Vector2D p1, Vector2D p2) override;
	void DrawLines(size_t dim, float const *lines, size_t n) override;
	void DrawLineStrip(Vector2D const *points, size_t n) override;
	void DrawRectangle(Vector2D p1, Vector2D p2) override;
	void DrawPolygon(Vector2D const *points, size_t n) override;
	void DrawMultiPolygon(
		std::vector<float> const& points,
		std::vector<int> const& start,
		std::vector<int> const& count,
		Vector2D video_pos,
		Vector2D video_size,
		bool invert_fill) override;
	void DrawCircle(Vector2D center, float radius) override;
	void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) override;

	wxSize MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};
