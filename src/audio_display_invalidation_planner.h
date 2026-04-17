#pragma once

#include <initializer_list>

class AudioDisplayInvalidationPlanner {
public:
	struct Rect {
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;

		constexpr bool IsEmpty() const {
			return width <= 0 || height <= 0;
		}

		friend constexpr bool operator==(Rect const& lhs, Rect const& rhs) {
			return lhs.x == rhs.x
				&& lhs.y == rhs.y
				&& lhs.width == rhs.width
				&& lhs.height == rhs.height;
		}
	};

	static constexpr Rect Union(Rect a, Rect b) {
		if (a.IsEmpty())
			return b;
		if (b.IsEmpty())
			return a;

		int const left = a.x < b.x ? a.x : b.x;
		int const top = a.y < b.y ? a.y : b.y;
		int const right_a = a.x + a.width;
		int const right_b = b.x + b.width;
		int const bottom_a = a.y + a.height;
		int const bottom_b = b.y + b.height;
		int const right = right_a > right_b ? right_a : right_b;
		int const bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
		return { left, top, right - left, bottom - top };
	}

	static constexpr Rect UnionAll(std::initializer_list<Rect> rects) {
		Rect result;
		for (auto const& rect : rects)
			result = Union(result, rect);
		return result;
	}

	static constexpr bool ShouldRefreshTrackCursor(int old_pos, int new_pos) {
		if (old_pos == new_pos)
			return false;
		return old_pos >= 0 || new_pos >= 0;
	}

	static constexpr Rect TrackCursorLineRect(int pos, int scroll_left, int audio_top, int audio_height) {
		if (pos < 0 || audio_height <= 0)
			return { };
		return { pos - scroll_left - 1, audio_top, 3, audio_height };
	}

	static constexpr Rect PlanTrackCursorDirtyRect(
		int old_pos,
		int new_pos,
		Rect old_label_rect,
		Rect new_label_rect,
		int scroll_left,
		int audio_top,
		int audio_height) {
		return UnionAll({
			TrackCursorLineRect(old_pos, scroll_left, audio_top, audio_height),
			TrackCursorLineRect(new_pos, scroll_left, audio_top, audio_height),
			old_label_rect,
			new_label_rect,
		});
	}

	static constexpr Rect PlanMarkerMoveDirtyRect(Rect old_marker_rect, Rect new_marker_rect) {
		return Union(old_marker_rect, new_marker_rect);
	}

	static constexpr Rect PlanSelectionEdgeDirtyRect(int old_pos, int new_pos, int scroll_left, int audio_top, int audio_height) {
		return Union(
			TrackCursorLineRect(old_pos, scroll_left, audio_top, audio_height),
			TrackCursorLineRect(new_pos, scroll_left, audio_top, audio_height));
	}
};
