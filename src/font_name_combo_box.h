#pragma once

#include "font_family_selection_model.h"

#include <optional>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/combobox.h>
#include <wx/timer.h>

#ifdef __WXMSW__
#include <windows.h>
#include <commctrl.h>
#endif

namespace font_name_combo_box_detail {

inline wxString Fold(wxString value) {
	return value.Lower();
}

struct ContainsMatch {
	std::size_t index = 0;
	FontFamilyId family_id = 0;
};

/// First choice whose label contains the query (case-insensitive).
/// Always searches from the catalog head; the list itself is never filtered.
inline std::optional<ContainsMatch> FindContainsMatch(
	std::vector<std::pair<wxString, FontFamilyId>> const& choices,
	wxString const& query) noexcept
{
	if (query.empty())
		return std::nullopt;

	auto const folded_query = Fold(query);
	for (std::size_t index = 0; index < choices.size(); ++index) {
		auto const& [label, family_id] = choices[index];
		// family_id may be 0 for enumerator fallback choices; match by label.
		if (Fold(label).Find(folded_query) != wxNOT_FOUND)
			return ContainsMatch{index, family_id};
	}
	return std::nullopt;
}

/// Typed length after stripping a native auto-complete suffix, if any.
/// A fully selected value (selection from 0) is a committed choice, not an
/// empty typed prefix.
inline std::size_t TypedQueryLength(
	std::size_t value_length,
	long selection_start,
	long selection_end,
	long insertion_point) noexcept
{
	if (selection_start > 0
		&& selection_start < selection_end
		&& selection_end == static_cast<long>(value_length)
		&& insertion_point == selection_start)
		return static_cast<std::size_t>(selection_start);
	return value_length;
}

} // namespace font_name_combo_box_detail

/// Editable font selector with optional case-insensitive contains matching.
///
/// When contains matching is off, the control is a plain editable combo: native
/// prefix behaviour is left untouched.
///
/// When enabled, the intentional difference is the match predicate (contains
/// instead of prefix). Opening the list or typing while it is open jumps the
/// drop-down highlight to the first contains match. The list is never filtered.
///
/// MSW note: moving the list caret can rewrite the edit field, and a later
/// native paint/message can clear our highlight. We therefore (1) restore the
/// edit only when it actually changed (avoid SetWindowText-driven native
/// prefix walk), (2) jump the list caret once per open/key with redraw frozen,
/// and (3) re-apply that same caret once after a short settle timer — not a
/// second full match pass (which looked like scrolling item-by-item when
/// typing first and opening the list second).
class FontNameComboBox final : public wxComboBox {
	struct DisplayChoice {
		wxString label;
		FontFamilyId family_id = 0;
	};

	std::vector<DisplayChoice> all_choices;
	std::vector<std::pair<wxString, FontFamilyId>> match_choices;
	std::optional<FontFamilyId> selected_family_id;
	/// Last contains match row while searching (not a committed selection).
	std::optional<std::size_t> highlight_index;
	/// User-typed query while searching; edit should show this until commit.
	wxString typed_query;
	bool contains_matching = false;
	bool applying = false;
	wxTimer settle_timer;

	static wxArrayString ToWxChoices(std::vector<FontFamilyChoice> const& choices) {
		wxArrayString result;
		result.reserve(choices.size());
		for (auto const& choice : choices)
			result.Add(wxString::FromUTF8(choice.label.c_str()));
		return result;
	}

	bool IsDropDownOpen() const {
#ifdef __WXMSW__
		auto const hwnd = GetHWND();
		if (!hwnd)
			return false;
		return ::SendMessageW(reinterpret_cast<HWND>(hwnd), CB_GETDROPPEDSTATE, 0, 0) != 0;
#else
		return false;
#endif
	}

	std::optional<FontFamilyId> ExactListMatch(wxString const& value) const {
		for (auto const& choice : all_choices) {
			if (choice.label == value && choice.family_id != 0)
				return choice.family_id;
		}
		return std::nullopt;
	}

	std::optional<std::size_t> ExactListIndex(wxString const& value) const {
		for (std::size_t index = 0; index < all_choices.size(); ++index) {
			if (all_choices[index].label == value)
				return index;
		}
		return std::nullopt;
	}

