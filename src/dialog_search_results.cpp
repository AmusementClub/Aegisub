/// @file dialog_search_results.cpp
/// @see dialog_search_results.h

#include "dialog_search_results.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "utils.h"

#include <libaegisub/exception.h>

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <unordered_map>

namespace {
std::unordered_map<agi::Context *, DialogSearchResults *> search_results_dialogs;

void close_existing_results_dialog(agi::Context *context) {
	auto it = search_results_dialogs.find(context);
	if (it == search_results_dialogs.end())
		return;
	auto *dlg = it->second;
	search_results_dialogs.erase(it);
	dlg->Destroy();
}

std::string const& dialogue_field_text(AssDialogue const& line,
                                       SearchReplaceSettings::Field field) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return line.Text.get();
		case SearchReplaceSettings::Field::STYLE: return line.Style.get();
		case SearchReplaceSettings::Field::ACTOR: return line.Actor.get();
		case SearchReplaceSettings::Field::EFFECT: return line.Effect.get();
	}
	throw agi::InternalError("Bad field for search results");
}
}

void DialogSearchResults::InitCommon(bool replace_mode) {
	has_replacement = replace_mode;
	replacements_applied = replace_mode;

	hit_indices_by_line.clear();
	hit_indices_by_line.reserve(hits.size());
	for (std::size_t i = 0; i < hits.size(); ++i)
		hit_indices_by_line[hits[i].line_id].push_back(i);

	list = new wxListView(this, -1, wxDefaultPosition, FromDIP(wxSize(860, 280)),
	                      wxLC_REPORT);

	// Column order: Line/Start/Style are quick orientation; Match|Replacement
	// pair the before/after fragments; Context|Replaced line pair the full
	// before/after line so a diff scan stays eye-adjacent.
	list->InsertColumn(0, _("Line"), wxLIST_FORMAT_RIGHT, FromDIP(50));
	list->InsertColumn(1, _("Start"), wxLIST_FORMAT_LEFT, FromDIP(90));
	list->InsertColumn(2, _("Style"), wxLIST_FORMAT_LEFT, FromDIP(100));
	list->InsertColumn(3, _("Match"), wxLIST_FORMAT_LEFT, FromDIP(120));
	if (has_replacement)
		list->InsertColumn(4, _("Replacement"), wxLIST_FORMAT_LEFT, FromDIP(120));
	list->InsertColumn(has_replacement ? 5 : 4, _("Context"),
	                   wxLIST_FORMAT_LEFT, FromDIP(240));
	if (has_replacement)
		list->InsertColumn(6, _("Replaced line"), wxLIST_FORMAT_LEFT, FromDIP(240));

	copy_button = new wxButton(this, -1, _("&Copy selected lines"));
	auto close_button = new wxButton(this, wxID_CLOSE);

	status = new wxStaticText(this, -1, wxEmptyString);

	auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(copy_button, wxSizerFlags().Border(wxRIGHT));
	button_sizer->AddStretchSpacer(1);
	button_sizer->Add(close_button);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(list, wxSizerFlags(1).Expand().Border());
	main_sizer->Add(status, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	SetSizerAndFit(main_sizer);
	CenterOnParent();

	list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &DialogSearchResults::OnActivate, this);
	copy_button->Bind(wxEVT_BUTTON, &DialogSearchResults::OnCopySelected, this);
	close_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

	// Modeless wxDialog defaults to Hide() on close; that would leave the
	// object and its AssFile commit listener alive forever. Match DialogManager:
	// Skip the event, then Destroy.
	Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& evt) {
		auto *dlg = static_cast<DialogSearchResults *>(evt.GetEventObject());
		evt.Skip();
		dlg->Destroy();
	});

	// Details give changed_lines so unrelated single-line edits do not wipe a
	// large find-all report.
	file_changed_slot = c->GetCore().ass->AddCommitDetailsListener(
		&DialogSearchResults::OnCommit, this);

	// Build the matcher so a later RecomputeChangedLine can re-match an edited
	// row in place. Find reports only; replace reports grey changed rows
	// without re-running substitution.
	RebuildEnumerator();

	PopulateList();

	// Autosize text-heavy columns to their content so the new "Replaced line"
	// column is visible without horizontal scrolling. Fixed widths remain for
	// Line/Start/Style (short, bounded). Take the larger of content/header so a
	// long header label never truncates.
	auto fit = [&](int col) {
		list->SetColumnWidth(col, wxLIST_AUTOSIZE);
		int const content = list->GetColumnWidth(col);
		list->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
		int const header = list->GetColumnWidth(col);
		if (content > header)
			list->SetColumnWidth(col, content);
	};
	fit(3); // Match
	if (has_replacement) fit(4); // Replacement
	fit(has_replacement ? 5 : 4); // Context
	if (has_replacement) fit(6); // Replaced line
}

