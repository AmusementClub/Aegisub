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

#include "export_framerate_transform.h"

#include "ass_time_projection.h"

#include <libaegisub/ass/time.h>

#include <algorithm>

namespace {
// Matches agi::Time::operator int()'s ASS centisecond rounding, but stays as a
// local helper because karaoke values are relative durations rather than
// absolute timestamps.
int round_duration_to_centiseconds(int ms) {
	ms = std::max(0, ms);
	return ((ms + 5) / 10) * 10;
}
}

int TransformFrameBoundaryTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int time_ms,
	agi::vfr::Time boundary) {
	int frame = source.FrameAtTime(time_ms, boundary);
	return destination.TimeAtFrame(frame, boundary);
}

int TransformFrameInstantTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int time_ms) {
	int frame = source.FrameAtTime(time_ms, agi::vfr::EXACT);
	return destination.TimeAtFrame(frame, agi::vfr::EXACT);
}

AssFramerateTransform BuildAssFramerateTransform(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int start_ms,
	int end_ms) {
	AssFramerateTransform result;
	result.new_start_ms = TransformFrameBoundaryTimeForExport(source, destination, start_ms, agi::vfr::START);
	result.new_end_ms = TransformFrameBoundaryTimeForExport(source, destination, end_ms, agi::vfr::END);
	// ASS-relative tags are anchored to the serialized dialogue interval, so
	// reuse the whole-line storage projection rather than projecting endpoints
	// independently.
	auto const old_projected = ProjectAssDialogueTimesForStorage(agi::Time(start_ms), agi::Time(end_ms), &source);
	auto const new_projected = ProjectAssDialogueTimesForStorage(agi::Time(result.new_start_ms), agi::Time(result.new_end_ms), &destination);
	result.old_ass_start_ms = old_projected.first;
	result.old_ass_end_ms = old_projected.second;
	result.new_ass_start_ms = new_projected.first;
	result.new_ass_end_ms = new_projected.second;
	return result;
}

int TransformRelativeStartTagTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int relative_ms) {
	int old_absolute_ms = transform.old_ass_start_ms + relative_ms;
	int new_absolute_ms = TransformFrameInstantTimeForExport(source, destination, old_absolute_ms);
	int value = new_absolute_ms - transform.new_ass_start_ms;
	if (value == 0 && relative_ms != 0)
		value = 1;
	return value;
}

int TransformRelativeEndTagTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int relative_ms) {
	int old_absolute_ms = transform.old_ass_end_ms - relative_ms;
	int new_absolute_ms = TransformFrameInstantTimeForExport(source, destination, old_absolute_ms);
	return transform.new_ass_end_ms - new_absolute_ms;
}

int TransformKaraokeDurationForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int duration_cs,
	int& old_accumulated_cs,
	int& new_accumulated_cs) {
	int old_absolute_end_ms = transform.old_ass_start_ms + (old_accumulated_cs + duration_cs) * 10;
	int new_absolute_end_ms = TransformFrameInstantTimeForExport(source, destination, old_absolute_end_ms);
	int new_boundary_cs = round_duration_to_centiseconds(new_absolute_end_ms - transform.new_ass_start_ms) / 10;
	int value = std::max(0, new_boundary_cs - new_accumulated_cs);
	old_accumulated_cs += duration_cs;
	new_accumulated_cs += value;
	return value;
}
