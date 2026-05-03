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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_time_projection.h"

#include <array>
#include <string>
#include <vector>

namespace video_subtitle_scene_cache {

struct SubtitleSceneLineSnapshot {
	int layer = 0;
	std::array<int, 3> margin = { { 0, 0, 0 } };
	int start = 0;
	int end = 0;
	std::string style;
	std::string effect;
	std::string text;
};

using SubtitleSceneSnapshot = std::vector<SubtitleSceneLineSnapshot>;

inline bool IsVisualSubtitleCommitType(int type) {
	if (type == AssFile::COMMIT_NEW)
		return true;

	return (type & AssFile::COMMIT_SCRIPTINFO)
		|| (type & AssFile::COMMIT_STYLES)
		|| (type & AssFile::COMMIT_DIAG_ADDREM)
		|| (type & AssFile::COMMIT_DIAG_META)
		|| (type & AssFile::COMMIT_DIAG_TIME)
		|| (type & AssFile::COMMIT_DIAG_TEXT)
		|| (type & AssFile::COMMIT_EXTRADATA);
}

inline SubtitleSceneLineSnapshot MakeSubtitleSceneLineSnapshot(AssDialogueBase const& line) {
	SubtitleSceneLineSnapshot snapshot;
	snapshot.layer = line.Layer;
	snapshot.margin = line.Margin;
	snapshot.start = line.Start;
	snapshot.end = line.End;
	snapshot.style = line.Style.get();
	snapshot.effect = line.Effect.get();
	snapshot.text = line.Text.get();
	return snapshot;
}

inline bool SubtitleSceneLineNeedsAnimatedTimingRefresh(
	SubtitleSceneLineSnapshot const& previous,
	SubtitleSceneLineSnapshot const& current) {
	if (previous.start == current.start && previous.end == current.end)
		return false;
	return !current.effect.empty() || current.text.find('\\') != std::string::npos;
}

template <typename DialogueRange>
SubtitleSceneSnapshot CaptureSubtitleSceneSnapshot(
	DialogueRange const& lines,
	agi::vfr::Framerate const& fps,
	int frame_time_ms) {
	SubtitleSceneSnapshot snapshot;
	for (auto const& line : lines) {
		if (line.Comment)
			continue;
		if (!IsAssDialogueVisibleAtTimeForOutput(line.Start, line.End, frame_time_ms, AssTimeOutputMode::LegacyRounding, &fps))
			continue;
		snapshot.push_back(MakeSubtitleSceneLineSnapshot(line));
	}
	return snapshot;
}

template <typename DialogueRange>
bool CurrentFrameSubtitleSceneNeedsRefresh(
	DialogueRange const& lines,
	agi::vfr::Framerate const& fps,
	int frame_time_ms,
	SubtitleSceneSnapshot const& displayed_snapshot) {
	size_t snapshot_index = 0;
	for (auto const& line : lines) {
		if (line.Comment)
			continue;
		if (!IsAssDialogueVisibleAtTimeForOutput(line.Start, line.End, frame_time_ms, AssTimeOutputMode::LegacyRounding, &fps))
			continue;

		if (snapshot_index >= displayed_snapshot.size())
			return true;

		auto const current = MakeSubtitleSceneLineSnapshot(line);
		auto const& previous = displayed_snapshot[snapshot_index];
		bool const provider_would_refresh =
			previous.layer != current.layer
			|| previous.margin != current.margin
			|| previous.style != current.style
			|| previous.effect != current.effect
			|| previous.text != current.text
			|| SubtitleSceneLineNeedsAnimatedTimingRefresh(previous, current);
		if (provider_would_refresh)
			return true;

		++snapshot_index;
	}

	return snapshot_index != displayed_snapshot.size();
}

template <typename DialogueRange>
bool ShouldWaitForFreshPacket(
	int commit_type,
	bool has_displayed_packet,
	bool has_incremental_changed_line,
	DialogueRange const& lines,
	agi::vfr::Framerate const& fps,
	int frame_time_ms,
	SubtitleSceneSnapshot const& displayed_snapshot) {
	if (!IsVisualSubtitleCommitType(commit_type) || !has_displayed_packet)
		return false;

	bool const scene_needs_refresh =
		CurrentFrameSubtitleSceneNeedsRefresh(lines, fps, frame_time_ms, displayed_snapshot);

	if (has_incremental_changed_line)
		return scene_needs_refresh;

	// Without a single changed dialogue line, treat multi-line dialogue edits
	// using the scene snapshot diff. For style/script info/extradata reloads,
	// we still need to wait when a visible line exists because those commits can
	// affect rendering without changing dialogue fields.
	bool const commit_can_change_visuals_without_dialogue_diff =
		commit_type == AssFile::COMMIT_NEW
		|| (commit_type & (AssFile::COMMIT_SCRIPTINFO | AssFile::COMMIT_STYLES | AssFile::COMMIT_EXTRADATA));
	if (commit_can_change_visuals_without_dialogue_diff) {
		if (scene_needs_refresh)
			return true;
		return !displayed_snapshot.empty();
	}

	return scene_needs_refresh;
}

} // namespace video_subtitle_scene_cache