	/// Read typed text; strip a native auto-complete suffix if present.
	wxString CaptureTypedQuery() {
		auto value = GetValue();
		long selection_start = 0;
		long selection_end = 0;
		GetSelection(&selection_start, &selection_end);
		auto const length = font_name_combo_box_detail::TypedQueryLength(
			value.length(), selection_start, selection_end, GetInsertionPoint());

		if (length < value.length()) {
			typed_query = value.Left(length);
			return typed_query;
		}

		// List/caret move may have replaced the edit with a full family name
		// while we still hold a shorter typed query — keep the typed query.
		if (!typed_query.empty()
			&& typed_query.length() < value.length()
			&& ExactListIndex(value)) {
			auto const folded_value = font_name_combo_box_detail::Fold(value);
			auto const folded_typed = font_name_combo_box_detail::Fold(typed_query);
			if (folded_value.Find(folded_typed) != wxNOT_FOUND)
				return typed_query;
		}

		typed_query = value;
		return typed_query;
	}

	/// Write the edit field without selecting a list item (CB_SETCURSEL fills).
	void ForceEditText(wxString const& text) {
		applying = true;
#ifdef __WXMSW__
		auto const combo_hwnd = reinterpret_cast<HWND>(GetHWND());
		if (combo_hwnd) {
			COMBOBOXINFO info{};
			info.cbSize = sizeof(info);
			if (::GetComboBoxInfo(combo_hwnd, &info) && info.hwndItem)
				::SetWindowTextW(info.hwndItem, text.wc_str());
			::SetWindowTextW(combo_hwnd, text.wc_str());
		}
#endif
		if (GetValue() != text)
			ChangeValue(text);
		SetInsertionPoint(static_cast<long>(text.length()));
		applying = false;
	}

	/// List highlight + scroll only (not CB_SETCURSEL — that commits the name).
	/// Redraw is frozen so a late native prefix step does not paint intermediate
	/// rows when we immediately correct the caret (type-then-open path).
	void ApplyListCaret(std::size_t index) {
		if (index >= all_choices.size())
			return;
#ifdef __WXMSW__
		auto const combo_hwnd = reinterpret_cast<HWND>(GetHWND());
		if (!combo_hwnd)
			return;
		COMBOBOXINFO info{};
		info.cbSize = sizeof(info);
		if (!::GetComboBoxInfo(combo_hwnd, &info) || !info.hwndList)
			return;

		auto const list = info.hwndList;
		// Already on the target row and it is the top item — nothing to do.
		// Avoids a second LB_SETTOPINDEX looking like another scroll step.
		if (::SendMessageW(list, LB_GETCURSEL, 0, 0) == static_cast<LRESULT>(index)
			&& ::SendMessageW(list, LB_GETTOPINDEX, 0, 0) == static_cast<LRESULT>(index))
			return;

		applying = true;
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
		::SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(index), 0);
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		::RedrawWindow(list, nullptr, nullptr,
			RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
		applying = false;
#else
		wxUnusedVar(index);
#endif
	}

	void StopSettleTimer() {
		if (settle_timer.IsRunning())
			settle_timer.Stop();
	}

	void StartSettleTimer() {
		// One caret re-stick if native clears the highlight after open/paint.
		// Does not re-run matching; only re-applies highlight_index.
		if (!settle_timer.IsRunning())
			settle_timer.StartOnce(50);
	}

	void OnSettleTimer(wxTimerEvent&) {
		StopSettleTimer();
		if (!contains_matching || !highlight_index || !IsDropDownOpen())
			return;
		// Caret only — rewriting the edit here re-triggers native list search
		// and looks like the list scrolling through items one by one.
		ApplyListCaret(*highlight_index);
		if (!typed_query.empty() && GetValue() != typed_query)
			ForceEditText(typed_query);
	}

	void JumpToMatch(std::size_t index, wxString const& typed) {
		if (index >= all_choices.size())
			return;
		typed_query = typed;
		highlight_index = index;

		// Caret first (may rewrite edit). Only force the edit when native
		// actually changed it — SetWindowText while dropped runs prefix search
		// and paints intermediate rows (type-then-open "scroll through" feel).
		ApplyListCaret(index);
		if (GetValue() != typed_query)
			ForceEditText(typed_query);
		StartSettleTimer();
	}

