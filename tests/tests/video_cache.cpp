#include <main.h>

#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_provider_manager.h"

#include <libaegisub/vfr.h>

namespace {
class CountingVideoProvider final : public VideoProvider {
public:
	int get_frame_calls = 0;
	int get_native_frame_calls = 0;
	int colorspace_changes = 0;
	std::string color_space = "BT.709";
	size_t frame_bytes = 16;
	bool provide_native_owner = false;
	std::vector<int> frame_requests;
	std::vector<int> native_frame_requests;
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(2, 2);

	void GetFrame(int n, VideoFrame &frame) override {
		++get_frame_calls;
		frame_requests.push_back(n);
		frame.width = 2;
		frame.height = 2;
		frame.pitch = 8;
		frame.flipped = false;
		frame.data.assign(frame_bytes, static_cast<unsigned char>(n));
	}

	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override {
		++get_native_frame_calls;
		native_frame_requests.push_back(n);
		struct NativeFrameStorage {
			std::array<unsigned char, 4> plane0 = { };
			std::array<unsigned char, 4> plane1 = { };
			std::array<unsigned char, 4> plane2 = { };
		};

		static std::array<unsigned char, 4> fallback_plane = { };
		fallback_plane[0] = static_cast<unsigned char>(n);
		frame = { };
		frame.output_mode = SourceFrameOutputMode::Native;
		frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 7 };
		frame.format_info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 8, 1);
		frame.width = 2;
		frame.height = 2;
		frame.plane_count = frame.format_info.plane_count;
		frame.geometry = geometry;
		if (provide_native_owner) {
			auto storage = std::make_shared<NativeFrameStorage>();
			storage->plane0[0] = static_cast<unsigned char>(n);
			storage->plane1[0] = static_cast<unsigned char>(n + 1);
			storage->plane2[0] = static_cast<unsigned char>(n + 2);
			frame.planes[0] = { storage->plane0.data(), 2, 2, 2 };
			frame.planes[1] = { storage->plane1.data(), 2, 2, 2 };
			frame.planes[2] = { storage->plane2.data(), 2, 2, 2 };
			owner = storage;
		}
		else {
			frame.planes[0] = { fallback_plane.data(), 2, 2, 2 };
			frame.planes[1] = { fallback_plane.data(), 2, 2, 2 };
			frame.planes[2] = { fallback_plane.data(), 2, 2, 2 };
			owner.reset();
		}
		return true;
	}

	void SetColorSpace(std::string const& matrix) override {
		++colorspace_changes;
		color_space = matrix;
	}

	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return color_space; }
	SourceFrameGeometry GetFrameGeometry() const override { return geometry; }
	std::string GetDecoderName() const override { return "counting"; }
};
}

TEST(video_cache, repeated_access_hits_cache) {
	auto raw = new CountingVideoProvider;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	VideoFrame frame;

	cache->GetFrame(7, frame);
	cache->GetFrame(7, frame);

	EXPECT_EQ(1, raw->get_frame_calls);
}

TEST(video_cache, lru_eviction_removes_least_recently_used_frame) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 17;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(1, frame);
	cache->GetFrame(2, frame);
	cache->GetFrame(1, frame);
	cache->GetFrame(3, frame);
	cache->GetFrame(2, frame);

	EXPECT_EQ(4, raw->get_frame_calls);
}

TEST(video_cache, sequential_reverse_access_warms_next_bgra_frame) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(10, frame);
	cache->GetFrame(9, frame);
	EXPECT_EQ((std::vector<int>{10, 9, 8}), raw->frame_requests);

	cache->GetFrame(8, frame);
	EXPECT_EQ((std::vector<int>{10, 9, 8, 7}), raw->frame_requests);
}

TEST(video_cache, sequential_forward_access_warms_next_bgra_frame) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(10, frame);
	cache->GetFrame(11, frame);
	EXPECT_EQ((std::vector<int>{10, 11, 12}), raw->frame_requests);

	cache->GetFrame(12, frame);
	EXPECT_EQ((std::vector<int>{10, 11, 12, 13}), raw->frame_requests);
}

TEST(video_cache, fixed_step_forward_access_warms_matching_bgra_frame) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(2, frame);
	cache->GetFrame(4, frame);
	EXPECT_EQ((std::vector<int>{2, 4, 6}), raw->frame_requests);

	cache->GetFrame(6, frame);
	EXPECT_EQ((std::vector<int>{2, 4, 6, 8}), raw->frame_requests);
}

TEST(video_cache, step_warm_resets_when_direction_changes) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(10, frame);
	cache->GetFrame(9, frame);
	cache->GetFrame(10, frame);

	EXPECT_EQ((std::vector<int>{10, 9, 8}), raw->frame_requests);
}

TEST(video_cache, step_warm_resets_when_delta_changes) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(2, frame);
	cache->GetFrame(4, frame);
	cache->GetFrame(7, frame);

	EXPECT_EQ((std::vector<int>{2, 4, 6, 7}), raw->frame_requests);
}

TEST(video_cache, native_requests_do_not_reset_bgra_step_warm) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	VideoFrame frame;
	SourceFrame native_frame;
	std::shared_ptr<void> owner;

	cache->GetFrame(10, frame);
	cache->GetFrame(11, frame);
	ASSERT_TRUE(cache->GetNativeFrame(20, native_frame, owner));
	cache->GetFrame(12, frame);

	EXPECT_EQ((std::vector<int>{10, 11, 12, 13}), raw->frame_requests);
	EXPECT_EQ((std::vector<int>{20}), raw->native_frame_requests);
}

