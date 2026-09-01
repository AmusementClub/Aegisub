#include <main.h>

#include "../../src/motion_track/synthetic_frame_reader.h"
#include "../../src/motion_track/translation_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using namespace aegisub::motion_track;

RoiRect const kObject{20, 12, 24, 16};

std::uint8_t TextureNoise(int u, int v) {
	std::uint64_t z = (std::uint64_t(u) * 0x9E3779B97F4A7C15ull)
	                ^ (std::uint64_t(v) * 0xBF58476D1CE4E5B9ull + 1);
	z ^= z >> 27;
	z *= 0x94D049BB133111EBull;
	return static_cast<std::uint8_t>((z ^ (z >> 31)) >> 40);
}

SyntheticTranslationScene MakeScene(double vx = 0.0, double vy = 0.0) {
	SyntheticTranslationScene::Config config;
	config.width = 96;
	config.height = 64;
	config.frame_count = 8;
	config.object_at_frame0 = kObject;
	config.velocity_x = vx;
	config.velocity_y = vy;
	return SyntheticTranslationScene(config);
}

GrayPatch FetchPatch(SyntheticTranslationScene const& scene, int frame,
                     RoiRect roi) {
	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	EXPECT_EQ(FrameReadStatus::Ok, reader.FetchGray(frame, roi, patch).status);
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

TrackStepResult StepAt(TranslationTrackerBackend& backend,
                       SyntheticTranslationScene const& scene, int frame,
                       double center_x, double center_y, int radius) {
	auto const ex =
		int(std::floor(center_x - (kObject.w - 1) / 2.0 + 0.5));
	auto const ey =
		int(std::floor(center_y - (kObject.h - 1) / 2.0 + 0.5));
	RoiRect crop{ex - radius, ey - radius, kObject.w + 2 * radius,
	             kObject.h + 2 * radius};

	SyntheticFrameReader reader(scene);
	GrayPatch patch;
	if (reader.FetchGray(frame, crop, patch).status != FrameReadStatus::Ok)
		return TrackStepResult{};

	TrackStepRequest request;
	request.frame = frame;
	request.image = patch.View();
	request.image_origin_x = crop.x;
	request.image_origin_y = crop.y;
	request.search_center_x = center_x;
	request.search_center_y = center_y;
	return backend.Step(request); // patch alive through the call
}
}

TEST(motion_track_backend_real, gain_and_bias_keep_ok_with_same_center) {
	auto scene = MakeScene();
	TranslationTrackerBackend backend(
		TranslationTrackerConfig{/*ncc_min*/ 0.55, /*residual_max*/ 80.0});
	GrayPatch seed = FetchPatch(scene, 0, kObject);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	// Scale+bias every pixel of the search crops: zero-mean NCC is invariant
	// to linear photometric changes, so tracking must survive.
	SyntheticTranslationScene scaled_scene = MakeScene(3.0, 0.0);

	double const center_x = 20.0 + 11.5 + 3.0;
	double const center_y = 12.0 + 7.5;
	auto const ex = int(std::floor(center_x - 11.5 + 0.5)) - 8;
	auto const ey = int(std::floor(center_y - 7.5 + 0.5)) - 8;
	RoiRect crop{ex, ey, 40, 32};
	SyntheticFrameReader reader(scaled_scene);
	GrayPatch patch;
	ASSERT_EQ(FrameReadStatus::Ok,
	          reader.FetchGray(1, crop, patch).status);
	for (auto& byte : patch.gray)
		byte = static_cast<std::uint8_t>(
			std::min(255, int(byte) * 13 / 10 + 9));

	TrackStepRequest request;
	request.frame = 1;
	request.image = patch.View();
	request.image_origin_x = crop.x;
	request.image_origin_y = crop.y;
	request.search_center_x = center_x;
	request.search_center_y = center_y;
	auto const result = backend.Step(request);
	EXPECT_EQ(TrackStatus::Ok, result.status);
	EXPECT_NEAR(center_x, result.candidate_center_x, 0.25);
	EXPECT_NEAR(center_y, result.candidate_center_y, 0.15);
}

TEST(motion_track_backend_real, occlusion_of_half_template_fails_single_frame) {
	auto scene = MakeScene(4.0, 0.0);
	TranslationTrackerBackend backend;
	GrayPatch seed = FetchPatch(scene, 0, kObject);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	// Stamp unrelated noise over >=50% of the object's footprint at frame 1.
	SyntheticFrameReader reader(scene);
	double const cx = 20.0 + 11.5 + 4.0;
	double const cy = 12.0 + 7.5;
	int const radius = 8;
	auto const ex = int(std::floor(cx - 11.5 + 0.5)) - radius;
	auto const ey = int(std::floor(cy - 7.5 + 0.5)) - radius;
	GrayPatch patch;
	ASSERT_EQ(FrameReadStatus::Ok,
	          reader.FetchGray(1, RoiRect{ex, ey, kObject.w + 16, kObject.h + 16},
	                           patch)
	              .status);
	for (int y = 0; y < kObject.h / 2 + 2; ++y)
		for (int x = 0; x < kObject.w; ++x)
			patch.gray[size_t(8 + y) * size_t(patch.stride) + size_t(8 + x)] =
			    TextureNoise(x + 91, y + 37);

	TrackStepRequest request;
	request.frame = 1;
	request.image = patch.View();
	request.image_origin_x = ex;
	request.image_origin_y = ey;
	request.search_center_x = cx;
	request.search_center_y = cy;
	auto const result = backend.Step(request);
	EXPECT_EQ(TrackStatus::Failed, result.status);
	EXPECT_NE(TrackFailureReason::None, result.failure);
}

