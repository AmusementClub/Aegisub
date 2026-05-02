#include <main.h>

#include "../../src/video_overlay_helpers.h"

#include <utility>
#include <vector>

namespace {
class CaptureOverlayContext final : public VideoOverlayDrawContext {
public:
	std::vector<std::pair<Vector2D, Vector2D>> lines;

	void SetLineColour(wxColour const&, float = 1.0f, int = 1) override { }
	void SetFillColour(wxColour const&, float = 1.0f) override { }
	void SetInvert() override { }
	void ClearInvert() override { }

	void DrawLine(Vector2D p1, Vector2D p2) override {
		lines.emplace_back(p1, p2);
	}

	void DrawLines(size_t, float const *, size_t) override { }
	void DrawLineStrip(Vector2D const *, size_t) override { }
	void DrawRectangle(Vector2D, Vector2D) override { }
	void DrawPolygon(Vector2D const *, size_t) override { }
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override { }
	void DrawCircle(Vector2D, float) override { }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override { }

	wxSize MeasureText(std::string const&, VideoOverlayTextStyle const&) override {
		return wxSize();
	}

	void DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) override { }
};

void ExpectVectorNear(Vector2D actual, Vector2D expected) {
	EXPECT_NEAR(expected.X(), actual.X(), 1e-4f);
	EXPECT_NEAR(expected.Y(), actual.Y(), 1e-4f);
}
}

TEST(video_overlay_helpers, layout_res_adjusted_z_scale_matches_libass_y_ratio) {
	EXPECT_FLOAT_EQ(
		4.0f,
		video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
			Vector2D(1920, 1080),
			Vector2D(1920, 540)));
	EXPECT_FLOAT_EQ(
		12.0f,
		video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
			Vector2D(1920, 720),
			Vector2D(1920, 1080)));
}

TEST(video_overlay_helpers, layout_res_adjusted_z_scale_falls_back_for_invalid_resolutions) {
	EXPECT_FLOAT_EQ(
		video_overlay_helpers::kPerspectiveZScale,
		video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
			Vector2D(1920, 0),
			Vector2D(1920, 540)));
	EXPECT_FLOAT_EQ(
		video_overlay_helpers::kPerspectiveZScale,
		video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
			Vector2D(1920, 1080),
			Vector2D(1920, 0)));
}

TEST(video_overlay_helpers, scaled_projection_uses_layout_res_adjusted_z_scale) {
	Vector2D const origin(10.0f, 20.0f);
	Vector2D const scale(100.0f, 100.0f);
	Vector2D const point(100.0f, 0.0f);
	float const adjusted_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
		Vector2D(1920, 1080),
		Vector2D(1920, 540));

	auto const default_projected = video_overlay_helpers::ProjectScaledRotatedPoint(
		point, origin, scale, 0.0f, 45.0f, 0.0f);
	auto const adjusted_projected = video_overlay_helpers::ProjectScaledRotatedPoint(
		point, origin, scale, 0.0f, 45.0f, 0.0f, adjusted_z_scale);

	EXPECT_GT(adjusted_projected.X(), default_projected.X());
	EXPECT_FLOAT_EQ(origin.Y(), adjusted_projected.Y());
}

TEST(video_overlay_helpers, projected_scaled_line_passes_adjusted_z_scale_to_overlay_context) {
	CaptureOverlayContext context;
	Vector2D const origin(10.0f, 20.0f);
	Vector2D const scale(100.0f, 100.0f);
	Vector2D const end(100.0f, 0.0f);
	float const adjusted_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
		Vector2D(1920, 1080),
		Vector2D(1920, 540));

	video_overlay_helpers::DrawProjectedLine(
		context,
		Vector2D(0.0f, 0.0f),
		end,
		origin,
		scale,
		0.0f,
		45.0f,
		0.0f,
		adjusted_z_scale);

	ASSERT_EQ(1u, context.lines.size());
	ExpectVectorNear(context.lines[0].first, origin);
	ExpectVectorNear(
		context.lines[0].second,
		video_overlay_helpers::ProjectScaledRotatedPoint(
			end, origin, scale, 0.0f, 45.0f, 0.0f, adjusted_z_scale));
}

TEST(video_overlay_helpers, projected_sheared_line_passes_adjusted_z_scale_to_overlay_context) {
	CaptureOverlayContext context;
	Vector2D const origin(10.0f, 20.0f);
	video_overlay_helpers::Vec3 const end { 100.0f, 0.0f, 0.0f };
	float const adjusted_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(
		Vector2D(1920, 1080),
		Vector2D(1920, 540));

	video_overlay_helpers::DrawProjectedLine(
		context,
		{ 0.0f, 0.0f, 0.0f },
		end,
		origin,
		0.0f,
		45.0f,
		0.0f,
		0.0f,
		0.0f,
		adjusted_z_scale);

	ASSERT_EQ(1u, context.lines.size());
	ExpectVectorNear(context.lines[0].first, origin);
	ExpectVectorNear(
		context.lines[0].second,
		video_overlay_helpers::ProjectRotatedShearedPoint(
			end, origin, 0.0f, 45.0f, 0.0f, 0.0f, 0.0f, adjusted_z_scale));
}
