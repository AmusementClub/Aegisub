/// @file dialog_search_results.h
/// @brief Results panel for Find All / Replace All.

#pragma once

#include "subtitle_match_report.h"

#include <libaegisub/signal.h>
#include <wx/dialog.h>

#include <memory>
#include <unordered_set>
#include <vector>

namespace agi { struct Context; }
struct AssFileCommitDetails;
class AssDialogue;
class wxListView;
class wxStaticText;
class wxButton;
class wxListEvent;

/// Modeless list of matches or replacements produced by SearchReplaceEngine.
/// Not registered with DialogManager: construction needs the hit vectors, not
/// only an agi::Context*. Closed via Destroy (not Hide) so commit listeners
/// cannot accumulate.
class DialogSearchResults final : public wxDialog {
	/// Display rows: MatchHit fields plus optional replacement / post-edit range.
	struct DisplayHit {
		int line_id = 0;
		int row = -1;
		std::string start_time;
		std::string style;
		std::string matched;
		std::shared_ptr<std::string const> line_text;
		std::string replacement;
		SearchReplaceSettings::Field field = SearchReplaceSettings::Field::TEXT;
		/// Byte range in `line_text` (pre-replacement for replace reports).
		std::size_t match_start = 0;
		std::size_t match_end = 0;
		/// Range used for jump; post-replacement while the edit still applies,
		/// otherwise the same as match_start/match_end.
		std::size_t jump_start = 0;
		std::size_t jump_end = 0;
	};

	agi::Context *c;
	/// Whether the Replacement column is shown (historical report shape).
	bool has_replacement = false;
	/// Whether the document still has those replacements applied. Cleared when
	/// undo restores pre-replacement field text so status/jumps stay honest.
	bool replacements_applied = false;
	bool stale = false;

	wxListView *list = nullptr;
	wxStaticText *status = nullptr;
	wxButton *copy_button = nullptr;
	agi::signal::Connection file_changed_slot;

	std::vector<DisplayHit> hits;
	/// Line Ids present in `hits`, for O(1) intersection with commit spans.
	std::unordered_set<int> hit_line_ids;

	void InitCommon(bool replace_mode);
	void PopulateList();
	void MarkStale();
	/// Undo/redo COMMIT_NEW keeps AssDialogue::Id values. If every hit still
	/// resolves and its captured field text is still on the live line, refresh
	/// display metadata and keep the panel usable; otherwise mark stale.
	bool TryReconcileAfterDocumentRebuild();
	void OnCommit(AssFileCommitDetails commit);
	void OnActivate(wxListEvent& evt);
	void OnCopySelected(wxCommandEvent&);
	/// `hit_index` is the index into `hits` (stored as item data), not the
	/// list-view visual row.
	void JumpToHit(std::size_t hit_index);

	DialogSearchResults(agi::Context *c,
	                    std::vector<aegisub::subtitle_match_report::MatchHit> matches);
	DialogSearchResults(agi::Context *c,
	                    std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements);

public:
	static void Show(agi::Context *context,
	                 std::vector<aegisub::subtitle_match_report::MatchHit> matches);
	static void Show(agi::Context *context,
	                 std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements);
	/// Close any open results panel (e.g. after a search with zero hits).
	static void Dismiss(agi::Context *context);

	~DialogSearchResults();
};
