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

#include "time_display_mode.h"

#include "ass_time_projection.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace {
constexpr int kMaxInternalTimeMs = 10 * 60 * 60 * 1000 - 6;

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

int normalize_display_duration_ms(agi::Time const& duration, SubtitleTimeDisplayMode mode) {
	if (mode == SubtitleTimeDisplayMode::Ass)
		return std::max(0, static_cast<int>(duration));
	if (mode == SubtitleTimeDisplayMode::Exact)
		return std::max(0, duration.GetMillisecond());
	throw std::logic_error("Frame display mode does not expose millisecond durations");
}
}

SubtitleTimeDisplayMode DefaultTimeDisplayModeForFile(agi::fs::path const& filename) {
	if (filename.empty())
		return SubtitleTimeDisplayMode::Ass;

	auto const extension = lowercase(agi::fs::PathToString(filename.extension()));
	if (extension == ".ass" || extension == ".ssa")
		return SubtitleTimeDisplayMode::Ass;
	return SubtitleTimeDisplayMode::Exact;
}

std::pair<int, int> GetDialogueTimesForDisplay(
	agi::Time const& start,
	agi::Time const& end,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const* fps) {
	if (mode == SubtitleTimeDisplayMode::Ass)
		return ProjectAssDialogueTimesForStorage(start, end, fps);
	if (mode == SubtitleTimeDisplayMode::Exact)
		return { start.GetMillisecond(), end.GetMillisecond() };
	throw std::logic_error("Frame display mode does not expose millisecond timestamps");
}

int GetDurationForDisplay(
	agi::Time const& start,
	agi::Time const& end,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const* fps) {
	auto const displayed = GetDialogueTimesForDisplay(start, end, mode, fps);
	return std::max(0, displayed.second - displayed.first);
}

agi::Time GetEndTimeForDisplayedDuration(
	agi::Time const& start,
	agi::Time const& current_end,
	agi::Time const& displayed_duration,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const* fps) {
	if (mode == SubtitleTimeDisplayMode::Exact)
		return start + displayed_duration;
	if (mode != SubtitleTimeDisplayMode::Ass)
		throw std::logic_error("Frame display mode does not expose millisecond durations");

	int const target_duration_ms = normalize_display_duration_ms(displayed_duration, mode);
	if (GetDurationForDisplay(start, current_end, mode, fps) == target_duration_ms)
		return current_end;
	if (target_duration_ms == 0)
		return start;

	auto const current_displayed = GetDialogueTimesForDisplay(start, current_end, mode, fps);
	int const current_start_ms = current_displayed.first;
	int const target_end_from_current_start = std::clamp(
		current_start_ms + target_duration_ms,
		current_start_ms,
		kMaxInternalTimeMs);

	// If the existing ASS-projected start can still represent the requested
	// duration, keep that displayed anchor and place the internal end directly
	// on the requested ASS end timestamp.
	auto const anchored_to_current = GetDialogueTimesForDisplay(
		start,
		agi::Time(target_end_from_current_start),
		mode,
		fps);
	if (anchored_to_current.first == current_start_ms
		&& anchored_to_current.second == target_end_from_current_start) {
		return agi::Time(target_end_from_current_start);
	}

	// Otherwise the previous displayed start came from a short-interval collapse
	// and cannot be preserved. Fall back to the canonical start projection for
	// this internal start, then place the end on the requested ASS boundary.
	int const canonical_start_ms = ProjectAssTimeForStorage(
		start.GetMillisecond(),
		AssStorageTimeBoundary::Start,
		fps);
	return agi::Time(std::clamp(
		canonical_start_ms + target_duration_ms,
		canonical_start_ms,
		kMaxInternalTimeMs));
}

std::string FormatTimeForDisplay(int ms, SubtitleTimeDisplayMode mode) {
	agi::Time const time(ms);
	if (mode == SubtitleTimeDisplayMode::Ass)
		return time.GetAssFormatted();
	if (mode == SubtitleTimeDisplayMode::Exact)
		return time.GetAssFormatted(true);
	throw std::logic_error("Frame display mode does not format timestamps");
}

std::string FormatTimeForDisplay(agi::Time const& time, SubtitleTimeDisplayMode mode) {
	return FormatTimeForDisplay(time.GetMillisecond(), mode);
}