TEST(video_cache, step_warm_is_disabled_when_cache_cannot_hold_two_frames) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 32;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(10, frame);
	cache->GetFrame(9, frame);

	EXPECT_EQ((std::vector<int>{10, 9}), raw->frame_requests);
}

TEST(video_cache, set_color_space_clears_cache) {
	auto raw = new CountingVideoProvider;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	VideoFrame frame;

	cache->GetFrame(5, frame);
	cache->GetFrame(5, frame);
	cache->SetColorSpace("BT.601");
	cache->GetFrame(5, frame);

	EXPECT_EQ(2, raw->get_frame_calls);
	EXPECT_EQ(1, raw->colorspace_changes);
	EXPECT_EQ("BT.601", raw->color_space);
}

TEST(video_cache, forwards_native_frames_and_geometry_queries) {
	auto raw = new CountingVideoProvider;
	raw->geometry.visible_rect = { 1, 0, 1, 2 };
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	ASSERT_TRUE(cache->GetNativeFrame(9, frame, owner));
	EXPECT_EQ(1, raw->get_native_frame_calls);
	EXPECT_EQ(1, frame.geometry.visible_rect.x);
	EXPECT_EQ(0, frame.geometry.visible_rect.y);
	EXPECT_EQ(1, frame.geometry.visible_rect.width);
	EXPECT_EQ(2, frame.geometry.visible_rect.height);

	auto geometry = cache->GetFrameGeometry();
	EXPECT_EQ(1, geometry.visible_rect.x);
	EXPECT_EQ(0, geometry.visible_rect.y);
	EXPECT_EQ(1, geometry.visible_rect.width);
	EXPECT_EQ(2, geometry.visible_rect.height);
}

TEST(video_cache, native_frames_with_owner_hit_cache) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	ASSERT_TRUE(cache->GetNativeFrame(7, frame, owner));
	ASSERT_TRUE(cache->GetNativeFrame(7, frame, owner));

	EXPECT_EQ(1, raw->get_native_frame_calls);
	ASSERT_TRUE(owner);
	EXPECT_EQ(7, frame.planes[0].data[0]);
	EXPECT_EQ(8, frame.planes[1].data[0]);
	EXPECT_EQ(9, frame.planes[2].data[0]);
}

TEST(video_cache, native_frames_without_owner_are_not_cached) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = false;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	ASSERT_TRUE(cache->GetNativeFrame(7, frame, owner));
	ASSERT_TRUE(cache->GetNativeFrame(7, frame, owner));

	EXPECT_EQ(2, raw->get_native_frame_calls);
	EXPECT_FALSE(owner);
}

TEST(video_cache, native_lru_eviction_removes_least_recently_used_frame) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 23);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	cache->GetNativeFrame(1, frame, owner);
	cache->GetNativeFrame(2, frame, owner);
	cache->GetNativeFrame(1, frame, owner);
	cache->GetNativeFrame(3, frame, owner);
	cache->GetNativeFrame(2, frame, owner);

	EXPECT_EQ(4, raw->get_native_frame_calls);
}

TEST(video_cache, sequential_reverse_access_warms_next_native_frame) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	cache->GetNativeFrame(10, frame, owner);
	cache->GetNativeFrame(9, frame, owner);
	EXPECT_EQ((std::vector<int>{10, 9, 8}), raw->native_frame_requests);

	cache->GetNativeFrame(8, frame, owner);
	EXPECT_EQ((std::vector<int>{10, 9, 8, 7}), raw->native_frame_requests);
}

TEST(video_cache, sequential_forward_access_warms_next_native_frame) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	cache->GetNativeFrame(10, frame, owner);
	cache->GetNativeFrame(11, frame, owner);
	EXPECT_EQ((std::vector<int>{10, 11, 12}), raw->native_frame_requests);

	cache->GetNativeFrame(12, frame, owner);
	EXPECT_EQ((std::vector<int>{10, 11, 12, 13}), raw->native_frame_requests);
}

TEST(video_cache, fixed_step_forward_access_warms_matching_native_frame) {
	auto raw = new CountingVideoProvider;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	SourceFrame frame;
	std::shared_ptr<void> owner;

	cache->GetNativeFrame(2, frame, owner);
	cache->GetNativeFrame(4, frame, owner);
	EXPECT_EQ((std::vector<int>{2, 4, 6}), raw->native_frame_requests);

	cache->GetNativeFrame(6, frame, owner);
	EXPECT_EQ((std::vector<int>{2, 4, 6, 8}), raw->native_frame_requests);
}

TEST(video_cache, bgra_requests_do_not_reset_native_step_warm) {
	auto raw = new CountingVideoProvider;
	raw->frame_bytes = 8;
	raw->provide_native_owner = true;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 64);
	VideoFrame bgra_frame;
	SourceFrame native_frame;
	std::shared_ptr<void> owner;

	ASSERT_TRUE(cache->GetNativeFrame(20, native_frame, owner));
	ASSERT_TRUE(cache->GetNativeFrame(21, native_frame, owner));
	cache->GetFrame(10, bgra_frame);
	ASSERT_TRUE(cache->GetNativeFrame(22, native_frame, owner));

	EXPECT_EQ((std::vector<int>{20, 21, 22, 23}), raw->native_frame_requests);
	EXPECT_EQ((std::vector<int>{10}), raw->frame_requests);
}
