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

#pragma once

#include <libaegisub/vfr.h>

struct AssFramerateTransform {
	int new_start_ms = 0;
	int new_end_ms = 0;
	int old_ass_start_ms = 0;
	int old_ass_end_ms = 0;
	int new_ass_start_ms = 0;
	int new_ass_end_ms = 0;
};

int TransformFrameBoundaryTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int time_ms,
	agi::vfr::Time boundary);

int TransformFrameInstantTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int time_ms);

AssFramerateTransform BuildAssFramerateTransform(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	int start_ms,
	int end_ms);

int TransformRelativeStartTagTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int relative_ms);

int TransformRelativeEndTagTimeForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int relative_ms);

int TransformKaraokeDurationForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int duration_cs,
	int& old_accumulated_cs,
	int& new_accumulated_cs);

int TransformKaraokeStartForExport(
	agi::vfr::Framerate const& source,
	agi::vfr::Framerate const& destination,
	AssFramerateTransform const& transform,
	int start_cs,
	int& old_accumulated_cs,
	int& new_accumulated_cs);
