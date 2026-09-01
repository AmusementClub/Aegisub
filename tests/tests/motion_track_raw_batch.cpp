#include <main.h>

#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/motion_track/raw_batch_motion_frame_reader.h"
#include "../../src/motion_track/video_provider_lease.h"

#include "../../src/async_video_provider.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"

#include <libaegisub/make_unique.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {
using namespace aegisub::motion_track;

class FakeSubtitlesProvider final : public SubtitlesProvider {
	void LoadSubtitles(const char*, size_t) override {}
	void DrawSubtitles(VideoFrame&, double) override {}
};

class PatternVideoProvider final : public VideoProvider {
public:
	void GetFrame(int n, VideoFrame& frame) override {
		frame.width = 4;
		frame.height = 3;
		frame.pitch = 4 * 4;
		frame.flipped = flipped;
		frame.data.assign(frame.pitch * frame.height, 0);
		for (int y = 0; y < 3; ++y)
			for (int x = 0; x < 4; ++x)
				SetBgra(x, y, frame, n);
	}
	void SetColorSpace(std::string const&) override {}
	int GetFrameCount() const override { return frame_count; }
	int GetWidth() const override { return 4; }
	int GetHeight() const override { return 3; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetRealColorSpace() const override { return "BT.709"; }
	SourceFrameGeometry GetFrameGeometry() const override {
		return MakeDefaultSourceFrameGeometry(4, 3);
	}
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return {}; }
	bool GetNativeFrame(int, SourceFrame&, std::shared_ptr<void>&) override { return false; }
	std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override {
		return {SourceFrameOutputMode::Bgra8};
	}
	bool SetOutputMode(SourceFrameOutputMode) override { return true; }
	std::string GetDecoderName() const override { return "pattern"; }

	int frame_count = 10;
	bool flipped = false;

private:
	static void SetBgra(int x, int y, VideoFrame& frame, int n) {
		size_t const base = size_t(y) * frame.pitch + size_t(x) * 4;
		frame.data[base + 0] = static_cast<std::uint8_t>(x * 10 + n);     // B
		frame.data[base + 1] = static_cast<std::uint8_t>(y * 20 + n);     // G
		frame.data[base + 2] = static_cast<std::uint8_t>(x + y + n);      // R
		frame.data[base + 3] = 255;
	}
};
}

TEST(motion_track_raw_batch_reader, converts_inbounds_pixels_with_bt601) {
	auto provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<PatternVideoProvider>(),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			RawBatchMotionFrameReader reader(access);
			RawBgraView probe;
			if (access.FetchBgra(2, probe).status != FrameReadStatus::Ok)
				return RawVideoBatchStatus::Error;

			GrayPatch patch;
			auto read = reader.FetchGray(2, RoiRect{0, 0, 4, 3}, patch);
			if (read.status != FrameReadStatus::Ok)
				return RawVideoBatchStatus::Error;

			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 4; ++x) {
					int b = (x * 10 + 2) & 0xFF;
					int g = (y * 20 + 2) & 0xFF;
					int r = (x + y + 2) & 0xFF;
					int expected = (77 * r + 150 * g + 29 * b) >> 8;
					if (patch.gray[size_t(y) * 4 + size_t(x)] != expected)
						return RawVideoBatchStatus::Error;
				}
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(motion_track_raw_batch_reader, out_of_bounds_roi_uses_intersection_mean_fill) {
	auto provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<PatternVideoProvider>(),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			RawBatchMotionFrameReader reader(access);

			// Columns 0..1 inside, columns 2..3 outside.
			GrayPatch patch;
			if (reader.FetchGray(1, RoiRect{2, 0, 4, 3}, patch).status
			    != FrameReadStatus::Ok)
				return RawVideoBatchStatus::Error;

			auto const gray_at = [&](int sx, int sy) {
				int b = (sx * 10 + 1) & 0xFF;
				int g = (sy * 20 + 1) & 0xFF;
				int r = (sx + sy + 1) & 0xFF;
				return static_cast<std::uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
			};
			long long sum = 0;
			long long count = 0;
			for (int y = 0; y < 3; ++y)
				for (int sx = 2; sx < 4; ++sx) {
					sum += gray_at(sx, y);
					++count;
				}
			auto const fill =
				static_cast<std::uint8_t>((sum + count / 2) / count);
			// ROI columns 0..1 are real scene pixels (scene x 2..3); columns
			// 2..3 fall outside and carry the intersection-mean fill.
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 4; ++x) {
					std::uint8_t const expected =
					    x < 2 ? gray_at(2 + x, y) : fill;
					if (patch.gray[size_t(y) * 4 + size_t(x)] != expected)
						return RawVideoBatchStatus::Error;
				}
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(motion_track_provider_lease, acquire_release_and_drain) {
	auto state = VideoProviderLeaseState::Create();
	{
		auto handle = state->Acquire();
		ASSERT_TRUE(handle);
		EXPECT_FALSE(handle.ChangeRequested());
		state->Begin();
		EXPECT_TRUE(handle.ChangeRequested());
		// Retirement denies new leases.
		EXPECT_FALSE(state->Acquire());
	}
	// Handle released: drain completes immediately.
	state->Drain();
	state->Complete();

	auto again = state->Acquire();
	ASSERT_TRUE(again);
	EXPECT_FALSE(again.ChangeRequested());
}

TEST(motion_track_provider_lease, generation_bump_is_visible_to_outstanding_handles) {
	auto state = VideoProviderLeaseState::Create();
	auto handle = state->Acquire();
	ASSERT_TRUE(handle);
	state->Begin(); // bumps the generation
	EXPECT_TRUE(handle.ChangeRequested());
	state->Complete();
}

TEST(motion_track_provider_lease, drain_for_times_out_while_lease_held) {
	auto state = VideoProviderLeaseState::Create();
	auto handle = state->Acquire();
	EXPECT_FALSE(state->DrainFor(std::chrono::milliseconds(50)));
	handle.Release();
	EXPECT_TRUE(state->DrainFor(std::chrono::milliseconds(50)));
}
