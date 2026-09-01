#include <main.h>

#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"
#include "../../src/motion_track/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;

GrayPatch MakePatch(int frame, int origin_x, int origin_y, int width, int height,
                    std::uint8_t fill) {
	GrayPatch patch;
	patch.frame = frame;
	patch.origin_x = origin_x;
	patch.origin_y = origin_y;
	patch.width = width;
	patch.height = height;
	patch.stride = width;
	patch.gray.resize(size_t(width) * size_t(height));
	for (size_t i = 0; i < patch.gray.size(); ++i)
		patch.gray[i] = static_cast<std::uint8_t>(fill + (i % 251) * 7);
	return patch;
}

TrackerSeed MakeSeed(TrackModel model, GrayView view, int seed_frame) {
	TrackerSeed seed;
	seed.model = model;
	seed.roi = RoiRect{10, 12, 4, 4};
	seed.template_gray = view;
	seed.seed_frame = seed_frame;
	return seed;
}

SyntheticTranslationScene MakeScene() {
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 8;
	config.object_at_frame0 = RoiRect{20, 12, 24, 16};
	config.velocity_x = 1.0;
	return SyntheticTranslationScene(config);
}

GrayPatch FetchScenePatch(SyntheticTranslationScene const& scene, int frame,
                          RoiRect roi) {
	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	EXPECT_EQ(FrameReadStatus::Ok,
	          reader.FetchGray(frame, roi, patch).status);
	return patch;
}

TrackerSeed SeedFrom(GrayPatch const& patch) {
	TrackerSeed seed;
	seed.model = TrackModel::Translation;
	seed.roi = RoiRect{patch.origin_x, patch.origin_y, patch.width,
	                   patch.height};
	seed.template_gray = patch.View();
	seed.seed_frame = patch.frame;
	return seed;
}
}

TEST(motion_track_types, frozen_enum_ordinal_values_are_stable) {
	EXPECT_EQ(0, static_cast<int>(TrackStatus::Ok));
	EXPECT_EQ(1, static_cast<int>(TrackStatus::Failed));
	EXPECT_EQ(2, static_cast<int>(TrackStatus::InvalidInput));
	EXPECT_EQ(3, static_cast<int>(TrackStatus::Unsupported));
	EXPECT_EQ(4, static_cast<int>(TrackStatus::Missing));

	EXPECT_EQ(1, static_cast<int>(TrackModel::Translation));
	EXPECT_EQ(2, static_cast<int>(TrackModel::Similarity));
	EXPECT_EQ(3, static_cast<int>(TrackModel::Homography));

	EXPECT_EQ(0, static_cast<int>(ApplyMode::Compact));
	EXPECT_EQ(1, static_cast<int>(ApplyMode::Exact));

	EXPECT_EQ(1.0, TrackSample{}.transform.matrix[0]);
	EXPECT_EQ(0.0, TrackSample{}.transform.matrix[1]);
	EXPECT_EQ(1.0, TrackSample{}.transform.matrix[4]);
	EXPECT_EQ(0.0, TrackSample{}.transform.matrix[5]);
	EXPECT_EQ(1.0, TrackSample{}.transform.matrix[8]);
	EXPECT_EQ(TrackStatus::Missing, TrackSample{}.status);
}

TEST(motion_track_find_sample, returns_null_for_empty_or_missing_frames) {
	std::vector<TrackSample> samples;
	EXPECT_EQ(nullptr, FindSample(samples, 0));

	samples.push_back(TrackSample{});
	samples.back().frame = 3;
	TrackSample tail{};
	tail.frame = 9;
	samples.push_back(tail);

	EXPECT_EQ(nullptr, FindSample(samples, 2));
	EXPECT_EQ(nullptr, FindSample(samples, 5));
	EXPECT_EQ(nullptr, FindSample(samples, 10));

	auto const* first = FindSample(samples, 3);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(3, first->frame);
	auto const* last = FindSample(samples, 9);
	ASSERT_NE(nullptr, last);
	EXPECT_EQ(9, last->frame);
}

TEST(motion_track_backend, reports_model_and_name) {
	TranslationTrackerBackend backend;
	EXPECT_EQ(TrackModel::Translation, backend.Model());
	EXPECT_EQ("ncc-pyramid", backend.Name());
}

TEST(motion_track_backend, rejects_non_translation_seeds_with_unsupported) {
	TranslationTrackerBackend backend;
	for (auto model : {TrackModel::Similarity, TrackModel::Homography}) {
		EXPECT_EQ(TrackStatus::Unsupported,
		          backend.Reset(MakeSeed(model, MakePatch(-1, 0, 0, 4, 4, 32).View(), 7)));
	}
	EXPECT_EQ(0u, backend.TemplateHash());

	// A rejected seed must leave the backend without a usable template.
	TrackStepRequest request;
	request.image = MakePatch(7, 0, 0, 4, 4, 16).View();
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Step(request).status);
}

