// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "ass_time_projection.h"

#include "ass_dialogue.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <limits>

namespace {
constexpr int kAssMaxMs = 10 * 60 * 60 * 1000 - 10;
constexpr int kAssInternalMaxMs = 10 * 60 * 60 * 1000 - 6;

int clamp_ass_internal_ms(int ms) {
	return std::clamp(ms, 0, kAssInternalMaxMs);
}

int floor_cs(int ms) {
	ms = clamp_ass_internal_ms(ms);
	return std::clamp(ms / 10 * 10, 0, kAssMaxMs);
}

int ceil_cs(int ms) {
	ms = clamp_ass_internal_ms(ms);
	if (ms <= 0)
		return 0;
	if (ms >= kAssMaxMs)
		return kAssMaxMs;
	return std::clamp(((ms + 9) / 10) * 10, 0, kAssMaxMs);
}

/// Symmetric round to centisecond grid — identical to agi::Time::operator int().
/// Always returns the same value for the same input regardless of boundary role,
/// eliminating the display inconsistency where Start and End show different ASS
/// timestamps for the same internal millisecond value.
int round_cs(int ms) {
	ms = clamp_ass_internal_ms(ms);
	return std::clamp((ms + 5) / 10 * 10, 0, kAssMaxMs);
}

agi::vfr::Time to_vfr_mode(AssStorageTimeBoundary boundary) {
	return boundary == AssStorageTimeBoundary::Start ? agi::vfr::START : agi::vfr::END;
}

bool candidate_preserves_frame(int candidate_ms, int original_ms, AssStorageTimeBoundary boundary, agi::vfr::Framerate const& fps) {
	auto const mode = to_vfr_mode(boundary);
	return fps.FrameAtTime(candidate_ms, mode) == fps.FrameAtTime(original_ms, mode);
}

int prefer_frame_safe_candidate(
	int original_ms,
	AssStorageTimeBoundary boundary,
	agi::vfr::Framerate const& fps) {
	std::array<int, 2> candidates = { floor_cs(original_ms), ceil_cs(original_ms) };
	std::array<bool, 2> valid = {
		candidate_preserves_frame(candidates[0], original_ms, boundary, fps),
		candidate_preserves_frame(candidates[1], original_ms, boundary, fps)
	};

	if (!valid[0] && !valid[1])
		return std::numeric_limits<int>::min();
	if (valid[0] && !valid[1])
		return candidates[0];
	if (!valid[0] && valid[1])
		return candidates[1];

	// Both candidates preserve frame identity. Use symmetric rounding
	// (same as agi::Time::operator int()) so that the same internal
	// millisecond value always produces the same ASS timestamp regardless
	// of whether it appears as a Start or End boundary.
	return round_cs(original_ms);
}

int project_short_interval_to_single_ass_bucket(int start_ms, int end_ms) {
	// When start/end projection collapses a positive interval, the source span
	// is smaller than ASS can represent. Pick the single centisecond bucket
	// containing the interval midpoint rather than expanding to the full hull.
	int midpoint_ms = start_ms + (end_ms - start_ms) / 2;
	int projected_start = floor_cs(midpoint_ms);
	if (projected_start >= kAssMaxMs)
		projected_start = std::max(0, kAssMaxMs - 10);
	return projected_start;
}
}

int ProjectAssTimeForStorage(int time_ms, AssStorageTimeBoundary boundary, agi::vfr::Framerate const* fps) {
	time_ms = clamp_ass_internal_ms(time_ms);

	if (fps && fps->IsLoaded()) {
		// If either neighboring ASS timestamp preserves the same START/END frame,
		// prefer that over the generic conservative ceil/floor rule.
		int const projected = prefer_frame_safe_candidate(time_ms, boundary, *fps);
		if (projected != std::numeric_limits<int>::min())
			return projected;
	}

	// No framerate available or neither candidate preserves frame identity.
	// Use symmetric rounding for display consistency.
	return round_cs(time_ms);
}

std::pair<int, int> ProjectAssDialogueTimesForStorage(agi::Time const& start, agi::Time const& end, agi::vfr::Framerate const* fps) {
	int const start_ms = start.GetMillisecond();
	int const end_ms = end.GetMillisecond();

	int projected_start = ProjectAssTimeForStorage(start_ms, AssStorageTimeBoundary::Start, fps);
	int projected_end = ProjectAssTimeForStorage(end_ms, AssStorageTimeBoundary::End, fps);

	if (end_ms > start_ms && projected_end <= projected_start) {
		projected_start = project_short_interval_to_single_ass_bucket(start_ms, end_ms);
		projected_end = std::min(kAssMaxMs, projected_start + 10);
	}

	if (projected_end < projected_start)
		projected_end = projected_start;

	return { projected_start, projected_end };
}

bool IsAssDialogueVisibleAtTimeForStorage(agi::Time const& start, agi::Time const& end, int time_ms, agi::vfr::Framerate const* fps) {
	auto const projected = ProjectAssDialogueTimesForStorage(start, end, fps);
	return !(projected.first > time_ms || projected.second <= time_ms);
}

std::string SerializeAssDialogueForStorage(AssDialogue const& line, agi::vfr::Framerate const* fps) {
	auto const projected = ProjectAssDialogueTimesForStorage(line.Start, line.End, fps);
	return line.GetEntryData(
		agi::Time(projected.first).GetAssFormatted(),
		agi::Time(projected.second).GetAssFormatted());
}