void DialogSearchResults::RebuildEnumerator() {
	// Replace reports never recompute (they grey changed rows), so they do not
	// need a matcher.
	if (is_replace_report)
		return;
	// Build from a copy with replace_with cleared. The panel's recompute path
	// only needs match offsets, never replacement text; clearing it makes
	// set_regex_replacements short-circuit, avoiding two UTF-32 format
	// expansions per match per keystroke. Find All's replace_with is whatever
	// the MRU last held (even for a find-only dialog), so it is frequently
	// non-empty even though the panel never reads it.
	SearchReplaceSettings find_only = settings;
	find_only.replace_with.clear();
	try {
		enumerator = MakeSubtitleMatchEnumerator(find_only);
	}
	catch (...) {
		// Bad regex should not reach here (Find All would have failed first),
		// but if it does, leave the panel without a matcher: RecomputeChangedLine
		// falls back to greying changed rows.
		enumerator = nullptr;
	}
}

DialogSearchResults::DialogSearchResults(agi::Context *context, SearchReplaceSettings settings,
                                         std::vector<aegisub::subtitle_match_report::MatchHit> matches)
: wxDialog(context->GetUI().parent, -1, _("Search results"),
           wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(context)
, settings(std::move(settings))
{
	hits.reserve(matches.size());
	for (auto& hit : matches) {
		DisplayHit d;
		d.line_id = hit.line_id;
		d.row = hit.row;
		d.start_time = std::move(hit.start_time);
		d.style = std::move(hit.style);
		d.matched = std::move(hit.matched);
		d.line_text = hit.line_text;
		// Freeze the report-time text so an undo that restores it is detectable
		// after edits re-pointed line_text (see TryReconcileAfterDocumentRebuild).
		d.original_line_text = hit.line_text;
		d.field = hit.field;
		d.match_start = hit.start;
		d.match_end = hit.end;
		// Freeze report-time offsets: RecomputeChangedLine rewrites match_start
		// to the edited match's offsets, but reconcile must restore from the
		// originals (which index into original_line_text, also frozen).
		d.original_match_start = hit.start;
		d.original_match_end = hit.end;
		d.jump_start = hit.start;
		d.jump_end = hit.end;
		hits.push_back(std::move(d));
	}
	InitCommon(false);
}

DialogSearchResults::DialogSearchResults(agi::Context *context, SearchReplaceSettings settings,
                                         std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements)
: wxDialog(context->GetUI().parent, -1, _("Replace results"),
           wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(context)
, is_replace_report(true)
, settings(std::move(settings))
{
	hits.reserve(replacements.size());
	for (auto& hit : replacements) {
		DisplayHit d;
		d.line_id = hit.line_id;
		d.row = hit.row;
		d.start_time = std::move(hit.start_time);
		d.style = std::move(hit.style);
		d.matched = std::move(hit.matched);
		// Compute replaced_line before move()-ing line_text/replacement out of
		// `hit` — ReplacedLineText reads both.
		d.replaced_line = aegisub::subtitle_match_report::ReplacedLineText(hit);
		d.line_text = hit.line_text;
		// Pre-replacement text is what an undo would restore, so it is the
		// "original" this report reconciles against.
		d.original_line_text = hit.line_text;
		d.replacement = std::move(hit.replacement);
		d.field = hit.field;
		d.match_start = hit.start;
		d.match_end = hit.end;
		// Pre-replacement offsets index into original_line_text (the pre-edit
		// field text). Freeze them so a post-edit reconcile can restore.
		d.original_match_start = hit.start;
		d.original_match_end = hit.end;
		// Text has already been rewritten; jump into the post-edit range until
		// an undo restores the pre-replacement field (see TryReconcile...).
		d.jump_start = hit.new_start;
		d.jump_end = hit.new_end;
		hits.push_back(std::move(d));
	}
	InitCommon(true);
}

DialogSearchResults::~DialogSearchResults() {
	auto it = search_results_dialogs.find(c);
	if (it != search_results_dialogs.end() && it->second == this)
		search_results_dialogs.erase(it);
}

void DialogSearchResults::PopulateList() {
	list->DeleteAllItems();
	list_row_by_hit.assign(hits.size(), -1);

	// System gray text for rows whose match no longer exists on the live line.
	// Cached once so PopulateList does not hit wxSystemSettings per row.
	wxColour const grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

	for (std::size_t i = 0; i < hits.size(); ++i) {
		auto const& hit = hits[i];
		long idx = list->InsertItem(static_cast<long>(i), fmt_wx("%d", hit.row + 1));
		list->SetItem(idx, 1, to_wx(hit.start_time));
		list->SetItem(idx, 2, to_wx(hit.style));
		list->SetItem(idx, 3, to_wx(hit.matched));
		if (has_replacement)
			list->SetItem(idx, 4, to_wx(hit.replacement));
		list->SetItem(idx, has_replacement ? 5 : 4, to_wx(*hit.line_text));
		if (has_replacement)
			list->SetItem(idx, 6, to_wx(hit.replaced_line));
		list->SetItemData(idx, static_cast<long>(i));
		list_row_by_hit[i] = idx;
		if (hit.per_hit_stale)
			list->SetItemTextColour(idx, grey);
	}

	UpdateStatusBar();
}

void DialogSearchResults::ReindexListRows() {
	list_row_by_hit.assign(hits.size(), -1);
	long const count = list->GetItemCount();
	for (long row = 0; row < count; ++row) {
		auto const hit_index = static_cast<std::size_t>(list->GetItemData(row));
		if (hit_index < list_row_by_hit.size())
			list_row_by_hit[hit_index] = row;
	}
}

void DialogSearchResults::UpdateListRow(std::size_t hit_index,
                                        wxColour const& grey, wxColour const& normal) {
	// In-place refresh of one row's text + colour, preserving selection and
	// scroll position. Avoids DeleteAllItems/InsertItem churn (each is several
	// SendMessage calls on Win32) on every keystroke for large reports.
	if (hit_index >= hits.size())
		return;
	// Use the cached hit-to-row mapping so updating k matches on one edited line
	// is O(k), not O(k * rows). Validate the cached entry to remain correct if a
	// future column-sort handler reorders the control without updating the map;
	// one O(rows) repair then serves all subsequent updates.
	long row = hit_index < list_row_by_hit.size() ? list_row_by_hit[hit_index] : -1;
	if (row < 0 || row >= list->GetItemCount() ||
	    static_cast<std::size_t>(list->GetItemData(row)) != hit_index) {
		ReindexListRows();
		row = hit_index < list_row_by_hit.size() ? list_row_by_hit[hit_index] : -1;
	}
	if (row < 0)
		return;
	auto const& hit = hits[hit_index];
	list->SetItem(row, 0, fmt_wx("%d", hit.row + 1));
	list->SetItem(row, 1, to_wx(hit.start_time));
	list->SetItem(row, 2, to_wx(hit.style));
	list->SetItem(row, 3, to_wx(hit.matched));
	if (has_replacement)
		list->SetItem(row, 4, to_wx(hit.replacement));
	list->SetItem(row, has_replacement ? 5 : 4, to_wx(*hit.line_text));
	if (has_replacement)
		list->SetItem(row, 6, to_wx(hit.replaced_line));

	// Colours are queried once per OnCommit (see caller) and passed in, so a
	// multi-row recompute does not issue a wxSystemSettings + LVM_GETTEXTCOLOR
	// SendMessage per row. `normal` is list->GetTextColour(): writing it back
	// explicitly is required because wxNullColour is a no-op under wxMSW's
	// wxItemAttr::AssignFrom (HasTextColour() is false), so a row greyed by a
	// prior recompute would stay grey after its match comes back.
	list->SetItemTextColour(row, hit.per_hit_stale ? grey : normal);
}

void DialogSearchResults::UpdateStatusBar() {
	if (stale) {
		// MarkStale owns the stale message; do not overwrite it here.
		return;
	}

	// Count usable vs greyed rows so Find reports do not claim matches that no
	// longer resolve, and both report shapes surface greyed historical rows.
	int usable = 0;
	int greyed = 0;
	for (auto const& hit : hits) {
		if (hit.per_hit_stale) ++greyed;
		else ++usable;
	}

	// The two report shapes state different things:
	//  - Find: "N matches found" describes the currently-valid matches, so it
	//    uses `usable` (greyed rows no longer match).
	//  - Replace: "N matches were replaced" states a historical fact — the
	//    replacements happened and have not been undone (replacements_applied
	//    is still true). It must not shrink to 0 when the user later edits a
	//    replaced row; that would read as "the replacement was rolled back",
	//    which it was not. So it uses the total hit count.
	int const total = static_cast<int>(hits.size());
	// Replace reports never re-match a row after it is edited, so per-hit stale
	// means only that the historical replacement row was re-edited. Do not use
	// that flag to reduce a purported live-match count after Replace All is
	// undone; the panel has no evidence that those matches disappeared.
	int const found_count = is_replace_report ? total : usable;
	wxString base = replacements_applied
		? fmt_plural(total, "One match was replaced.", "%d matches were replaced.", total)
		: fmt_plural(found_count, "One match found.", "%d matches found.", found_count);

	if (greyed > 0) {
		// The suffix meaning differs by report shape too: a greyed Find row no
		// longer matches; a greyed Replace row was on a line edited again after
		// the replacement, so the per-hit replacement view is no longer live.
		base += is_replace_report
			? fmt_plural(greyed, "  (one row re-edited)", "  (%d rows re-edited)", greyed)
			: fmt_plural(greyed, "  (one no longer matches)", "  (%d no longer match)", greyed);
	}
	status->SetLabel(base);
}

void DialogSearchResults::MarkStale() {
	if (stale)
		return;
	stale = true;
	// Keep the list enabled so the user can still scroll through a large
	// outdated report; JumpToHit refuses navigation while stale.
	copy_button->Enable(false);
	// ASCII only inside _(): Aegisub is built with wxNO_IMPLICIT_WXSTRING_ENCODING,
	// so non-ASCII bytes in _("...") go through FromAscii and assert in debug.
	status->SetLabel(_("Results are out of date. Re-run Find All or Replace All."));
}

bool DialogSearchResults::TryReconcileAfterDocumentRebuild() {
	auto core = c->GetCore();

	// This runs inside the commit listener. dialogue_field_text can in principle
	// throw agi::InternalError on a bad field enum; that is unreachable (fields
	// come from a fixed enum), but wrapping the whole reconcile keeps "the
	// listener never escapes" as a uniform invariant rather than relying on
	// per-call reasoning. On any throw, bail to MarkStale via the caller.
	try {
		// Resolve each report line before mutating any hit. New-file/load commits
		// remint IDs and therefore fail cleanly; undo/redo preserve them.
		std::vector<AssDialogue *> resolved_lines;
		resolved_lines.reserve(hit_indices_by_line.size());
		for (auto const& entry : hit_indices_by_line) {
			int const line_id = entry.first;
			AssDialogue *line = core.selectionController->GetDialogueById(line_id);
			if (!line)
				return false;
			resolved_lines.push_back(line);
		}

		if (!is_replace_report) {
			// A Find report can reconcile any undo/redo snapshot by applying the
			// same per-line rematch used for ordinary edits. This keeps redo and a
			// partial undo of several edited lines usable rather than requiring the
			// entire document to equal the original report snapshot.
			if (!enumerator)
				RebuildEnumerator();
			if (!enumerator)
				return false;

			std::vector<std::size_t> touched;
			for (AssDialogue const *line : resolved_lines) {
				if (!RecomputeChangedLine(*line, touched))
					return false;
			}

			replacements_applied = false;
			stale = false;
			copy_button->Enable(true);
			PopulateList();
			return true;
		}

		// Replace reports retain historical before/after data and deliberately do
		// not re-run substitution. They can only become fully usable again when
		// undo restores every captured pre-replacement field exactly.
		std::vector<AssDialogue *> resolved;
		resolved.reserve(hits.size());
		for (auto const& hit : hits) {
			AssDialogue *line = core.selectionController->GetDialogueById(hit.line_id);
			if (!line || dialogue_field_text(*line, hit.field) != *hit.original_line_text)
				return false;
			resolved.push_back(line);
		}

		// Second pass: original text is back on every line — restore each hit to
		// its report-time state so rows greyed by a recompute become jumpable
		// again. Use the frozen original offsets (which index into
		// original_line_text); a recompute moved match_start to the edited text's
		// offsets, which would be out of range for original_line_text.
		for (std::size_t i = 0; i < hits.size(); ++i) {
			auto& hit = hits[i];
			AssDialogue const *line = resolved[i]; // guaranteed non-null by pass 1
			hit.row = line->Row;
			hit.start_time = line->Start.GetAssFormatted();
			hit.style = line->Style.get();
			hit.line_text = hit.original_line_text;
			hit.match_start = hit.original_match_start;
			hit.match_end = hit.original_match_end;
			hit.matched = hit.original_line_text->substr(
				hit.original_match_start, hit.original_match_end - hit.original_match_start);
			hit.jump_start = hit.original_match_start;
			hit.jump_end = hit.original_match_end;
			hit.per_hit_stale = false;
		}

		// Successful replace-report reconcile: the captured pre-replacement text
		// is back, so the historical match coordinates are valid again.
		replacements_applied = false;
		stale = false;
		copy_button->Enable(true);
		PopulateList();
		return true;

	} // end try
	catch (...) {
		// Listener-must-not-escape fallback. The caller (OnCommit COMMIT_NEW
		// branch) treats false as "reconcile failed" and calls MarkStale.
		return false;
	}
}

bool DialogSearchResults::RecomputeChangedLine(AssDialogue const& line,
                                                std::vector<std::size_t>& touched) {
	// Append this line's hit indices (in report order) to the caller's vector;
	// remember where our slice begins so the mirror write-back below addresses
	// only this call's entries.
	auto const line_hits = hit_indices_by_line.find(line.Id);
	if (line_hits == hit_indices_by_line.end())
		return true;
	std::size_t const begin = touched.size();
	touched.insert(touched.end(), line_hits->second.begin(), line_hits->second.end());
	std::size_t const count = line_hits->second.size();

	// Replace reports, and Find reports whose matcher failed to build, cannot
	// re-match precisely: grey the changed line's rows outright. (Replace All
	// followed by a manual edit on the same row is rare, and rebuilding the
	// per-hit replacement text would mean re-running the substitution flow.)
	// Still refresh display metadata (row/start_time/style/line_text) from the
	// live line so a greyed row's Context column matches its still-usable
	// siblings on the same line, instead of showing the pre-edit text.
	if (is_replace_report || !enumerator) {
		auto live_text = std::make_shared<std::string const>(
			dialogue_field_text(line, hits[touched[begin]].field));
		for (std::size_t k = 0; k < count; ++k) {
			auto& d = hits[touched[begin + k]];
			d.row = line.Row;
			d.start_time = line.Start.GetAssFormatted();
			d.style = line.Style.get();
			d.line_text = live_text;
			d.per_hit_stale = true;
		}
		return true;
	}

	// RecomputeLineHits operates on MatchHit, so project the touched hits into a
	// throwaway MatchHit buffer (preserving order), run the pure-logic align,
	// then copy refreshed fields back. Only the changed line's hits go through
	// this; unrelated hits are untouched.
	std::vector<aegisub::subtitle_match_report::MatchHit> mirror;
	mirror.reserve(count);
	for (std::size_t k = 0; k < count; ++k) {
		auto const& d = hits[touched[begin + k]];
		aegisub::subtitle_match_report::MatchHit m;
		m.line_id = d.line_id;
		m.row = d.row;
		m.start_time = d.start_time;
		m.style = d.style;
		m.matched = d.matched;
		m.line_text = d.line_text;
		m.field = d.field;
		m.start = d.match_start;
		m.end = d.match_end;
		mirror.push_back(std::move(m));
	}

	// RecomputeLineHits runs the compiled enumerator, which for a regex
	// pattern goes through boost::regex_search — and that can throw
	// boost::regex_error (complexity/recursion limits, e.g. a catastrophic
	// backtracking pattern that only fails after the line is edited). This
	// call is inside the AssFile commit listener: an exception here escapes
	// AnnounceCommitDetails before AnnounceCommit fires, so the edit that
	// triggered the recompute would silently never reach grid/video/edit-box.
	// Same failure shape as the substr-out-of-range crash in reconcile.
	// Degrade to all-grey on this line and fuse the matcher (drop it so the
	// next keystroke does not repeat the throw), mirroring InitCommon's
	// catch-around of MakeSubtitleMatchEnumerator.
	aegisub::subtitle_match_report::RecomputeResult res;
	try {
		res = aegisub::subtitle_match_report::RecomputeLineHits(
			line, settings, enumerator, mirror.begin(), mirror.end());
	}
	catch (...) {
		// Fuse the matcher so the next keystroke does not repeat the throw, then
		// mark the whole report stale. Stale is the honest state here: the panel
		// can no longer re-match edited rows, and its per-hit "no longer match"
		// wording would be a lie (the rows were never re-checked). Stale also
		// blocks jump/copy so the user is not misled by stale-but-usable rows.
		//
		// Recoverability is preserved: COMMIT_NEW (undo/redo) is handled before
		// the `if (stale) return` guard in OnCommit, so reconciliation can rebuild
		// the matcher and retry against the restored snapshot. A future Find All
		// also builds a fresh matcher. We accept losing live recompute until one
		// of those recovery paths succeeds rather than letting the throw escape
		// the commit listener.
		enumerator = nullptr;
		MarkStale();
		return false;
	}

	// First `res.refreshed` mirror entries realign, in order, to our slice of
	// `touched`; the rest (res.orphaned) keep refreshed line_text but their old
	// matched fragment, and are marked per-hit stale.
	for (std::size_t k = 0; k < count; ++k) {
		auto& d = hits[touched[begin + k]];
		auto const& m = mirror[k];
		// line_text/row/start_time/style are refreshed for every hit by
		// RecomputeLineHits (so the Context column stays consistent across a
		// line whose match count shrank); copy them back unconditionally.
		d.row = m.row;
		d.start_time = m.start_time;
		d.style = m.style;
		d.line_text = m.line_text;
		if (k < res.refreshed) {
			d.matched = m.matched;
			d.match_start = m.start;
			d.match_end = m.end;
			// Find reports jump to the match itself; no replacement applies.
			d.jump_start = m.start;
			d.jump_end = m.end;
			d.per_hit_stale = false;
		}
		else {
			// Keep the old matched fragment (offsets would be meaningless);
			// only the live line_text/metadata moved.
			d.per_hit_stale = true;
		}
	}
	return true;
}

void DialogSearchResults::OnCommit(AssFileCommitDetails commit) {
	// COMMIT_NEW is used by undo/redo (Ids preserved via AssDialogueBase) and
	// by load/close (new objects, dead Ids). Prefer reconcile over a blind stale.
	if (commit.type == AssFile::COMMIT_NEW) {
		if (!TryReconcileAfterDocumentRebuild())
			MarkStale();
		return;
	}

	// ADDREM / ORDER renumber rows and may drop lines; always invalidate.
	if ((commit.type & AssFile::COMMIT_DIAG_ADDREM) ||
	    (commit.type & AssFile::COMMIT_ORDER)) {
		MarkStale();
		return;
	}

	// Displayed columns include text, start time, and style; any of those on a
	// reported line can invalidate jump targets or the list contents.
	// COMMIT_STYLES alone is intentionally omitted: style manager delete/paste
	// can leave a dangling Style *name* in the column, but jump ranges are
	// unaffected, and greying the whole report on pure style-list edits is
	// worse UX. Style renames that also rewrite events include COMMIT_DIAG_META.
	static constexpr int kContentMask =
		AssFile::COMMIT_DIAG_TEXT |
		AssFile::COMMIT_DIAG_META |
		AssFile::COMMIT_DIAG_TIME;
	if (!(commit.type & kContentMask))
		return;

	// Many commits (multi-select edit, Replace Next/All) pass an empty
	// changed_lines span. Scope is then unknown: treat as whole-file dirty.
	if (commit.changed_lines.empty()) {
		MarkStale();
		return;
	}

	// Already stale: a recompute cannot un-stale the panel, and rebuilding the
	// list per keystroke would both cost O(rows) and fight the stale banner.
	// The first edit that invalidated everything already called MarkStale().
	if (stale)
		return;

	// A content edit on a reported line may have shifted or removed its match.
	// Re-match that line in place instead of greying the whole report: rows
	// that still match keep working, rows that don't grey out individually,
	// and unrelated rows are untouched.
	//
	// We do NOT MarkStale when every touched row goes grey: the amend text
	// commits from keystroke editing are COMMIT_DIAG_TEXT, not COMMIT_NEW, so
	// they never reach the reconcile path that could clear `stale`. Marking
	// stale on all-grey would make a 1-hit report unrecoverable (type a char
	// that breaks the match → stale → backspace → blocked by the guard above),
	// while a 2-hit report under the same sequence recovers via recompute.
	// UpdateStatusBar already renders "0 matches found.  (1 no longer match)",
	// which is clear enough; keeping the panel on the recompute path preserves
	// recoverability regardless of hit count.
	std::vector<std::size_t> touched;
	for (AssDialogue const *line : commit.changed_lines) {
		if (!line || !hit_indices_by_line.count(line->Id))
			continue;
		if (!RecomputeChangedLine(*line, touched))
			return;
	}

	if (touched.empty())
		return;

	// Re-render only the rows RecomputeChangedLine actually modified (it never
	// touches hits on other line ids), avoiding a full-rebuild per keystroke.
	// Query the two colours once: wxSystemSettings::GetColour is not free and
	// list->GetTextColour() is a SendMessage (LVM_GETTEXTCOLOR) on wxMSW.
	wxColour const grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	wxColour const normal = list->GetTextColour();
	for (auto i : touched)
		UpdateListRow(i, grey, normal);
	UpdateStatusBar();
}

void DialogSearchResults::OnActivate(wxListEvent& evt) {
	// Item data is the hits index; visual row may diverge if columns are sorted.
	JumpToHit(static_cast<std::size_t>(list->GetItemData(evt.GetIndex())));
}

void DialogSearchResults::JumpToHit(std::size_t hit_index) {
	if (stale || hit_index >= hits.size())
		return;

	auto const& hit = hits[hit_index];
	// A row whose match no longer exists on the live line: refuse silently.
	// Other rows remain jumpable; the row itself is greyed by UpdateListRow
	// (and by PopulateList on a full rebuild). Signal the refusal rather than
	// being silent, so a double-click that does nothing is not mistaken for a
	// bug (consistent with OnCopySelected's all-grey feedback).
	if (hit.per_hit_stale) {
		wxBell();
		return;
	}
	auto core = c->GetCore();
	AssDialogue *line = core.selectionController->GetDialogueById(hit.line_id);
	if (!line)
		return;

	core.selectionController->SetSelectionAndActive({ line }, line);
	// Offsets are only meaningful in the subtitle text edit box for TEXT
	// searches (same rule as SearchReplaceEngine::FindReplace).
	if (hit.field == SearchReplaceSettings::Field::TEXT) {
		core.textSelectionController->SetSelection(static_cast<long>(hit.jump_start),
		                                            static_cast<long>(hit.jump_end));
	}
}

void DialogSearchResults::OnCopySelected(wxCommandEvent&) {
	if (stale)
		return;

	std::string out;
	int selected = 0;
	long i = -1;
	while ((i = list->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
		auto const idx = static_cast<std::size_t>(list->GetItemData(i));
		if (idx >= hits.size())
			continue;
		++selected;
		// Skip per-hit-stale rows: their match no longer exists and the row is
		// only kept for context, so copying would export stale content.
		if (hits[idx].per_hit_stale)
			continue;
		if (!out.empty())
			out.push_back('\n');
		out += *hits[idx].line_text;
	}

	if (out.empty()) {
		// No usable rows copied. Distinguish "every selected row was greyed"
		// (worth signalling) from "nothing was selected" (the app-wide no-op
		// convention for an empty selection).
		if (selected > 0)
			wxBell();
		return;
	}
	SetClipboard(out);
}

void DialogSearchResults::Show(agi::Context *context, SearchReplaceSettings settings,
                               std::vector<aegisub::subtitle_match_report::MatchHit> matches) {
	if (matches.empty())
		return;
	close_existing_results_dialog(context);
	auto *dialog = new DialogSearchResults(context, std::move(settings), std::move(matches));
	search_results_dialogs[context] = dialog;
	dialog->wxDialog::Show();
	dialog->Raise();
}

void DialogSearchResults::Show(agi::Context *context, SearchReplaceSettings settings,
                               std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements) {
	if (replacements.empty())
		return;
	close_existing_results_dialog(context);
	auto *dialog = new DialogSearchResults(context, std::move(settings), std::move(replacements));
	search_results_dialogs[context] = dialog;
	dialog->wxDialog::Show();
	dialog->Raise();
}

void DialogSearchResults::Dismiss(agi::Context *context) {
	close_existing_results_dialog(context);
}
