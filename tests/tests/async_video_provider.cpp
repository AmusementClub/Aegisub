#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/async_video_provider.h"
#include "../../src/export_fixstyle.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/subtitle_overlay_blend.h"
#include "../../src/transient_font_set.h"
#include "../../src/video_render_geometry.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/vfr.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <functional>
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
	int frame_width = 2;
	int frame_height = 2;
	bool bgra_flipped = false;
	std::string color_space = "BT.709";
	std::string real_color_space = "BT.709";
	SourceFrameNativeFormatIdentity native_format = { };
	SourceFrameChromaLocation native_chroma_location = SourceFrameChromaLocation::Unknown;
	SourceFrameGeometry bgra_geometry = { };
	SourceFrameGeometry native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	std::vector<SourceFrameOutputMode> available_modes = { SourceFrameOutputMode::Bgra8 };
	SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;
	std::function<void(int, VideoFrame&)> fill_frame;

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

		frame.width = frame_width;
		frame.height = frame_height;
		frame.pitch = static_cast<size_t>(frame_width) * 4;
		frame.flipped = bgra_flipped;
		frame.data.assign(frame.pitch * frame.height, 0);
		frame.data[0] = static_cast<unsigned char>(n);
		if (fill_frame)
			fill_frame(n, frame);
	}

	void SetColorSpace(std::string const& matrix) override { color_space = matrix; }
	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return frame_width; }
	int GetHeight() const override { return frame_height; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return color_space; }
	std::string GetRealColorSpace() const override { return real_color_space; }
	SourceFrameGeometry GetFrameGeometry() const override {
		if (output_mode == SourceFrameOutputMode::Native)
			return native_geometry;
		if (bgra_geometry.storage_width > 0 && bgra_geometry.storage_height > 0)
			return bgra_geometry;
		return MakeDefaultSourceFrameGeometry(frame_width, frame_height);
	}
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return native_format; }
	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override {
		if (output_mode != SourceFrameOutputMode::Native)
			return false;

		int native_width = frame_width;
		int native_height = frame_height;
		auto format_info = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
		auto storage = std::make_shared<FakeNativeFrameStorage>();
		storage->plane0.resize(static_cast<size_t>(native_width) * native_height, 0);
		storage->plane1.resize(
			static_cast<size_t>(GetSourceFramePlaneWidth(format_info, native_width, 1))
			* GetSourceFramePlaneHeight(format_info, native_height, 1)
			* format_info.planes[1].bytes_per_sample,
			0);
		storage->plane0[0] = static_cast<unsigned char>(n);
		storage->plane1[0] = static_cast<unsigned char>(n + 1);

		frame = { };
		frame.output_mode = SourceFrameOutputMode::Native;
		frame.native_format = native_format.IsValid()
			? native_format
			: SourceFrameNativeFormatIdentity{ SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 7 };
		frame.format_info = format_info;
		frame.width = native_width;
		frame.height = native_height;
		frame.flipped = false;
		frame.plane_count = frame.format_info.plane_count;
		frame.geometry = native_geometry;
		frame.color = SourceFrameColorMetadataFromLegacyColorSpace(color_space);
		frame.chroma_location = native_chroma_location;
		frame.planes[0] = {
			storage->plane0.data(),
			native_width,
			native_width,
			native_height
		};
		frame.planes[1] = {
			storage->plane1.data(),
			GetSourceFramePlaneWidth(format_info, native_width, 1) * format_info.planes[1].bytes_per_sample,
			GetSourceFramePlaneWidth(format_info, native_width, 1),
			GetSourceFramePlaneHeight(format_info, native_height, 1)
		};
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

class FakeGeometryAwareOverlaySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;
	int render_overlay_calls = 0;
	SourceFrameGeometry last_source_geometry = { };
	int last_overlay_target_x = 0;
	int last_overlay_target_y = 0;
	int last_overlay_width = 0;
	int last_overlay_height = 0;

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

	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double) override {
		++render_overlay_calls;
		last_source_geometry = source.geometry;

		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		overlay.canvas_width = source.geometry.storage_width;
		overlay.canvas_height = source.geometry.storage_height;
		overlay.target_x = 1;
		overlay.target_y = 2;
		overlay.width = 4;
		overlay.height = 2;
		last_overlay_target_x = overlay.target_x;
		last_overlay_target_y = overlay.target_y;
		last_overlay_width = overlay.width;
		last_overlay_height = overlay.height;
		overlay.planes[0].data +=
			static_cast<std::ptrdiff_t>(overlay.target_y) * overlay.planes[0].stride +
			static_cast<std::ptrdiff_t>(overlay.target_x) * 4;
		overlay.planes[0].width = overlay.width;
		overlay.planes[0].height = overlay.height;

		for (int y = 0; y < overlay.height; ++y) {
			auto* row = overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride;
			for (int x = 0; x < overlay.width; ++x) {
				auto* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
				pixel[0] = 10;
				pixel[1] = 20;
				pixel[2] = 30;
				pixel[3] = 200;
			}
		}
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

class FakeActivationAwareSubtitlesProvider final : public SubtitlesProvider {
public:
	int activation_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	void OnActivated() override {
		++activation_calls;
	}

	void DrawSubtitles(VideoFrame &, double) override {
	}
};

class FakeActivationOrderSubtitlesProvider final : public SubtitlesProvider {
	int *destruction_count = nullptr;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	int destroyed_before_activation = -1;

	explicit FakeActivationOrderSubtitlesProvider(int *destruction_count)
	: destruction_count(destruction_count) {
	}

	~FakeActivationOrderSubtitlesProvider() override {
		if (destruction_count)
			++*destruction_count;
	}

	void OnActivated() override {
		destroyed_before_activation = destruction_count ? *destruction_count : -1;
	}

	void DrawSubtitles(VideoFrame &, double) override {
	}
};

struct RecordedFrame {
	int frame_number = -1;
	int subtitle_generation = -1;
	double time = 0.0;
	bool has_overlay = false;
	int overlay_dirty_rect_count = 0;
	uint64_t overlay_continuity_generation = 0;
	SourceFrameRect source_visible_rect = { };
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
		frame.overlay_continuity_generation = frame_evt->packet.subtitle_overlay.continuity_generation;
		frame.source_visible_rect = GetSourceFrameVisibleRect(frame_evt->packet.source_frame);

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

std::function<std::unique_ptr<VideoProvider>()> g_video_provider_factory;
std::function<std::unique_ptr<SubtitlesProvider>(SubtitleRenderEnvironment const&)> g_subtitles_provider_factory;
std::shared_ptr<const TransientFontSet> g_last_factory_transient_fonts;
agi::BackgroundRunner *g_last_factory_background_runner = nullptr;

struct ScopedFactoryOverride final {
	~ScopedFactoryOverride() {
		g_video_provider_factory = nullptr;
		g_subtitles_provider_factory = nullptr;
		g_last_factory_transient_fonts.reset();
		g_last_factory_background_runner = nullptr;
	}
};
}

std::vector<std::string> VideoProviderFactory::GetClasses() { return {}; }
std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() { return {}; }
std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *) {
	if (g_video_provider_factory)
		return g_video_provider_factory();
	return nullptr;
}
std::vector<std::string> SubtitlesProviderFactory::GetClasses() { return {}; }
std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(SubtitleRenderEnvironment const& env) {
	g_last_factory_background_runner = env.background_runner;
	g_last_factory_transient_fonts = env.transient_fonts;
	if (g_subtitles_provider_factory)
		return g_subtitles_provider_factory(env);
	return nullptr;
}
void SubtitlesProvider::LoadSubtitles(AssFile *, int) {
	static const char payload[] = "test";
	LoadSubtitles(payload, sizeof(payload) - 1);
}
void AssFixStylesFilter::ProcessSubs(AssFile *) { }

