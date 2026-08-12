#pragma once

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegisub::visual_frame_visibility {

template<class Line>
struct Interval {
	int first_frame = 0;
	int last_frame = -1;
	Line *line = nullptr;
};

template<class Line>
class Index {
	struct Boundary {
		int frame = 0;
		Line *line = nullptr;
	};

	std::vector<Boundary> starts;
	std::vector<Boundary> ends;
	std::unordered_map<Line *, std::pair<int, int>> interval_by_line;
	std::set<Line *> visible;

	static bool BoundaryLess(Boundary const& left, Boundary const& right) noexcept {
		return left.frame < right.frame;
	}

public:
	void Rebuild(std::vector<Interval<Line>> const& intervals, int frame) {
		starts.clear();
		ends.clear();
		interval_by_line.clear();
		visible.clear();
		starts.reserve(intervals.size());
		ends.reserve(intervals.size());

		for (auto const& interval : intervals) {
			if (!interval.line || interval.last_frame < interval.first_frame)
				continue;
			starts.push_back({interval.first_frame, interval.line});
			ends.push_back({interval.last_frame, interval.line});
			interval_by_line.emplace(interval.line, std::make_pair(interval.first_frame, interval.last_frame));
			if (interval.first_frame <= frame && frame <= interval.last_frame)
				visible.insert(interval.line);
		}

		std::stable_sort(starts.begin(), starts.end(), BoundaryLess);
		std::stable_sort(ends.begin(), ends.end(), BoundaryLess);
	}

	std::set<Line *> const& Visible() const noexcept { return visible; }

	void Advance(int old_frame, int new_frame,
		std::vector<Line *>& entered,
		std::vector<Line *>& exited)
	{
		entered.clear();
		exited.clear();
		if (new_frame == old_frame)
			return;

		std::unordered_set<Line *> candidates;
		auto append_candidates = [&](std::vector<Boundary> const& boundaries, int first, int last) {
			auto begin = std::upper_bound(boundaries.begin(), boundaries.end(), first,
				[](int frame, Boundary const& boundary) { return frame < boundary.frame; });
			auto end = std::upper_bound(boundaries.begin(), boundaries.end(), last,
				[](int frame, Boundary const& boundary) { return frame < boundary.frame; });
			for (; begin != end; ++begin)
				candidates.insert(begin->line);
		};

		if (old_frame < new_frame) {
			append_candidates(starts, old_frame, new_frame);
			// A line is visible on its last frame and exits on the next one.
			append_candidates(ends, old_frame - 1, new_frame - 1);
		}
		else {
			// Moving backwards enters lines whose end was crossed and exits lines
			// whose first visible frame was crossed.
			append_candidates(ends, new_frame - 1, old_frame - 1);
			append_candidates(starts, new_frame, old_frame);
		}

		for (auto *line : candidates) {
			bool const was_visible = visible.count(line) != 0;
			auto const interval = interval_by_line.find(line);
			bool const visible_at_destination = interval != interval_by_line.end()
				&& interval->second.first <= new_frame
				&& new_frame <= interval->second.second;
			if (visible_at_destination && !was_visible) {
				visible.insert(line);
				entered.push_back(line);
			}
			else if (!visible_at_destination && was_visible) {
				visible.erase(line);
				exited.push_back(line);
			}
		}
	}
};

}
