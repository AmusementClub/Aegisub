#include <main.h>

#include "../../src/visual_guide_overlay.h"
#include "../../src/video_overlay_draw_context.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
VisualGuideViewport MakeViewport() {
	return {
		100.0, 50.0, 800.0, 400.0,
		400.0, 200.0,
	};
}

VisualGuideSnapshotView ViewFrom(VisualGuideSnapshot const& snapshot) {
	return { snapshot.generation, snapshot.guides, snapshot.selected_id };
}

class CaptureOverlayContext final : public VideoOverlayDrawContext {
public:
	struct Polygon {
		std::vector<Vector2D> points;
		wxColour fill_colour;
		float fill_alpha = 0.0f;
	};

	struct Rectangle {
		Vector2D first;
		Vector2D second;
	};

	struct Text {
		std::string value;
		int x = 0;
		int y = 0;
	};

	wxColour line_colour;
	float line_alpha = 1.0f;
	int line_width = 1;
	wxColour fill_colour;
	float fill_alpha = 1.0f;
	std::vector<Polygon> polygons;
	std::vector<Rectangle> rectangles;
	std::vector<Text> text;
	int invert_count = 0;

	void SetLineColour(wxColour const& colour, float alpha = 1.0f, int width = 1) override {
		line_colour = colour;
		line_alpha = alpha;
		line_width = width;
	}
	void SetFillColour(wxColour const& colour, float alpha = 1.0f) override {
		fill_colour = colour;
		fill_alpha = alpha;
	}
	void SetInvert() override { ++invert_count; }
	void ClearInvert() override { }
	void DrawLine(Vector2D, Vector2D) override { }
	void DrawLines(size_t, float const*, size_t) override { }
	void DrawLineStrip(Vector2D const*, size_t) override { }
	void DrawRectangle(Vector2D first, Vector2D second) override {
		rectangles.push_back({ first, second });
	}
	void DrawPolygon(Vector2D const* points, size_t n) override {
		Polygon polygon;
		polygon.points.assign(points, points + n);
		polygon.fill_colour = fill_colour;
		polygon.fill_alpha = fill_alpha;
		polygons.push_back(std::move(polygon));
	}
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override { }
	void DrawCircle(Vector2D, float) override { }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override { }

	wxSize MeasureText(std::string const& value, VideoOverlayTextStyle const&) override {
		return wxSize(static_cast<int>(value.size()) * 7, 12);
	}
	void DrawText(std::string const& value, int x, int y, VideoOverlayTextStyle const&) override {
		text.push_back({ value, x, y });
	}
};
}

TEST(visual_guide_overlay, maps_script_points_through_video_viewport) {
	auto const viewport = MakeViewport();
	auto const script = VisualGuideToCanvas({ 100.0, 50.0 }, viewport);
	EXPECT_FLOAT_EQ(300.0f, script.X());
	EXPECT_FLOAT_EQ(150.0f, script.Y());

	auto const round_trip = CanvasToVisualGuide(Vector2D(300.0f, 150.0f), viewport);
	ASSERT_TRUE(round_trip);
	EXPECT_DOUBLE_EQ(100.0, round_trip->x);
	EXPECT_DOUBLE_EQ(50.0, round_trip->y);
}

TEST(visual_guide_overlay, refuses_invalid_dimensions_without_dividing_by_zero) {
	auto viewport = MakeViewport();
	viewport.script_height = 0.0;
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	viewport = MakeViewport();
	viewport.canvas_width = 0.0;
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	viewport = MakeViewport();
	viewport.canvas_x = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(IsVisualGuideViewportMappable(viewport));
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 1.0, 2.0 };
	guide.second = { 3.0, 4.0 };
	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	CaptureOverlayContext context;
	VisualGuideOverlay().Draw(context, viewport, ViewFrom(snapshot), {});
	EXPECT_TRUE(context.polygons.empty());
	EXPECT_TRUE(context.rectangles.empty());
	EXPECT_TRUE(context.text.empty());
}

TEST(visual_guide_overlay, formats_measurement_values_without_negative_zero) {
	EXPECT_EQ("+400", FormatVisualGuideLabelNumber(400.0, true));
	EXPECT_EQ("-220.25", FormatVisualGuideLabelNumber(-220.25, true));
	EXPECT_EQ("0", FormatVisualGuideLabelNumber(-0.0001, true));
	EXPECT_EQ("456.51", FormatVisualGuideLabelNumber(456.508488, false));
	EXPECT_EQ("0", FormatVisualGuideLabelNumber(std::numeric_limits<double>::infinity(), false));
}

TEST(visual_guide_overlay, draws_selected_measurement_with_filled_quads_and_label) {
	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 100.0, 50.0 };
	guide.second = { 200.0, 100.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	snapshot.selected_id = guide.id;

	CaptureOverlayContext context;
	VisualGuideOverlayStyle style;
	style.line_colour = wxColour(10, 20, 30);
	style.highlight_colour = wxColour(40, 50, 60);
	VisualGuideOverlay().Draw(context, MakeViewport(), ViewFrom(snapshot), style);

	// Outline quad then core quad.
	ASSERT_EQ(2u, context.polygons.size());
	EXPECT_EQ(4u, context.polygons[0].points.size());
	EXPECT_EQ(4u, context.polygons[1].points.size());
	EXPECT_EQ(40, context.polygons[1].fill_colour.Red());
	EXPECT_EQ(50, context.polygons[1].fill_colour.Green());
	EXPECT_EQ(60, context.polygons[1].fill_colour.Blue());
	EXPECT_FLOAT_EQ(1.0f, context.polygons[1].fill_alpha);
	EXPECT_EQ(0, context.invert_count);

	// Label background rectangle plus three metric lines (no unit row).
	ASSERT_EQ(1u, context.rectangles.size());
	EXPECT_GE(context.rectangles[0].first.X(), 100.0f);
	EXPECT_GE(context.rectangles[0].first.Y(), 50.0f);
	EXPECT_LE(context.rectangles[0].second.X(), 900.0f);
	EXPECT_LE(context.rectangles[0].second.Y(), 450.0f);
	ASSERT_EQ(3u, context.text.size());
	EXPECT_EQ("dX +100", context.text[0].value);
	EXPECT_EQ("dY +50", context.text[1].value);
	EXPECT_EQ("L 111.8", context.text[2].value);
}

TEST(visual_guide_overlay, skips_guides_when_viewport_is_unmappable) {
	auto viewport = MakeViewport();
	viewport.script_width = 0.0;

	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 1.0, 2.0 };
	guide.second = { 3.0, 4.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	CaptureOverlayContext context;
	VisualGuideOverlay().Draw(context, viewport, ViewFrom(snapshot), {});

	EXPECT_TRUE(context.polygons.empty());
	EXPECT_TRUE(context.rectangles.empty());
	EXPECT_TRUE(context.text.empty());
}
