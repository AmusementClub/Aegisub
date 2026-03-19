#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/async_video_provider.h"
#include "../../src/export_fixstyle.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/subtitle_overlay_blend.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/vfr.h>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <mutex>

namespace {
struct VideoProviderState {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<int> requested_frames;
	bool block_next = false;
	bool entered = false;
	bool released = false;
};

struct FakeNativeFrameStorage {
	std::vector<unsigned char> plane0;
	std::vector<unsigned char> plane1;
};

class FakeVideoProvider final : public VideoProvider {
	std::shared_ptr<VideoProviderState> state;

public:
	std::string color_space = "BT.709";
	std::string real_color_space = "BT.709";
	SourceFrameNativeFormatIdentity native_format = { };
	SourceFrameChromaLocation native_chroma_location = SourceFrameChromaLocation::Unknown;
	SourceFrameGeometry native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	std::vector<SourceFrameOutputMode> available_modes = { SourceFrameOutputMode::Bgra8 };
	SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;

	explicit FakeVideoProvider(std::shared_ptr<VideoProviderState> state)
	: state(std::move(state)) {
	}

	void GetFrame(int n, VideoFrame &frame) override {
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->requested_frames.push_back(n);
			if (state->block_next) {
				state->entered = true;
				state->cv.notify_all();
			}
		}

		std::unique_lock<std::mutex> lock(state->mutex);
		if (state->block_next) {
			state->cv.wait(lock, [&] { return state->released; });
			state->block_next = false;
			state->released = false;
		}
		lock.unlock();

		frame.width = 2;
		frame.height = 2;
		frame.pitch = 8;
		frame.flipped = false;
		frame.data.assign(16, 0);
		frame.data[0] = static_cast<unsigned char>(n);
	}

	void SetColorSpace(std::string const& matrix) override { color_space = matrix; }
	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return color_space; }
	std::string GetRealColorSpace() const override { return real_color_space; }
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return native_format; }
	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override {
		if (output_mode != SourceFrameOutputMode::Native)
			return false;

		auto storage = std::make_shared<FakeNativeFrameStorage>();
		storage->plane0.resize(4, 0);
		storage->plane1.resize(4, 0);
		storage->plane0[0] = static_cast<unsigned char>(n);
		storage->plane1[0] = static_cast<unsigned char>(n + 1);

		frame = { };
		frame.output_mode = SourceFrameOutputMode::Native;
		frame.native_format = native_format.IsValid()
			? native_format
			: SourceFrameNativeFormatIdentity{ SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 7 };
		frame.format_info = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
		frame.width = 2;
		frame.height = 2;
		frame.flipped = false;
		frame.plane_count = frame.format_info.plane_count;
		frame.geometry = native_geometry;
		frame.color = SourceFrameColorMetadataFromLegacyColorSpace(color_space);
		frame.chroma_location = native_chroma_location;
		frame.planes[0] = { storage->plane0.data(), 2, 2, 2 };
		frame.planes[1] = { storage->plane1.data(), 4, 1, 1 };
		owner = storage;
		return true;
	}
	std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override { return available_modes; }
	bool SetOutputMode(SourceFrameOutputMode mode) override {
		if (std::find(available_modes.begin(), available_modes.end(), mode) == available_modes.end())
			return false;
		output_mode = mode;
		return true;
	}
	std::string GetDecoderName() const override { return "fake"; }
};

class FakeSubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	void DrawSubtitles(VideoFrame &dst, double) override {
		if (dst.data.size() < 2)
			dst.data.resize(2);
		dst.data[1] = static_cast<unsigned char>(load_calls);
	}
};

class FakeOverlaySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);
		auto *pixel = overlay.planes[0].data;
		pixel[0] = 10;
		pixel[1] = 20;
		pixel[2] = 30;
		pixel[3] = 128;
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeDirtyRectOverlaySubtitlesProvider final : public SubtitlesProvider {
	std::vector<SubtitleOverlayDirtyRect> dirty_rects;
	int render_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		++render_calls;
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);

		auto* pixel = overlay.planes[0].data + 4;
		pixel[0] = 5;
		pixel[1] = 6;
		pixel[2] = 7;
		pixel[3] = 255;

		dirty_rects.clear();
		if (render_calls == 1)
			dirty_rects.push_back({ 1, 0, 1, 1 });

		overlay.dirty_rects = dirty_rects.empty() ? nullptr : dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(dirty_rects.size());
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeDropSensitiveOverlaySubtitlesProvider final : public SubtitlesProvider {
	std::vector<SubtitleOverlayDirtyRect> dirty_rects;
	int render_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		++render_calls;
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);

		auto* pixel = overlay.planes[0].data + 4;
		pixel[0] = static_cast<unsigned char>(10 + render_calls);
		pixel[1] = 20;
		pixel[2] = 30;
		pixel[3] = 255;

		dirty_rects.clear();
		if (render_calls == 1)
			dirty_rects.push_back({ 0, 0, overlay.width, overlay.height });

		overlay.dirty_rects = dirty_rects.empty() ? nullptr : dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(dirty_rects.size());
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeInvisibleOverlaySubtitlesProvider final : public SubtitlesProvider {
private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		overlay.premultiplied_alpha = true;
		overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
		overlay.has_visible_content = false;
		overlay.dirty_rects = nullptr;
		overlay.dirty_rect_count = 0;
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeCompatibilityOnlySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;
	int render_overlay_calls = 0;
	int draw_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::CompatibilityFrameOnly;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay&, double) override {
		++render_overlay_calls;
		return true;
	}

	void DrawSubtitles(VideoFrame &dst, double) override {
		++draw_calls;
		if (dst.data.size() < 2)
			dst.data.resize(2);
		dst.data[1] = static_cast<unsigned char>(10 + load_calls);
	}
};

struct RecordedFrame {
	int frame_number = -1;
	int subtitle_generation = -1;
	double time = 0.0;
	bool has_overlay = false;
	int overlay_dirty_rect_count = 0;
	bool overlay_force_full_upload = false;
};

class EventRecorder {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<RecordedFrame> frames;

public:
	void operator()(std::unique_ptr<wxEvent> evt) {
		if (evt->GetEventType() != EVT_FRAME_READY)
			return;

		auto *frame_evt = static_cast<FrameReadyEvent *>(evt.get());
		auto display_frame = frame_evt->packet.DisplayFrame();
		RecordedFrame frame;
		frame.frame_number = display_frame && !display_frame->data.empty() ? display_frame->data[0] : -1;
		frame.subtitle_generation = display_frame && display_frame->data.size() > 1 ? display_frame->data[1] : -1;
		frame.time = frame_evt->time;
		frame.has_overlay = frame_evt->packet.has_subtitle_overlay;
		frame.overlay_dirty_rect_count = frame_evt->packet.subtitle_overlay.dirty_rect_count;
		frame.overlay_force_full_upload = frame_evt->packet.subtitle_overlay.force_full_upload;

		{
			std::lock_guard<std::mutex> lock(mutex);
			frames.push_back(frame);
		}
		cv.notify_all();
	}

	bool WaitForCount(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return frames.size() >= count; });
	}

	std::vector<RecordedFrame> Snapshot() {
		std::lock_guard<std::mutex> lock(mutex);
		return frames;
	}
};

AssFile MakeSubtitleFile(std::string const& text) {
	AssFile file;
	auto *line = new AssDialogue;
	line->Start = 0;
	line->End = 5000;
	line->Row = 0;
	line->Text = text;
	file.Events.push_back(*line);
	return file;
}
}

std::vector<std::string> VideoProviderFactory::GetClasses() { return {}; }
std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() { return {}; }
std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *) { return nullptr; }
std::vector<std::string> SubtitlesProviderFactory::GetClasses() { return {}; }
std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(agi::BackgroundRunner *) { return nullptr; }
void SubtitlesProvider::LoadSubtitles(AssFile *, int) {
	static const char payload[] = "test";
	LoadSubtitles(payload, sizeof(payload) - 1);
}
void AssFixStylesFilter::ProcessSubs(AssFile *) { }

TEST(async_video_provider, request_frame_keeps_only_latest_pending_render) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	provider.RequestFrame(1, 1000);

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.RequestFrame(2, 2000);
	provider.RequestFrame(3, 3000);
	provider.RequestFrame(4, 4000);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(4, frames.back().frame_number);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{1, 4}), state->requested_frames);
}

TEST(async_video_provider, load_subtitles_invalidates_stale_render_result) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto first = MakeSubtitleFile("old");
	auto second = MakeSubtitleFile("new");

	provider.LoadSubtitles(&first);
	provider.RequestFrame(1, 1000);

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.LoadSubtitles(&second);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(1, frames.back().frame_number);
	EXPECT_EQ(2, frames.back().subtitle_generation);
}

