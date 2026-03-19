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
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(2, 2);

	void GetFrame(int n, VideoFrame &frame) override {
		++get_frame_calls;
		frame.width = 2;
		frame.height = 2;
		frame.pitch = 8;
		frame.flipped = false;
		frame.data.assign(frame_bytes, static_cast<unsigned char>(n));
	}

	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override {
		++get_native_frame_calls;
		static unsigned char plane[4] = { };
		plane[0] = static_cast<unsigned char>(n);
		frame = { };
		frame.output_mode = SourceFrameOutputMode::Native;
		frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 7 };
		frame.format_info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 8, 1);
		frame.width = 2;
		frame.height = 2;
		frame.plane_count = frame.format_info.plane_count;
		frame.geometry = geometry;
		frame.planes[0] = { plane, 2, 2, 2 };
		frame.planes[1] = { plane, 2, 2, 2 };
		frame.planes[2] = { plane, 2, 2, 2 };
		owner.reset();
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
	raw->frame_bytes = 16;
	auto cache = CreateCacheVideoProvider(std::unique_ptr<VideoProvider>(raw), 32);
	VideoFrame frame;

	cache->GetFrame(1, frame);
	cache->GetFrame(2, frame);
	cache->GetFrame(1, frame);
	cache->GetFrame(3, frame);
	cache->GetFrame(2, frame);

	EXPECT_EQ(4, raw->get_frame_calls);
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
