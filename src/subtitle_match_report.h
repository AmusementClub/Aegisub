/// @file subtitle_match_report.h
/// @brief Enumerate find/replace matches and report where they were.
///
/// Split out of the find/replace engine so that "which places matched" is a
/// pure query over an event list, independent of the GUI and of whether the
/// caller intends to actually replace anything. The engine's ReplaceAll and
/// the results panel both go through here, so a replace report and a
/// find-all listing are guaranteed to agree on what counts as a match.

#pragma once

#include "ass_file.h"
#include "subtitle_matcher.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AssDialogue;

namespace aegisub::subtitle_match_report {

/// One match, located in the field text as it stood before any replacement.
struct MatchHit {
	/// AssDialogue::Id of the line that matched. Prefer this over a raw pointer:
	/// undo/redo restores lines via AssDialogue(AssDialogueBase), which keeps the
	/// Id from the snapshot (ass_dialogue.cpp AssDialogueBase ctor). The
	/// AssDialogue(AssDialogue) copy ctor remints Ids and is not used by undo.
	/// File load/close replace the event list with new objects, so old Ids then
	/// fail GetDialogueById and any open report must be discarded.
	int line_id = 0;
	/// Row number at enumeration time, for display only.
	int row = -1;
	/// Start time and style captured at enumeration so a results list can
	/// render without O(hits × events) Id lookups.
	std::string start_time;
	std::string style;
	/// Which dialogue field `start`/`end`/`line_text` refer to. Needed so a
	/// results UI can jump without painting Style/Actor/Effect ranges into the
	/// text edit box, and so undo reconcile compares the correct field.
	SearchReplaceSettings::Field field = SearchReplaceSettings::Field::TEXT;
	/// Byte range of the match within the original field text.
	std::size_t start = 0;
	std::size_t end = 0;
	/// The text that matched.
	std::string matched;
	/// Full original field text, shared by every hit from the same source line.
	std::shared_ptr<std::string const> line_text;
};

/// One applied replacement. The pre-replacement range is inherited from
/// MatchHit; `new_start`/`new_end` locate the same match in the final text.
struct ReplacementHit : MatchHit {
	std::string replacement;
	std::size_t new_start = 0;
	std::size_t new_end = 0;
};

/// A whole-line enumerator built once and reused across lines. For a regex
/// search, constructing one compiles the pattern.
using MatchEnumerator = SubtitleMatchEnumerator;

/// Whether a line is eligible under the settings' comment and style filters.
/// Exposed so callers that walk events themselves stay consistent with these.
bool LineIsEligible(AssDialogue const& line, SearchReplaceSettings const& settings);

/// Advance past a match that consumed nothing. Kept as a small UTF-8 cursor
/// utility for callers which need to step one whole character.
/// Steps one whole UTF-8 character to avoid landing mid-sequence. Returns a
/// position past the end of `text` once the text is exhausted.
std::size_t AdvancePastEmptyMatch(std::string const& text, std::size_t pos);

/// Enumerate every match in `line` without modifying it. `enumerate` must have
/// been built from `settings`.
void FindInLine(AssDialogue const& line, SearchReplaceSettings const& settings,
	             MatchEnumerator& enumerate, std::vector<MatchHit>& out);

/// Enumerate every match across `events`, honoring the comment and style
/// filters. When settings.limit_to is SELECTED, only lines present in
/// `selection` are considered (an empty selection yields no hits).
std::vector<MatchHit> FindAll(EntryList<AssDialogue> const& events,
                              SearchReplaceSettings const& settings,
                              std::vector<AssDialogue const *> const& selection);

/// Replace every match in `line`, writing the field back, and report each
/// replacement. `enumerate` must have been built from `settings`. Returns the
/// number of replacements appended to `out`.
std::size_t ReplaceInLine(AssDialogue& line, SearchReplaceSettings const& settings,
	                      MatchEnumerator& enumerate, std::vector<ReplacementHit>& out);

/// Full field text as it would read if only this hit's replacement were
/// applied: `original[:start] + replacement + original[end:]`. Each hit stands
/// on its own (matches the existing per-hit Replacement column) rather than
/// reflecting the combined result of Replace All.
std::string ReplacedLineText(ReplacementHit const& hit);

}