TEST(motion_track_backend_real, subpixel_velocity_tracks_within_tolerance) {
	for (double vy : {0.25, 0.5}) {
		auto scene = MakeScene(0.0, vy);
		TranslationTrackerBackend backend;
		GrayPatch seed = FetchPatch(scene, 0, kObject);
		ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

		double center_x = 20.0 + 11.5;
		double center_y = 12.0 + 7.5;
		for (int frame = 1; frame <= 6; ++frame) {
			center_y += vy;
			auto const result = StepAt(backend, scene, frame, center_x,
			                           center_y, 6);
			ASSERT_EQ(TrackStatus::Ok, result.status) << "frame " << frame;
			EXPECT_NEAR(center_x, result.candidate_center_x, 0.25)
			    << "frame " << frame;
			EXPECT_NEAR(center_y, result.candidate_center_y, 0.25)
			    << "frame " << frame;
		}
	}
}

TEST(motion_track_backend_real, moving_crop_accumulates_absolute_centers) {
	auto scene = MakeScene(5.0, 0.0);
	TranslationTrackerBackend backend;
	GrayPatch seed = FetchPatch(scene, 0, kObject);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	double center_x = 20.0 + 11.5;
	double const center_y = 12.0 + 7.5;
	for (int frame = 1; frame <= 5; ++frame) {
		center_x += 5.0;
		auto const result =
			StepAt(backend, scene, frame, center_x, center_y, 8);
		ASSERT_EQ(TrackStatus::Ok, result.status) << "frame " << frame;
		EXPECT_NEAR(center_x, result.candidate_center_x, 0.25)
		    << "frame " << frame;
	}
}

TEST(motion_track_backend_real, tracking_stays_inside_search_radius) {
	auto scene = MakeScene(1.0, 0.0);
	TranslationTrackerBackend backend;
	GrayPatch seed = FetchPatch(scene, 0, kObject);
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(SeedFrom(seed)));

	auto ok_result =
		StepAt(backend, scene, 2, 20.0 + 11.5 + 2.0, 12.0 + 7.5, 6);
	EXPECT_EQ(TrackStatus::Ok, ok_result.status);
	EXPECT_NEAR(20.0 + 11.5 + 2.0, ok_result.candidate_center_x, 0.25);

	// Crop over pure background (object far away): flat-window NccLow.
	auto far_result =
		StepAt(backend, scene, 1, 80.5, 50.5, 2);
	EXPECT_EQ(TrackStatus::Failed, far_result.status);
}

TEST(motion_track_backend_real, flat_search_window_fails) {
	TranslationTrackerBackend backend;
	std::vector<std::uint8_t> flat_template(size_t(24) * 16, 90);
	TrackerSeed seed;
	seed.model = TrackModel::Translation;
	seed.roi = RoiRect{0, 0, 24, 16};
	seed.template_gray = GrayView{flat_template.data(), 24, 24, 16};
	seed.seed_frame = 0;
	ASSERT_EQ(TrackStatus::Ok, backend.Reset(seed));

	auto scene = MakeScene();
	auto const result = StepAt(backend, scene, 1, 31.5, 19.5, 6);
	EXPECT_EQ(TrackStatus::Failed, result.status);
}

namespace {
// Vertical stripes with a 12 px period: every period-aligned offset
// correlates exactly like the accepted one.
std::vector<std::uint8_t> MakeStripes(int width, int height) {
	std::vector<std::uint8_t> stripes(size_t(width) * size_t(height));
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			stripes[size_t(y) * width + x] =
			    static_cast<std::uint8_t>(((x / 6) % 2 == 0) ? 220 : 30);
	return stripes;
}

TrackStepResult StepOnStripes(TranslationTrackerBackend& backend,
                              int image_size, int tw, int th) {
	auto const stripes = MakeStripes(image_size, image_size);
	TrackerSeed seed;
	seed.model = TrackModel::Translation;
	seed.roi = RoiRect{0, 0, tw, th};
	seed.template_gray =
	    GrayView{stripes.data(), image_size, tw, th};
	seed.seed_frame = 0;
	if (backend.Reset(seed) != TrackStatus::Ok)
		return TrackStepResult{};

	int const left = (image_size - tw) / 2;
	TrackStepRequest request;
	request.frame = 1;
	request.image = GrayView{stripes.data(), image_size, image_size,
	                         image_size};
	request.image_origin_x = 0;
	request.image_origin_y = 0;
	request.search_center_x = left + (tw - 1) / 2.0;
	request.search_center_y = left + (th - 1) / 2.0;
	return backend.Step(request);
}
}

TEST(motion_track_backend_real, periodic_stripes_flag_ambiguous_peak) {
	// A 24x24 template spans two stripe periods inside a 96x96 crop: the NCC
	// surface has equally strong maxima one period away, so the step must be
	// rejected instead of silently locking onto one repetition.
	TranslationTrackerBackend backend;
	auto const result = StepOnStripes(backend, 96, 24, 24);
	EXPECT_EQ(TrackStatus::Failed, result.status);
	EXPECT_EQ(TrackFailureReason::AmbiguousPeak, result.failure);
}

TEST(motion_track_backend_real, ambiguity_check_can_be_disabled) {
	TranslationTrackerConfig config;
	config.ambiguity_check = false;
	TranslationTrackerBackend backend(config);
	auto const result = StepOnStripes(backend, 96, 24, 24);
	EXPECT_EQ(TrackStatus::Ok, result.status);
}