TEST(ass_file_transient_fonts, copy_assignment_and_swap_preserve_shared_state) {
	auto primary_fonts = std::make_shared<TransientFontSet>();
	primary_fonts->generation = 17;
	primary_fonts->fonts.push_back({ "primary.ttf", "font/ttf", { 'a', 'b', 'c' } });

	auto secondary_fonts = std::make_shared<TransientFontSet>();
	secondary_fonts->generation = 23;
	secondary_fonts->fonts.push_back({ "secondary.otf", "font/otf", { 'x', 'y' } });

	AssFile original;
	original.SetTransientFonts(primary_fonts);

	AssFile copied(original);
	ASSERT_TRUE(copied.GetTransientFonts());
	EXPECT_EQ(primary_fonts, copied.GetTransientFonts());
	EXPECT_EQ(17u, copied.GetTransientFonts()->generation);
	ASSERT_EQ(1u, copied.GetTransientFonts()->fonts.size());
	EXPECT_EQ("primary.ttf", copied.GetTransientFonts()->fonts.front().original_name);

	AssFile assigned;
	assigned = original;
	ASSERT_TRUE(assigned.GetTransientFonts());
	EXPECT_EQ(primary_fonts, assigned.GetTransientFonts());

	AssFile other;
	other.SetTransientFonts(secondary_fonts);
	original.swap(other);
	EXPECT_EQ(secondary_fonts, original.GetTransientFonts());
	EXPECT_EQ(primary_fonts, other.GetTransientFonts());
}

