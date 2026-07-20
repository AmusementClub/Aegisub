#pragma once

#include <algorithm>
#include <vector>

#include <wx/arrstr.h>
#include <wx/combobox.h>

/// Editable font selector with optional case-insensitive contains filtering.
/// The complete choice list is retained so deleting text can restore matches.
class FontNameComboBox final : public wxComboBox {
	std::vector<wxString> all_choices;
	bool contains_matching;
	bool rebuilding = false;

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
		for (auto const& choice : all_choices) {
			if (folded_query.empty() || Fold(choice).Find(folded_query) != wxNOT_FOUND)
				Append(choice);
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

public:
	FontNameComboBox(
		wxWindow *parent,
		wxString const& value,
		wxSize const& size,
		wxArrayString const& choices,
		bool contains_matching)
	: wxComboBox(parent, -1, value, wxDefaultPosition, size, choices,
		wxCB_DROPDOWN | wxTE_PROCESS_ENTER)
	, all_choices(choices.begin(), choices.end())
	, contains_matching(contains_matching)
	{
		Bind(wxEVT_TEXT, &FontNameComboBox::OnText, this);
	}
};
