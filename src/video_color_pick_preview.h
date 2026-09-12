#pragma once

#include "video_color_pick.h"

#include <functional>
#include <memory>

namespace agi::dispatch {
class Executor;
}

namespace aegisub::color_pick {

/// A grid and pick prediction calculated from the same frame and source pixel.
struct Snapshot {
	std::shared_ptr<const VideoFrame> frame;
	int x = 0;
	int y = 0;
	Result pick;
	std::vector<agi::Color> grid;
};

/// Runs one prediction at a time, retaining only the newest requested input.
/// Requests, Clear, destruction and callbacks belong to the UI executor's
/// thread. Executors must outlive queued work; frame pixels must be immutable.
class Preview final {
	public:
	using Callback = std::function<void(Snapshot)>;

	Preview(agi::dispatch::Executor& worker, agi::dispatch::Executor& ui,
			Callback callback, int radius = 8);
	~Preview();

	Preview(Preview const&) = delete;
	Preview& operator=(Preview const&) = delete;
	Preview(Preview&&) = delete;
	Preview& operator=(Preview&&) = delete;

	/// Results from the current frame are delivered even after pointer motion,
	/// so continuous movement cannot starve the preview. Old frames are dropped.
	void Request(std::shared_ptr<const VideoFrame> frame, int x, int y);
	/// Hide/invalidate the preview without waiting for an active calculation.
	void Clear();

	private:
	struct State;
	std::shared_ptr<State> state;
};

}
