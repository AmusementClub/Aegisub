#pragma once

#include "font_family_selection_model.h"

#include <algorithm>
#include <optional>
#include <vector>

#include <wx/arrstr.h>
#include <wx/combobox.h>

/// Editable font selector with optional case-insensitive contains filtering.
/// The complete choice list is retained so deleting text can restore matches.
class FontNameComboBox final : public wxComboBox {
	struct DisplayChoice {
		wxString label;
		FontFamilyId family_id = 0;
	};
	std::vector<DisplayChoice> all_choices;
	std::vector<FontFamilyId> visible_family_ids;
	std::optional<FontFamilyId> selected_family_id;
	std::optional<FontFamilyId> pending_selection_id;
	bool contains_matching;
	bool rebuilding = false;

	static wxArrayString ToWxChoices(std::vector<FontFamilyChoice> const& choices) {
		wxArrayString result;
		result.reserve(choices.size());
		for (auto const& choice : choices)
			result.Add(wxString::FromUTF8(choice.label.c_str()));
		return result;
	}

	static wxString Fold(wxString value) {
		return value.Lower();
	}

	wxString TypedQuery() const {
		auto query = GetValue();
		long selection_start = 0;
		long selection_end = 0;
		GetSelection(&selection_start, &selection_end);
		// Native editable combos may select the auto-completed suffix after
		// matching a prefix. Ignore that suffix while filtering.
		if (selection_start < selection_end
			&& selection_end == static_cast<long>(query.length())
			&& GetInsertionPoint() == selection_start)
			query = query.Left(selection_start);
		return query;
	}

	void RebuildChoices(wxString const& query) {
		auto const folded_query = Fold(query);
		auto const insertion_point = GetInsertionPoint();

		Freeze();
		Clear();
		visible_family_ids.clear();
		for (auto const& choice : all_choices) {
			if (folded_query.empty() || Fold(choice.label).Find(folded_query) != wxNOT_FOUND) {
				Append(choice.label);
				visible_family_ids.push_back(choice.family_id);
			}
		}
		ChangeValue(query);
		SetInsertionPoint(std::min(insertion_point, static_cast<long>(query.length())));
		Thaw();
	}

	void OnText(wxCommandEvent& event) {
		if (contains_matching && !rebuilding) {
			rebuilding = true;
			RebuildChoices(TypedQuery());
			rebuilding = false;
		}
		event.Skip();
	}

	bool ProcessEvent(wxEvent& event) override {
		if (!rebuilding && event.GetId() == GetId()) {
			if (event.GetEventType() == wxEVT_TEXT) {
				// Typing an alias must resolve through the catalog, even when its
				// spelling happens to equal one of the displayed choices.
				auto const selection = wxComboBox::GetSelection();
				if (selection >= 0 &&
				    static_cast<std::size_t>(selection) < visible_family_ids.size() &&
				    GetString(selection) == static_cast<wxCommandEvent&>(event).GetString())
					pending_selection_id = visible_family_ids[selection];
				else
					pending_selection_id.reset();
				bool const still_selected_choice = selected_family_id && selection >= 0 &&
					static_cast<std::size_t>(selection) < visible_family_ids.size() &&
					visible_family_ids[selection] == *selected_family_id &&
					GetString(selection) == static_cast<wxCommandEvent&>(event).GetString();
				if (!still_selected_choice)
					selected_family_id.reset();
			}
			else if (event.GetEventType() == wxEVT_COMBOBOX) {
				auto const& command = static_cast<wxCommandEvent&>(event);
				auto const selection = command.GetInt();
				if (selection >= 0 && static_cast<std::size_t>(selection) < visible_family_ids.size() &&
				    visible_family_ids[selection] != 0 && GetString(selection) == command.GetString())
					selected_family_id = visible_family_ids[selection];
				else if (pending_selection_id && *pending_selection_id != 0)
					selected_family_id = pending_selection_id;
				else
					selected_family_id.reset();
				pending_selection_id.reset();
			}
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
	, contains_matching(contains_matching)
	{
		all_choices.reserve(choices.size());
		for (auto const& choice : choices) {
			all_choices.push_back({
				wxString::FromUTF8(choice.label.c_str()), choice.family_id});
		}
		visible_family_ids.reserve(all_choices.size());
		for (auto const& choice : all_choices)
			visible_family_ids.push_back(choice.family_id);
		Bind(wxEVT_TEXT, &FontNameComboBox::OnText, this);
	}

	std::optional<FontFamilyId> SelectedFamilyId() const noexcept {
		return selected_family_id;
	}
};