TEST(motion_track_backend, rejects_degenerate_seed_views_with_invalid_input) {
	TranslationTrackerBackend backend;

	GrayView null_view;
	EXPECT_EQ(TrackStatus::InvalidInput,
	          backend.Reset(MakeSeed(TrackModel::Translation, null_view, 0)));

	GrayPatch zero_height = MakePatch(-1, 0, 0, 4, 0, 8);
	EXPECT_EQ(TrackStatus::InvalidInput,
	          backend.Reset(MakeSeed(TrackModel::Translation, zero_height.View(), 0)));

	GrayPatch bad_stride = MakePatch(-1, 0, 0, 4, 4, 8);
	bad_stride.stride = 2; // stride < width
	EXPECT_EQ(TrackStatus::InvalidInput,
	          backend.Reset(MakeSeed(TrackModel::Translation, bad_stride.View(), 0)));
}

TEST(motion_track_backend, reset_copies_template_pixels) {
	TranslationTrackerBackend backend;
	GrayPatch patch = MakePatch(5, -6, -7, 12, 8, 40);

	EXPECT_EQ(TrackStatus::Ok,
	          backend.Reset(MakeSeed(TrackModel::Translation, patch.View(), 5)));
	std::uint64_t const hash_before = backend.TemplateHash();
	EXPECT_NE(0u, hash_before);

	// Mutate the caller-owned buffer: the copied template must not change.
	for (auto& byte : patch.gray) byte ^= 0xFF;
	EXPECT_EQ(hash_before, backend.TemplateHash());

	// Re-seeding with different content must change the stored template.
	GrayPatch other = MakePatch(6, 0, 0, 12, 8, 200);
	EXPECT_EQ(TrackStatus::Ok,
	          backend.Reset(MakeSeed(TrackModel::Translation, other.View(), 6)));
	EXPECT_NE(hash_before, backend.TemplateHash());
}

TEST(motion_track_backend, step_survives_seed_buffer_destruction) {
	auto scene = MakeScene();
	std::uint64_t hash_first = 0;
	{
		TranslationTrackerBackend backend;
		GrayPatch patch = FetchScenePatch(scene, 0, RoiRect{20, 12, 24, 16});
		ASSERT_EQ(TrackStatus::Ok,
		          backend.Reset(MakeSeed(TrackModel::Translation, patch.View(), 11)));
		hash_first = backend.TemplateHash();
	} // GrayPatch destroyed here

	TranslationTrackerBackend backend;
	{
		GrayPatch patch = FetchScenePatch(scene, 0, RoiRect{20, 12, 24, 16});
		ASSERT_EQ(TrackStatus::Ok,
		          backend.Reset(MakeSeed(TrackModel::Translation, patch.View(), 11)));
	}
	// Template buffer gone; the backend-owned copy still answers Step on a
	// fresh crop of the same content, and the stored template is unchanged.
	double const center_x = 20.0 + 11.5 + 3.0;
	double const center_y = 12.0 + 7.5;
	auto const ex = int(std::floor(center_x - 11.5 + 0.5)) - 6;
	auto const ey = int(std::floor(center_y - 7.5 + 0.5)) - 6;
	auto patch = FetchScenePatch(scene, 3, RoiRect{ex, ey, 36, 28});

	TrackStepRequest request;
	request.frame = 3;
	request.image = patch.View();
	request.image_origin_x = ex;
	request.image_origin_y = ey;
	request.search_center_x = center_x;
	request.search_center_y = center_y;

	TrackStepResult result = backend.Step(request);
	EXPECT_EQ(TrackStatus::Ok, result.status);
	EXPECT_EQ(3, result.frame);
	EXPECT_NEAR(center_x, result.candidate_center_x, 0.25);
	EXPECT_NEAR(center_y, result.candidate_center_y, 0.25);
	// Same deterministic content re-seeded into a fresh instance must
	// reproduce the identical stored-template checksum.
	EXPECT_NE(0u, hash_first);
	EXPECT_EQ(hash_first, backend.TemplateHash());
}

TEST(motion_track_backend, step_without_reset_is_invalid_input) {
	TranslationTrackerBackend backend;
	TrackStepRequest request;
	request.frame = 3;
	request.image = MakePatch(3, 0, 0, 8, 8, 99).View();
	TrackStepResult result = backend.Step(request);
	EXPECT_EQ(TrackStatus::InvalidInput, result.status);
	EXPECT_EQ(3, result.frame);
}