	void UpdateContainsHighlight(bool force_jump) {
		if (!contains_matching || applying)
			return;

		auto const query = CaptureTypedQuery();
		if (query.empty()) {
			selected_family_id.reset();
			typed_query.clear();
			highlight_index.reset();
			StopSettleTimer();
			return;
		}

		// Exact full-label type-in: commit family id, still jump the list row.
		if (auto const exact = ExactListMatch(query)) {
			selected_family_id = exact;
			if (auto const index = ExactListIndex(query);
				index && (force_jump || IsDropDownOpen()))
				JumpToMatch(*index, query);
			return;
		}
		selected_family_id.reset();

		auto const match = font_name_combo_box_detail::FindContainsMatch(
			match_choices, query);
		if (!match) {
			highlight_index.reset();
			StopSettleTimer();
			if (GetValue() != typed_query && !typed_query.empty())
				ForceEditText(typed_query);
			return;
		}

		// Remember match while closed; jump as soon as the list is open.
		highlight_index = match->index;
		if (force_jump || IsDropDownOpen())
			JumpToMatch(match->index, query);
		else if (GetValue() != typed_query && !typed_query.empty())
			ForceEditText(typed_query);
	}

	void OnText(wxCommandEvent& event) {
		if (!applying && contains_matching) {
			CallAfter([this] {
				UpdateContainsHighlight(/*force_jump=*/IsDropDownOpen());
			});
		}
		event.Skip();
	}

	void OnDropDown(wxCommandEvent& event) {
		if (!applying && contains_matching) {
			// Snapshot typed text before open-side effects rewrite the edit.
			// Do this synchronously so the deferred jump uses the real query
			// even if native fills a family name into the edit on drop.
			auto const snapshot = CaptureTypedQuery();
			if (!snapshot.empty())
				typed_query = snapshot;

			// One deferred jump only. A second full JumpToMatch (old double
			// CallAfter) re-scrolled the list and looked like item-by-item
			// motion. List HWND may still be missing on the first tick; the
			// settle timer re-applies caret only once without re-matching.
			CallAfter([this] {
				UpdateContainsHighlight(/*force_jump=*/true);
			});
		}
		event.Skip();
	}

	void OnCloseUp(wxCommandEvent& event) {
		StopSettleTimer();
		// Closing without an explicit pick must leave the typed query, not the
		// temporarily highlighted family name.
		if (highlight_index && !typed_query.empty() && GetValue() != typed_query)
			ForceEditText(typed_query);
		event.Skip();
	}

	void CommitListSelection(int selection) {
		if (selection < 0
			|| static_cast<std::size_t>(selection) >= all_choices.size()) {
			selected_family_id.reset();
			return;
		}

		auto const& choice = all_choices[static_cast<std::size_t>(selection)];
		StopSettleTimer();
		highlight_index.reset();
		typed_query = choice.label;
		ForceEditText(choice.label);

		if (choice.family_id != 0)
			selected_family_id = choice.family_id;
		else
			selected_family_id.reset();
	}

	bool ProcessEvent(wxEvent& event) override {
		if (!applying && event.GetId() == GetId()
			&& event.GetEventType() == wxEVT_COMBOBOX) {
			CommitListSelection(static_cast<wxCommandEvent&>(event).GetInt());
		}
		return wxComboBox::ProcessEvent(event);
	}

public:
	FontNameComboBox(
		wxWindow *parent,
		wxString const& value,
		wxSize const& size,
		std::vector<FontFamilyChoice> const& choices,
		bool contains_matching)
	: wxComboBox(parent, -1, value, wxDefaultPosition, size, ToWxChoices(choices),
		wxCB_DROPDOWN | wxTE_PROCESS_ENTER)
	, typed_query(value)
	, contains_matching(contains_matching)
	{
		all_choices.reserve(choices.size());
		match_choices.reserve(choices.size());
		for (auto const& choice : choices) {
			auto label = wxString::FromUTF8(choice.label.c_str());
			all_choices.push_back({label, choice.family_id});
			match_choices.emplace_back(label, choice.family_id);
		}

		if (contains_matching) {
			settle_timer.SetOwner(this);
			Bind(wxEVT_TIMER, &FontNameComboBox::OnSettleTimer, this,
				settle_timer.GetId());
			Bind(wxEVT_TEXT, &FontNameComboBox::OnText, this);
#if wxCHECK_VERSION(3, 1, 0)
			Bind(wxEVT_COMBOBOX_DROPDOWN, &FontNameComboBox::OnDropDown, this);
			Bind(wxEVT_COMBOBOX_CLOSEUP, &FontNameComboBox::OnCloseUp, this);
#endif
		}

		if (auto const exact = ExactListMatch(value))
			selected_family_id = exact;
	}

	~FontNameComboBox() override {
		StopSettleTimer();
	}

	/// Committed family only (explicit list pick or exact typed label).
	std::optional<FontFamilyId> SelectedFamilyId() const noexcept {
		return selected_family_id;
	}
};