TEST(async_video_provider, get_frame_flushes_pending_subtitle_state) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("sync");
	provider.LoadSubtitles(&subtitle_file);

	auto frame = provider.GetFrame(7, 7000);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 2u);
	EXPECT_EQ(7, frame->data[0]);
	EXPECT_EQ(1, frame->data[1]);
}

TEST(async_video_provider, get_render_packet_exposes_source_frame_and_overlay) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(9, 9000);
	ASSERT_TRUE(packet.source_frame_storage);
	ASSERT_TRUE(packet.composited_frame_storage);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_TRUE(packet.source_frame.IsValid());
	EXPECT_TRUE(packet.subtitle_overlay.IsValid());
	EXPECT_EQ("BT.709", packet.source_frame.color.matrix);
	EXPECT_EQ("BT.709", packet.source_frame.color.primaries);
	EXPECT_EQ(SourceFrameColorRange::Full, packet.source_frame.color.range);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
	EXPECT_EQ(0, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(0, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.visible_rect.height);
	EXPECT_TRUE(packet.subtitle_overlay.premultiplied_alpha);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, packet.subtitle_overlay.composition_mode);
	EXPECT_EQ(SubtitleOverlayCoordinateSpace::SourceStorage, packet.subtitle_overlay.coordinate_space);
	EXPECT_EQ(9, packet.source_frame_storage->data[0]);
	EXPECT_GT(packet.composited_frame_storage->data[0], packet.source_frame_storage->data[0]);
	EXPECT_GT(packet.composited_frame_storage->data[1], packet.source_frame_storage->data[1]);
	EXPECT_GT(packet.composited_frame_storage->data[2], packet.source_frame_storage->data[2]);
	EXPECT_EQ(128, packet.subtitle_overlay.planes[0].data[3]);
	ASSERT_EQ(1, packet.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].height);
}

TEST(async_video_provider, color_space_override_updates_effective_source_frame_metadata) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	provider.SetColorSpace("TV.601");
	auto packet = provider.GetRenderPacket(3, 3000);

	EXPECT_EQ("TV.601", packet.source_frame.color.matrix);
	EXPECT_EQ("BT.601", packet.source_frame.color.primaries);
	EXPECT_EQ(SourceFrameColorRange::Full, packet.source_frame.color.range);
}

TEST(async_video_provider, bgra_source_frame_preserves_upstream_native_format_identity) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 42 };
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto packet = provider.GetRenderPacket(3, 3000);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, packet.source_frame.native_format.format_namespace);
	EXPECT_EQ(42, packet.source_frame.native_format.format_id);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
}

TEST(async_video_provider, native_source_mode_returns_native_source_frame_packet) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 99 };
	video->native_chroma_location = SourceFrameChromaLocation::TopCenter;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000, true);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, packet.source_frame.native_format.format_namespace);
	EXPECT_EQ(99, packet.source_frame.native_format.format_id);
	EXPECT_EQ(SourceFrameChromaLocation::TopCenter, packet.source_frame.chroma_location);
	EXPECT_TRUE(packet.source_frame.IsValid());
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.visible_rect.height);
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_FALSE(static_cast<bool>(packet.composited_frame_storage));
	EXPECT_FALSE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_source_only_rotation_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000, true);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
}

TEST(async_video_provider, native_source_mode_falls_back_to_bgra_when_subtitle_path_cannot_yet_follow_rotation) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Bgra8, packet.source_frame.pixel_format);
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
}

TEST(async_video_provider, preferred_source_modes_choose_native_for_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Native, video->output_mode);
}

TEST(async_video_provider, compatibility_subtitle_mode_forces_bgra8_output_mode) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_FALSE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, video->output_mode);
}

TEST(async_video_provider, premultiplied_overlay_provider_can_supply_dirty_rects) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeDirtyRectOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(9, 9000);
	auto second = provider.GetRenderPacket(9, 9000);

	ASSERT_TRUE(first.has_subtitle_overlay);
	ASSERT_TRUE(second.has_subtitle_overlay);
	ASSERT_EQ(1, first.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(0, first.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].height);
	EXPECT_EQ(0, second.subtitle_overlay.dirty_rect_count);
}

