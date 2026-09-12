#include <main.h>

#include "../../src/video_color_pick_preview.h"

#include <libaegisub/dispatch.h>

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

using aegisub::color_pick::Preview;
using aegisub::color_pick::Snapshot;

class ManualExecutor final : public agi::dispatch::Executor {
	std::deque<agi::dispatch::Thunk> jobs;

	void DoPost(agi::dispatch::Thunk thunk) override {
		jobs.push_back(std::move(thunk));
	}

	public:
	[[nodiscard]] size_t Pending() const { return jobs.size(); }

	void RunOne() {
		ASSERT_FALSE(jobs.empty());
		auto job = std::move(jobs.front());
		jobs.pop_front();
		job();
	}
};

std::shared_ptr<VideoFrame> MakeFrame(agi::Color left, agi::Color right) {
	auto frame = std::make_shared<VideoFrame>();
	frame->width = 40;
	frame->height = 12;
	frame->pitch = frame->width * 4;
	frame->flipped = false;
	frame->data.resize(frame->height * frame->pitch);
	for (size_t y = 0; y < frame->height; ++y) {
		for (size_t x = 0; x < frame->width; ++x) {
			auto const color = x < 20 ? left : right;
			auto *pixel = frame->data.data() + y * frame->pitch + x * 4;
			pixel[0] = color.b;
			pixel[1] = color.g;
			pixel[2] = color.r;
			pixel[3] = 255;
		}
	}
	return frame;
}

class video_color_pick_preview : public ::testing::Test {
	protected:
	agi::Color const red{220, 20, 20};
	agi::Color const blue{20, 20, 220};
	ManualExecutor worker;
	ManualExecutor ui;
	std::vector<Snapshot> delivered;
	Preview preview{worker, ui, [this](Snapshot snapshot) {
						delivered.push_back(std::move(snapshot));
					},
					1};
	std::shared_ptr<VideoFrame> frame = MakeFrame(red, blue);

	void CompleteOne() {
		worker.RunOne();
		ui.RunOne();
	}

	void ExpectSnapshot(Snapshot const& snapshot, int x, agi::Color color) {
		EXPECT_EQ(frame, snapshot.frame);
		EXPECT_EQ(x, snapshot.x);
		EXPECT_EQ(6, snapshot.y);
		EXPECT_EQ(color, snapshot.pick.color);
		EXPECT_EQ(240, snapshot.pick.pixels);
		EXPECT_EQ(std::vector<agi::Color>(9, color), snapshot.grid);
	}
};

}

TEST_F(video_color_pick_preview, publishes_matching_grid_and_prediction_only_on_ui_completion) {
	preview.Request(frame, 5, 6);
	EXPECT_EQ(1u, worker.Pending());
	EXPECT_TRUE(delivered.empty());
	worker.RunOne();
	EXPECT_TRUE(delivered.empty());
	ASSERT_EQ(1u, ui.Pending());
	ui.RunOne();
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered.front(), 5, red);
	EXPECT_EQ(0u, worker.Pending());
}

TEST_F(video_color_pick_preview, coalesces_motion_and_keeps_publishing_while_requests_continue) {
	preview.Request(frame, 5, 6);
	for (int i = 0; i < 1000; ++i)
		preview.Request(frame, 6 + i % 28, 6);
	preview.Request(frame, 30, 6);
	EXPECT_EQ(1u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
	CompleteOne();
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered[0], 5, red);
	ASSERT_EQ(1u, worker.Pending());

	preview.Request(frame, 31, 6);
	CompleteOne();
	ASSERT_EQ(2u, delivered.size());
	ExpectSnapshot(delivered[1], 30, blue);
	ASSERT_EQ(1u, worker.Pending());
	CompleteOne();
	ASSERT_EQ(3u, delivered.size());
	ExpectSnapshot(delivered[2], 31, blue);
	EXPECT_EQ(0u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
}

TEST_F(video_color_pick_preview, does_not_repeat_an_identical_or_already_in_flight_input) {
	preview.Request(frame, 5, 6);
	preview.Request(frame, 5, 6);
	preview.Request(frame, 30, 6);
	preview.Request(frame, 5, 6);
	EXPECT_EQ(1u, worker.Pending());
	CompleteOne();
	preview.Request(frame, 5, 6);
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered.front(), 5, red);
	EXPECT_EQ(0u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
}

TEST_F(video_color_pick_preview, discards_an_old_frame_completion_and_computes_the_newest_frame) {
	preview.Request(frame, 5, 6);
	worker.RunOne();
	frame = MakeFrame(blue, red);
	preview.Request(frame, 5, 6);
	EXPECT_EQ(0u, worker.Pending());
	ui.RunOne();
	EXPECT_TRUE(delivered.empty());
	ASSERT_EQ(1u, worker.Pending());
	CompleteOne();
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered.front(), 5, blue);
	EXPECT_EQ(0u, worker.Pending());
}