TEST(motion_track_backend, step_rejects_degenerate_image_views) {
	auto scene = MakeScene();
	TranslationTrackerBackend backend;
	GrayPatch seed = FetchScenePatch(scene, 0, RoiRect{20, 12, 8, 8});
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	// Valid geometry: template from the same scene, crop contains it.
	auto patch = FetchScenePatch(scene, 1, RoiRect{18, 10, 20, 20});
	TrackStepRequest request;
	request.frame = 1;
	request.image = patch.View();
	request.image_origin_x = 18;
	request.image_origin_y = 10;
	request.search_center_x = 24.5 + 1.0;
	request.search_center_y = 15.5;
	EXPECT_NE(TrackStatus::InvalidInput, backend.Step(request).status);

	patch = FetchScenePatch(scene, 1, RoiRect{18, 10, 20, 20});
	request.image.data = nullptr;
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Step(request).status);

	patch = FetchScenePatch(scene, 1, RoiRect{18, 10, 20, 20});
	request.image = patch.View();
	request.image.stride = 4; // stride < width
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Step(request).status);

	patch = FetchScenePatch(scene, 1, RoiRect{18, 10, 8, 0}); // zero height
	request.image = patch.View();
	EXPECT_EQ(TrackStatus::InvalidInput, backend.Step(request).status);
}

// Regression: the no-guess path of the spatial pyramid used to search only
// [0, radius] instead of a window centered on the anchor, so matches to the
// right of / below the expected spot were unreachable. A 3x3 template forces
// level 1 to fail (its half-res 1x1 template has zero variance), exercising
// that exact path.
TEST(motion_track_backend, spatial_fallback_searches_centered_window) {
	TranslationTrackerConfig config;
	TranslationTrackerBackend backend(config);

	GrayPatch templ = MakePatch(-1, 0, 0, 3, 3, 10);
	ASSERT_EQ(TrackStatus::Ok,
	          backend.Reset(MakeSeed(TrackModel::Translation, templ.View(), 0)));

	// Image crop with anchor at (5,5): true match sits at (+5,+3) from it.
	int const w = 24, h = 24;
	GrayPatch image = MakePatch(1, 100, 100, w, h, 0);
	for (int y = 0; y < 3; ++y)
		for (int x = 0; x < 3; ++x)
			image.gray[size_t((8 + y) * w + (10 + x))] =
			    templ.gray[size_t(y * 3 + x)];

	TrackStepRequest request;
	request.frame = 1;
	request.image = image.View();
	request.image_origin_x = 100;
	request.image_origin_y = 100;
	// ROI center convention: search center such that expected left = (5,5).
	request.search_center_x = 100 + 5 + (3 - 1) / 2.0;
	request.search_center_y = 100 + 5 + (3 - 1) / 2.0;
	auto step = backend.Step(request);

	// The match sits exactly at the old window's exclusive edge (+5 from an
	// anchor whose inward radius is 5): reachable only when the fallback
	// window is centered on the anchor.
	ASSERT_EQ(TrackStatus::Ok, step.status);
	// Parabolic subpixel refinement may shift up to +-0.75 px off the
	// integer peak, so allow that band around the exact offset.
	EXPECT_NEAR(request.search_center_x + 5, step.candidate_center_x, 0.76);
	EXPECT_NEAR(request.search_center_y + 3, step.candidate_center_y, 0.76);
	EXPECT_NEAR(std::round(step.candidate_center_x),
	            request.search_center_x + 5, 0.5);
	EXPECT_NEAR(std::round(step.candidate_center_y),
	            request.search_center_y + 3, 0.5);
}

TEST(motion_track_backend, template_refresh_blends_only_when_enabled) {
	auto scene = MakeScene();
	GrayPatch seed = FetchScenePatch(scene, 0, RoiRect{20, 12, 24, 16});

	double const center_x = 20.0 + 11.5 + 3.0;
	double const center_y = 12.0 + 7.5;
	auto const ex = int(std::floor(center_x - 11.5 + 0.5)) - 6;
	auto const ey = int(std::floor(center_y - 7.5 + 0.5)) - 6;

	// Frame 3 with a small brightness patch inside the matched window: a big
	// enough appearance change that blending is observable in the template
	// hash, small enough that the median-based residual is untouched and the
	// match still succeeds.
	auto patch = FetchScenePatch(scene, 3, RoiRect{ex, ey, 36, 28});
	for (int y = 10; y < 12; ++y)
		for (int x = 8; x < 14; ++x) {
			size_t const i = size_t(y) * 36 + size_t(x);
			patch.gray[i] = std::uint8_t(std::min(255, int(patch.gray[i]) + 40));
		}

	TrackStepRequest request;
	request.frame = 3;
	request.image = patch.View();
	request.image_origin_x = ex;
	request.image_origin_y = ey;
	request.search_center_x = center_x;
	request.search_center_y = center_y;

	{
		// Default config: frozen template, hash survives a successful step.
		TranslationTrackerBackend backend;
		ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));
		auto const hash = backend.TemplateHash();
		ASSERT_EQ(TrackStatus::Ok, backend.Step(request).status);
		EXPECT_EQ(hash, backend.TemplateHash());
	}
	{
		TranslationTrackerBackend backend;
		TranslationTrackerConfig config;
		config.template_refresh = true;
		config.refresh_alpha = 0.5;
		backend.SetConfig(config);
		ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));
		auto const hash = backend.TemplateHash();
		ASSERT_EQ(TrackStatus::Ok, backend.Step(request).status);
		EXPECT_NE(hash, backend.TemplateHash());
	}
}