TEST(ass_file_transient_fonts, explicit_reset_clears_transient_fonts) {
	auto fonts = std::make_shared<TransientFontSet>();
	fonts->generation = 5;
	fonts->fonts.push_back({ "font.ttf", "font/ttf", { '1' } });

	AssFile file;
	file.SetTransientFonts(fonts);
	ASSERT_EQ(fonts, file.GetTransientFonts());

	file.SetTransientFonts({});
	EXPECT_FALSE(file.GetTransientFonts());
}

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

TEST(async_video_provider, get_frame_bgra_returns_cpu_frame_when_native_mode_selected) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	ASSERT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	ASSERT_EQ(SourceFrameOutputMode::Native, video->output_mode);

	auto frame = provider.GetFrameBgra(7, 7000, true);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 1u);
	EXPECT_EQ(7, frame->data[0]);
	EXPECT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Native, video->output_mode);
}

TEST(async_video_provider, find_key_point_range_scans_frames_inside_worker) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 4;
	video->frame_height = 4;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 4 && n <= 8) {
			set_pixel(1, 1, 40, 80, 120);
			set_pixel(0, 1, 40, 80, 120);
			set_pixel(2, 1, 40, 80, 120);
			set_pixel(1, 0, 40, 80, 120);
			set_pixel(1, 2, 40, 80, 120);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto result = provider.FindKeyPointRange({
		5,
		1,
		1,
		120,
		80,
		40,
		0,
		2,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(4, result.left);
	EXPECT_EQ(8, result.right);
	EXPECT_EQ((std::vector<int>{ 5, 3, 4, 7, 9, 8 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_respects_flipped_frame_coordinates) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->bgra_flipped = true;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 2 && n <= 4) {
			set_pixel(1, 2, 10, 30, 90);
			set_pixel(0, 2, 10, 30, 90);
			set_pixel(2, 2, 10, 30, 90);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto result = provider.FindKeyPointRange({
		3,
		1,
		0,
		90,
		30,
		10,
		0,
		2,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(2, result.left);
	EXPECT_EQ(4, result.right);
}

TEST(async_video_provider, find_key_point_range_refines_coarse_scan_boundaries) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 4;
	video->frame_height = 4;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 4 && n <= 11) {
			set_pixel(2, 1, 12, 64, 128);
			set_pixel(1, 1, 12, 64, 128);
			set_pixel(3, 1, 12, 64, 128);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto result = provider.FindKeyPointRange({
		7,
		2,
		1,
		128,
		64,
		12,
		0,
		4,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(4, result.left);
	EXPECT_EQ(11, result.right);
	EXPECT_EQ((std::vector<int>{ 7, 3, 6, 5, 4, 11, 15, 12 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_refines_to_boundary_when_coarse_scan_has_no_probe) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n <= 6) {
			set_pixel(1, 1, 16, 48, 96);
			set_pixel(0, 1, 16, 48, 96);
			set_pixel(2, 1, 16, 48, 96);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto result = provider.FindKeyPointRange({
		3,
		1,
		1,
		96,
		48,
		16,
		0,
		8,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(0, result.left);
	EXPECT_EQ(6, result.right);
	EXPECT_EQ((std::vector<int>{ 3, 2, 1, 0, 11, 4, 5, 6, 7 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_refines_to_boundary_after_coarse_hit) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n <= 12) {
			set_pixel(1, 1, 24, 72, 144);
			set_pixel(0, 1, 24, 72, 144);
			set_pixel(2, 1, 24, 72, 144);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto result = provider.FindKeyPointRange({
		12,
		1,
		1,
		144,
		72,
		24,
		0,
		8,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(0, result.left);
	EXPECT_EQ(12, result.right);
	EXPECT_EQ((std::vector<int>{ 12, 4, 3, 2, 1, 0, 20, 13 }), state->requested_frames);
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
	EXPECT_GT(packet.subtitle_overlay.continuity_generation, 0u);
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

TEST(async_video_provider, update_subtitles_advances_overlay_continuity_generation_for_direct_overlay) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto initial = MakeSubtitleFile("overlay-1");
	provider.LoadSubtitles(&initial);

	auto first = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(first.has_subtitle_overlay);
	auto const first_generation = first.subtitle_overlay.continuity_generation;

	auto updated = MakeSubtitleFile("overlay-2");
	provider.UpdateSubtitles(&updated, &updated.Events.front());

	auto second = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_GT(second.subtitle_overlay.continuity_generation, first_generation);
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

TEST(async_video_provider, direct_overlay_color_space_override_preserves_overlay_continuity_generation) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(3, 3000);
	ASSERT_TRUE(first.has_subtitle_overlay);
	auto const first_generation = first.subtitle_overlay.continuity_generation;

	provider.SetColorSpace("TV.601");
	auto second = provider.GetRenderPacket(3, 3000);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_EQ(first_generation, second.subtitle_overlay.continuity_generation);
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

TEST(async_video_provider, bgra_source_mode_propagates_provider_geometry_to_overlay_contract) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 5;
	video->frame_height = 6;
	video->bgra_geometry = MakeDefaultSourceFrameGeometry(5, 6);
	video->bgra_geometry.visible_rect = { 1, 2, 3, 2 };
	video->bgra_geometry.pixel_aspect_ratio = 1.25;
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, packet.source_frame.output_mode);
	EXPECT_EQ(1, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(3, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.height);
	EXPECT_DOUBLE_EQ(1.25, packet.source_frame.geometry.pixel_aspect_ratio);
	EXPECT_EQ(1, subs->last_source_geometry.visible_rect.x);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.y);
	EXPECT_EQ(3, subs->last_source_geometry.visible_rect.width);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.height);
	EXPECT_DOUBLE_EQ(1.25, subs->last_source_geometry.pixel_aspect_ratio);
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

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_source_only_display_vflip_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
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
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_rotated_subtitle_overlay_path) {
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
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_display_vflip_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
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
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.has_subtitle_overlay);
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_rotation_plus_display_vflip_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	video->native_geometry.display_vflip = true;
	video->native_geometry.pixel_aspect_ratio = 1.25;
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
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
	EXPECT_DOUBLE_EQ(1.25, packet.source_frame.geometry.pixel_aspect_ratio);
	EXPECT_TRUE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, native_source_mode_propagates_non_full_visible_rect_to_overlay_contract) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 8;
	video->frame_height = 6;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(8, 6);
	video->native_geometry.visible_rect = { 2, 1, 4, 3 };
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(1, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(4, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(3, packet.source_frame.geometry.visible_rect.height);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.x);
	EXPECT_EQ(1, subs->last_source_geometry.visible_rect.y);
	EXPECT_EQ(4, subs->last_source_geometry.visible_rect.width);
	EXPECT_EQ(3, subs->last_source_geometry.visible_rect.height);
	EXPECT_EQ(1, subs->last_overlay_target_x);
	EXPECT_EQ(2, subs->last_overlay_target_y);
	EXPECT_EQ(4, subs->last_overlay_width);
	EXPECT_EQ(2, subs->last_overlay_height);
	EXPECT_EQ(1, packet.subtitle_overlay.target_x);
	EXPECT_EQ(2, packet.subtitle_overlay.target_y);
	EXPECT_EQ(4, packet.subtitle_overlay.width);
	EXPECT_EQ(2, packet.subtitle_overlay.height);
	EXPECT_EQ(8, packet.subtitle_overlay.canvas_width);
	EXPECT_EQ(6, packet.subtitle_overlay.canvas_height);
	ASSERT_EQ(1, packet.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(1, packet.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(4, packet.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].height);

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(
		packet.subtitle_overlay,
		packet.source_frame.geometry);
	EXPECT_EQ(4, adjusted.canvas_width);
	EXPECT_EQ(3, adjusted.canvas_height);
	EXPECT_EQ(0, adjusted.target_x);
	EXPECT_EQ(1, adjusted.target_y);
	EXPECT_EQ(3, adjusted.width);
	EXPECT_EQ(2, adjusted.height);
	EXPECT_EQ(packet.subtitle_overlay.planes[0].data + 4, adjusted.planes[0].data);
	EXPECT_TRUE(adjusted.force_full_upload);
	EXPECT_EQ(nullptr, adjusted.dirty_rects);
	EXPECT_EQ(0, adjusted.dirty_rect_count);
}

TEST(async_video_provider, request_frame_event_preserves_non_full_visible_rect_metadata) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 8;
	video->frame_height = 6;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(8, 6);
	video->native_geometry.visible_rect = { 2, 1, 4, 3 };
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);
	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));

	provider.RequestFrame(5, 5000);
	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_TRUE(frames.back().has_overlay);
	EXPECT_EQ(2, frames.back().source_visible_rect.x);
	EXPECT_EQ(1, frames.back().source_visible_rect.y);
	EXPECT_EQ(4, frames.back().source_visible_rect.width);
	EXPECT_EQ(3, frames.back().source_visible_rect.height);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_display_vflip_source_only_display_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
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

TEST(async_video_provider, dropped_packet_advances_overlay_continuity_generation_on_next_delivered_event) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeDropSensitiveOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto baseline = provider.GetRenderPacket(0, 0);
	ASSERT_TRUE(baseline.has_subtitle_overlay);
	auto const baseline_generation = baseline.subtitle_overlay.continuity_generation;
	state->block_next = true;

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
	EXPECT_GT(frames.back().overlay_continuity_generation, baseline_generation);
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

TEST(async_video_provider, provider_activation_runs_on_initial_create_and_replace) {
	auto state = std::make_shared<VideoProviderState>();
	auto *first_subs = new FakeActivationAwareSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(first_subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_EQ(1, first_subs->activation_calls);

	auto *second_subs = new FakeActivationAwareSubtitlesProvider;
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(second_subs));
	EXPECT_EQ(1, second_subs->activation_calls);
}

TEST(async_video_provider, replacement_activates_new_provider_before_old_is_destroyed) {
	auto state = std::make_shared<VideoProviderState>();
	int destruction_count = 0;
	auto *first_subs = new FakeActivationOrderSubtitlesProvider(&destruction_count);
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(first_subs),
		[&](std::unique_ptr<wxEvent> evt) { recorder(std::move(evt)); });

	EXPECT_EQ(0, first_subs->destroyed_before_activation);
	EXPECT_EQ(0, destruction_count);

	auto *second_subs = new FakeActivationOrderSubtitlesProvider(&destruction_count);
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(second_subs));

	EXPECT_EQ(0, second_subs->destroyed_before_activation);
	EXPECT_EQ(1, destruction_count);
}

TEST(async_video_provider, filename_constructor_forwards_transient_fonts_to_factory) {
	ScopedFactoryOverride scope;
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;

	g_video_provider_factory = [state] {
		return agi::make_unique<FakeVideoProvider>(state);
	};
	g_subtitles_provider_factory = [subs](SubtitleRenderEnvironment const&) {
		return std::unique_ptr<SubtitlesProvider>(subs);
	};

	auto fonts = std::make_shared<TransientFontSet>();
	fonts->generation = 42;
	fonts->fonts.push_back({ "embedded.ttf", "font/ttf", { 'f', 'o', 'n', 't' } });

	wxEvtHandler parent;
	AsyncVideoProvider provider(agi::fs::path("dummy.mkv"), "", &parent, nullptr, fonts);

	ASSERT_TRUE(g_last_factory_transient_fonts);
	EXPECT_EQ(fonts, g_last_factory_transient_fonts);
	EXPECT_EQ(nullptr, g_last_factory_background_runner);
	EXPECT_EQ(42u, g_last_factory_transient_fonts->generation);
	ASSERT_EQ(1u, g_last_factory_transient_fonts->fonts.size());
	EXPECT_EQ("embedded.ttf", g_last_factory_transient_fonts->fonts.front().original_name);

	auto subtitle_file = MakeSubtitleFile("embedded");
	provider.LoadSubtitles(&subtitle_file);
	auto frame = provider.GetFrame(3, 3000);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 2u);
	EXPECT_EQ(3, frame->data[0]);
	EXPECT_EQ(1, frame->data[1]);
}