TEST(async_video_provider, invisible_premultiplied_overlay_is_not_forwarded_as_visible_overlay_packet) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeInvisibleOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(9, 9000);
	EXPECT_FALSE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, compatibility_only_backend_uses_single_legacy_render) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.source_frame_storage);
	ASSERT_TRUE(packet.composited_frame_storage);
	EXPECT_EQ(0, subs->render_overlay_calls);
	EXPECT_EQ(1, subs->draw_calls);
	EXPECT_EQ(5, packet.source_frame_storage->data[0]);
	EXPECT_EQ(11, packet.composited_frame_storage->data[1]);

	SubtitleOverlayStorage storage;
	SubtitleOverlay extracted_overlay;
	EXPECT_TRUE(ExtractOpaqueBgraDifferenceOverlay(*packet.source_frame_storage, *packet.composited_frame_storage, storage, extracted_overlay));
	EXPECT_EQ(1, extracted_overlay.width);
	EXPECT_EQ(1, extracted_overlay.height);

	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, packet.subtitle_overlay.composition_mode);
	EXPECT_TRUE(packet.subtitle_overlay.premultiplied_alpha);
	EXPECT_EQ(SubtitleOverlayCoordinateSpace::SourceStorage, packet.subtitle_overlay.coordinate_space);
	EXPECT_EQ(2, packet.subtitle_overlay.width);
	EXPECT_EQ(2, packet.subtitle_overlay.height);
	ASSERT_EQ(1, packet.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].height);
	EXPECT_EQ(255, packet.subtitle_overlay.planes[0].data[3]);
}

TEST(async_video_provider, compatibility_overlay_reuses_surface_when_content_is_stable) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	auto second = provider.GetRenderPacket(5, 5000);

	ASSERT_TRUE(first.has_subtitle_overlay);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_GT(first.subtitle_overlay.dirty_rect_count, 0);
	EXPECT_EQ(0, second.subtitle_overlay.dirty_rect_count);
}

TEST(async_video_provider, compatibility_overlay_common_path_cycles_between_two_storage_slots) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	auto* first_storage = first.subtitle_overlay_storage.get();
	ASSERT_TRUE(first_storage);

	auto second = provider.GetRenderPacket(5, 5000);
	auto* second_storage = second.subtitle_overlay_storage.get();
	ASSERT_TRUE(second_storage);

	first = { };

	auto third = provider.GetRenderPacket(5, 5000);
	auto* third_storage = third.subtitle_overlay_storage.get();
	ASSERT_TRUE(third_storage);

	EXPECT_NE(first_storage, second_storage);
	EXPECT_EQ(first_storage, third_storage);
}

TEST(async_video_provider, compatibility_overlay_uses_overflow_only_when_two_slots_are_held) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	auto* first_storage = first.subtitle_overlay_storage.get();
	ASSERT_TRUE(first_storage);

	auto second = provider.GetRenderPacket(5, 5000);
	auto* second_storage = second.subtitle_overlay_storage.get();
	ASSERT_TRUE(second_storage);
	ASSERT_NE(first_storage, second_storage);

	auto third = provider.GetRenderPacket(5, 5000);
	auto* third_storage = third.subtitle_overlay_storage.get();
	ASSERT_TRUE(third_storage);
	EXPECT_NE(third_storage, first_storage);
	EXPECT_NE(third_storage, second_storage);

	second = { };

	auto fourth = provider.GetRenderPacket(5, 5000);
	auto* fourth_storage = fourth.subtitle_overlay_storage.get();
	ASSERT_TRUE(fourth_storage);
	EXPECT_EQ(second_storage, fourth_storage);
}

TEST(async_video_provider, dropped_packet_forces_full_overlay_upload_on_next_delivered_event) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeDropSensitiveOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	provider.RequestFrame(1, 1000);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.RequestFrame(2, 2000);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2, frames.back().frame_number);
	EXPECT_TRUE(frames.back().has_overlay);
	EXPECT_TRUE(frames.back().overlay_force_full_upload);
}

TEST(async_video_provider, replacing_subtitles_provider_reuses_video_provider_and_refreshes_overlay_mode) {
	auto state = std::make_shared<VideoProviderState>();
	auto *compat_subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(compat_subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("swap");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(first.source_frame_storage);
	EXPECT_EQ(5, first.source_frame_storage->data[0]);
	ASSERT_TRUE(first.has_subtitle_overlay);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, first.subtitle_overlay.composition_mode);

	auto *overlay_subs = new FakeOverlaySubtitlesProvider;
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(overlay_subs));
	provider.LoadSubtitles(&subtitle_file);

	auto second = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(second.source_frame_storage);
	EXPECT_EQ(5, second.source_frame_storage->data[0]);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_TRUE(second.subtitle_overlay.premultiplied_alpha);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, second.subtitle_overlay.composition_mode);
	EXPECT_EQ(128, second.subtitle_overlay.planes[0].data[3]);
}
