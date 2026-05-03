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

#include <string>
#include <utility>

namespace agi {
class Time;
namespace vfr { class Framerate; }
}

class AssDialogue;

enum class AssStorageTimeBoundary {
	Start,
	End
};

enum class AssTimeOutputMode {
	LegacyRounding,
	FrameSafeProjection
};

int ProjectAssTimeForStorage(int time_ms, AssStorageTimeBoundary boundary, agi::vfr::Framerate const* fps = nullptr);
std::pair<int, int> ProjectAssDialogueTimesForStorage(agi::Time const& start, agi::Time const& end, agi::vfr::Framerate const* fps = nullptr);
std::pair<int, int> GetAssDialogueTimesForOutput(
	agi::Time const& start,
	agi::Time const& end,
	AssTimeOutputMode mode,
	agi::vfr::Framerate const* fps = nullptr);
bool IsAssDialogueVisibleAtTimeForOutput(
	agi::Time const& start,
	agi::Time const& end,
	int time_ms,
	AssTimeOutputMode mode,
	agi::vfr::Framerate const* fps = nullptr);
std::string SerializeAssDialogueForOutput(
	AssDialogue const& line,
	AssTimeOutputMode mode,
	agi::vfr::Framerate const* fps = nullptr);