TEST_F(video_color_pick_preview, clear_drops_pending_motion_and_a_queued_completion) {
	preview.Request(frame, 5, 6);
	preview.Request(frame, 30, 6);
	worker.RunOne();
	preview.Clear();
	ui.RunOne();
	EXPECT_TRUE(delivered.empty());
	EXPECT_EQ(0u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
}

TEST_F(video_color_pick_preview, reentry_after_clear_cannot_publish_the_previous_session) {
	preview.Request(frame, 5, 6);
	worker.RunOne();
	preview.Clear();
	preview.Request(frame, 5, 6);
	EXPECT_EQ(0u, worker.Pending());
	ui.RunOne();
	EXPECT_TRUE(delivered.empty());
	ASSERT_EQ(1u, worker.Pending());
	CompleteOne();
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered.front(), 5, red);
	EXPECT_EQ(0u, worker.Pending());
}

TEST_F(video_color_pick_preview, destruction_drops_work_before_and_after_calculation) {
	for (bool const calculated : {false, true}) {
		SCOPED_TRACE(calculated);
		auto active = std::make_unique<Preview>(worker, ui, [this](Snapshot snapshot) {
			delivered.push_back(std::move(snapshot));
		});
		active->Request(frame, 5, 6);
		if (calculated)
			worker.RunOne();
		active.reset();
		if (calculated)
			ui.RunOne();
		else
			worker.RunOne();
		EXPECT_TRUE(delivered.empty());
		EXPECT_EQ(0u, worker.Pending());
		EXPECT_EQ(0u, ui.Pending());
	}
}

TEST_F(video_color_pick_preview, completion_can_destroy_preview_with_newer_motion_pending) {
	std::unique_ptr<Preview> active;
	active = std::make_unique<Preview>(worker, ui, [&](Snapshot snapshot) {
		delivered.push_back(std::move(snapshot));
		active.reset(); }, 1);
	active->Request(frame, 5, 6);
	active->Request(frame, 30, 6);
	CompleteOne();
	EXPECT_EQ(nullptr, active);
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered.front(), 5, red);
	ASSERT_EQ(1u, worker.Pending());
	worker.RunOne();
	EXPECT_EQ(1u, delivered.size());
	EXPECT_EQ(0u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
}

TEST_F(video_color_pick_preview, callback_can_request_a_new_input_without_duplicate_work) {
	std::unique_ptr<Preview> active;
	active = std::make_unique<Preview>(worker, ui, [&](Snapshot snapshot) {
		delivered.push_back(std::move(snapshot));
		active->Request(frame, 31, 6); }, 1);
	active->Request(frame, 5, 6);
	active->Request(frame, 30, 6);
	CompleteOne();
	ASSERT_EQ(1u, delivered.size());
	ExpectSnapshot(delivered[0], 5, red);
	ASSERT_EQ(1u, worker.Pending());
	CompleteOne();
	ASSERT_EQ(2u, delivered.size());
	ExpectSnapshot(delivered[1], 30, blue);
	ASSERT_EQ(1u, worker.Pending());
	CompleteOne();
	ASSERT_EQ(3u, delivered.size());
	ExpectSnapshot(delivered[2], 31, blue);
	EXPECT_EQ(0u, worker.Pending());
	EXPECT_EQ(0u, ui.Pending());
}
