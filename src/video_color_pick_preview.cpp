#include "video_color_pick_preview.h"

#include <libaegisub/dispatch.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace aegisub::color_pick {

struct Preview::State final : std::enable_shared_from_this<State> {
	struct Input {
		std::shared_ptr<const VideoFrame> frame;
		int x;
		int y;
		uint64_t generation;

		bool operator==(Input const&) const = default;
	};

	agi::dispatch::Executor& worker;
	agi::dispatch::Executor& ui;
	Callback callback;
	int radius;
	uint64_t generation = 0;
	std::optional<Input> latest;
	bool in_flight = false;

	State(agi::dispatch::Executor& worker, agi::dispatch::Executor& ui,
		  Callback callback, int radius)
		: worker(worker), ui(ui), callback(std::move(callback)), radius(radius) {
	}

	void Start(Input const& input) {
		in_flight = true;
		std::weak_ptr<State> weak = shared_from_this();
		try {
			worker.Post([weak, input, radius = radius, ui = &ui] {
				if (weak.expired())
					return;
				Snapshot snapshot{.frame = input.frame, .x = input.x, .y = input.y};
				std::exception_ptr error;
				try {
					snapshot.pick = PickColor(*input.frame, input.x, input.y);
					snapshot.grid = ExtractZoomRegion(*input.frame, input.x, input.y, radius);
				}
				catch (...) {
					error = std::current_exception();
				}
				ui->Post([weak, input, snapshot = std::move(snapshot), error]() mutable {
					if (auto state = weak.lock())
						state->Complete(input, std::move(snapshot), error);
				});
			});
		}
		catch (...) {
			in_flight = false;
			throw;
		}
	}

	void Complete(Input const& input, Snapshot snapshot, std::exception_ptr const& error) {
		in_flight = false;
		bool const current = latest && latest->generation == input.generation &&
							 latest->frame == input.frame;
		// Start the newest input once, even when completion reports an error.
		// The failed input itself is never retried automatically.
		if (callback && latest && *latest != input)
			Start(*latest);
		if (!current || !callback)
			return;
		if (error)
			std::rethrow_exception(error);
		// The callback may destroy Preview; retain this invocation separately
		// from the callback member which destruction clears.
		auto deliver = callback;
		deliver(std::move(snapshot));
	}
};

Preview::Preview(agi::dispatch::Executor& worker, agi::dispatch::Executor& ui,
				 Callback callback, int radius)
	: state(std::make_shared<State>(worker, ui, std::move(callback), radius)) {
}

Preview::~Preview() {
	state->callback = {};
}

void Preview::Request(std::shared_ptr<const VideoFrame> frame, int x, int y) {
	if (!frame) {
		Clear();
		return;
	}
	State::Input input{.frame = std::move(frame), .x = x, .y = y, .generation = state->generation};
	if (state->latest == input)
		return;
	state->latest = std::move(input);
	if (!state->in_flight)
		state->Start(*state->latest);
}

void Preview::Clear() {
	++state->generation;
	state->latest.reset();
}

}
