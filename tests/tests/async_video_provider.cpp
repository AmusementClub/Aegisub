#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/async_video_provider.h"
#include "../../src/export_fixstyle.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/vfr.h>

#include <chrono>
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

class FakeVideoProvider final : public VideoProvider {
	std::shared_ptr<VideoProviderState> state;

public:
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
		frame.data = {
			static_cast<unsigned char>(n),
			0,
			0,
			0,
			0,
			0,
			0,
			0
		};
	}

	void SetColorSpace(std::string const&) override { }
	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
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

struct RecordedFrame {
	int frame_number = -1;
	int subtitle_generation = -1;
	double time = 0.0;
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
		RecordedFrame frame;
		frame.frame_number = frame_evt->frame && !frame_evt->frame->data.empty() ? frame_evt->frame->data[0] : -1;
		frame.subtitle_generation = frame_evt->frame && frame_evt->frame->data.size() > 1 ? frame_evt->frame->data[1] : -1;
		frame.time = frame_evt->time;

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
