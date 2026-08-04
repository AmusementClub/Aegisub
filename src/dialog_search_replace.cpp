// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

/// @file dialog_search_replace.cpp
/// @brief Find and Search/replace dialogue box and logic
/// @ingroup secondary_ui
///

#include "dialog_search_replace.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dialog_search_results.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "search_replace_engine.h"
#include "selection_controller.h"
#include "subs_edit_box.h"
#include "text_selection_controller.h"
#include "utils.h"
#include "validators.h"

#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <functional>
#include <unordered_set>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/combobox.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valgen.h>
#include <wx/wupdlock.h>

namespace {
DialogSearchReplace *search_replace_dialog = nullptr;
}

DialogSearchReplace::DialogSearchReplace(agi::Context* c, bool replace)
: wxDialog(c->GetUI().parent, -1, replace ? _("Replace") : _("Find"))
, c(c)
, settings(agi::make_unique<SearchReplaceSettings>())
, has_replace(replace)
{
	auto recent_find(lagi_MRU_wxAS("Find"));
	auto recent_replace(lagi_MRU_wxAS("Replace"));

	settings->field = static_cast<SearchReplaceSettings::Field>(OPT_GET("Tool/Search Replace/Field")->GetInt());
	settings->limit_to = static_cast<SearchReplaceSettings::Limit>(OPT_GET("Tool/Search Replace/Affect")->GetInt());
	settings->find = recent_find.empty() ? std::string() : from_wx(recent_find.front());
	settings->replace_with = recent_replace.empty() ? std::string() : from_wx(recent_replace.front());
	settings->match_case = OPT_GET("Tool/Search Replace/Match Case")->GetBool();
	settings->use_regex = OPT_GET("Tool/Search Replace/RegExp")->GetBool();
	settings->use_unicode_escapes = OPT_GET("Tool/Search Replace/Unicode Escapes")->GetBool();
	settings->ignore_comments = OPT_GET("Tool/Search Replace/Skip Comments")->GetBool();
	settings->skip_tags = OPT_GET("Tool/Search Replace/Skip Tags")->GetBool();
	settings->exact_match = false;

	auto find_sizer = new wxFlexGridSizer(2, 2, 5, 15);
	find_edit = new wxComboBox(this, -1, wxEmptyString, wxDefaultPosition, wxSize(300, -1), recent_find, wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->find));
	find_edit->SetMaxLength(0);
	find_sizer->Add(new wxStaticText(this, -1, _("Find what:")), wxSizerFlags().Center().Left());
	find_sizer->Add(find_edit);

	if (has_replace) {
		replace_edit = new wxComboBox(this, -1, wxEmptyString, wxDefaultPosition, wxSize(300, -1), lagi_MRU_wxAS("Replace"), wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->replace_with));
		replace_edit->SetMaxLength(0);
		find_sizer->Add(new wxStaticText(this, -1, _("Replace with:")), wxSizerFlags().Center().Left());
		find_sizer->Add(replace_edit);
	}

	auto options_sizer = new wxBoxSizer(wxVERTICAL);
	options_sizer->Add(new wxCheckBox(this, -1, _("&Match case"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->match_case)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Use regular expressions"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->use_regex)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("Use &Unicode escapes"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->use_unicode_escapes)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Skip Comments"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->ignore_comments)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("S&kip Override Tags"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->skip_tags)));

	auto left_sizer = new wxBoxSizer(wxVERTICAL);
	left_sizer->Add(find_sizer, wxSizerFlags().DoubleBorder(wxBOTTOM));
	left_sizer->Add(options_sizer);

	wxString field[] = { _("&Text"), _("St&yle"), _("A&ctor"), _("&Effect") };
	wxString affect[] = { _("A&ll rows"), _("Selected &rows") };

	// Bottom filter row: left column (In Field / Limit to stacked), right column (style filter)
	auto field_radio = new wxRadioBox(this, -1, _("In Field"), wxDefaultPosition, wxDefaultSize, countof(field), field, 0, wxRA_SPECIFY_COLS, MakeEnumBinder(&settings->field));
	auto limit_radio = new wxRadioBox(this, -1, _("Limit to"), wxDefaultPosition, wxDefaultSize, countof(affect), affect, 0, wxRA_SPECIFY_COLS, MakeEnumBinder(&settings->limit_to));

	auto field_col = new wxBoxSizer(wxVERTICAL);
	field_col->Add(field_radio, wxSizerFlags().Expand().Border(wxBOTTOM));
	field_col->Add(limit_radio, wxSizerFlags().Expand());

	auto style_filter_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Filter by style"));
	style_filter_box = new wxCheckListBox(this, -1, wxDefaultPosition, FromDIP(wxSize(150, 60)),
	                                     to_wx(c->GetCore().ass->GetStyles()));
	style_filter_box->SetToolTip(_("Only search within events that use the checked styles. Leave all unchecked to search every row."));
	style_filter_sizer->Add(style_filter_box, wxSizerFlags(1).Expand().Border(wxBOTTOM));

	auto style_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
	auto style_all_btn = new wxButton(this, -1, _("&All"));
	auto style_none_btn = new wxButton(this, -1, _("&None"));
	style_btn_sizer->Add(style_all_btn, wxSizerFlags().Border(wxRIGHT));
	style_btn_sizer->Add(style_none_btn);
	style_filter_sizer->Add(style_btn_sizer, wxSizerFlags().Center());

	auto filter_sizer = new wxBoxSizer(wxHORIZONTAL);
	filter_sizer->Add(field_col, wxSizerFlags().Border(wxRIGHT));
	filter_sizer->Add(style_filter_sizer, wxSizerFlags(1).Expand());

	auto find_next = new wxButton(this, -1, _("&Find next"));
	auto find_all = new wxButton(this, -1, _("Find &all"));
	auto replace_next = new wxButton(this, -1, _("Replace &next"));
	auto replace_all = new wxButton(this, -1, _("Replace &all"));
	find_next->SetDefault();

	auto button_sizer = new wxBoxSizer(wxVERTICAL);
	button_sizer->Add(find_next, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(find_all, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(replace_next, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(replace_all, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(new wxButton(this, wxID_CANCEL));

	if (!has_replace) {
		button_sizer->Hide(replace_next);
		button_sizer->Hide(replace_all);
	}

	auto top_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(left_sizer, wxSizerFlags().Border());
	top_sizer->Add(button_sizer, wxSizerFlags().Border());

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer);
	main_sizer->Add(filter_sizer, wxSizerFlags().Border());
	SetSizerAndFit(main_sizer);
	CenterOnParent();

	TransferDataToWindow();
	find_edit->SetFocus();
	find_edit->SelectAll();

	find_edit->Bind(wxEVT_TEXT_ENTER, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::FindNext));
	if (has_replace)
	  replace_edit->Bind(wxEVT_TEXT_ENTER, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceNext));
	find_next->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::FindNext));
	find_all->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::FindAll));
	replace_next->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceNext));
	replace_all->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceAll));

	style_all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		for (unsigned int i = 0; i < style_filter_box->GetCount(); ++i)
			style_filter_box->Check(i, true);
	});
	style_none_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		for (unsigned int i = 0; i < style_filter_box->GetCount(); ++i)
			style_filter_box->Check(i, false);
	});

	file_changed_slot = c->GetCore().ass->AddCommitListener(&DialogSearchReplace::OnCommit, this);
}

DialogSearchReplace::~DialogSearchReplace() {
	// Show can replace one dialog shape with the other using asynchronous
	// Destroy(). Do not let the old instance clear the newer pointer later.
	if (search_replace_dialog == this)
		search_replace_dialog = nullptr;
}

void DialogSearchReplace::FindReplace(bool (SearchReplaceEngine::*func)()) {
	TransferDataFromWindow();

	if (settings->find.empty())
		return;

	settings->match_styles.clear();
	for (unsigned int i = 0; i < style_filter_box->GetCount(); ++i)
		if (style_filter_box->IsChecked(i))
			settings->match_styles.push_back(from_wx(style_filter_box->GetString(i)));

	auto core = c->GetCore();
	core.search->Configure(*settings);
	try {
		((*core.search).*func)();
	}
	catch (agi::Exception const& e) {
		c->ShowError(e.GetMessage());
		return;
	}
	catch (std::exception const& e) {
		c->ShowError(e.what());
		return;
	}

	// Find All / Replace All store reports on the engine; open the panel here
	// rather than from the engine so dialog lifetime stays in dialog code.
	// Empty results must dismiss any previous panel so the UI cannot keep
	// showing a report from an earlier query. Pass the active settings so the
	// panel can re-match an edited row in place instead of greying the report.
	bool results_shown = false;
	if (!core.search->GetLastReplacements().empty()) {
		DialogSearchResults::Show(c, core.search->GetSettings(), core.search->GetLastReplacements());
		results_shown = true;
	}
	else if (!core.search->GetLastMatches().empty()) {
		DialogSearchResults::Show(c, core.search->GetSettings(), core.search->GetLastMatches());
		results_shown = true;
	}
	else if (func == &SearchReplaceEngine::FindAll || func == &SearchReplaceEngine::ReplaceAll)
		DialogSearchResults::Dismiss(c);

	config::mru->Add("Find", settings->find);
	if (has_replace)
		config::mru->Add("Replace", settings->replace_with);

	OPT_SET("Tool/Search Replace/Match Case")->SetBool(settings->match_case);
	OPT_SET("Tool/Search Replace/RegExp")->SetBool(settings->use_regex);
	OPT_SET("Tool/Search Replace/Unicode Escapes")->SetBool(settings->use_unicode_escapes);
	OPT_SET("Tool/Search Replace/Skip Comments")->SetBool(settings->ignore_comments);
	OPT_SET("Tool/Search Replace/Skip Tags")->SetBool(settings->skip_tags);
	OPT_SET("Tool/Search Replace/Field")->SetInt(static_cast<int>(settings->field));
	OPT_SET("Tool/Search Replace/Affect")->SetInt(static_cast<int>(settings->limit_to));

	UpdateDropDowns();
	if (!results_shown)
		find_edit->SetFocus();
}

static void update_mru(wxComboBox *cb, const char *mru_name) {
	cb->Freeze();
	cb->Clear();
	cb->Append(lagi_MRU_wxAS(mru_name));
	if (!cb->IsListEmpty())
		cb->SetSelection(0);
	cb->Thaw();
}

static wxString get_selected_text_for_search(agi::Context *context) {
	if (auto *edit_box = context->GetUI().subsEditBox) {
		auto selected = edit_box->GetEditControlSelectedText();
		if (!selected.empty())
			return to_wx(selected);
	}

	if (auto *active_line = context->selectionController->GetActiveLine()) {
		auto const& tsc = context->textSelectionController;
		long sel_start = tsc->GetSelectionStart();
		long sel_end = tsc->GetSelectionEnd();
		if (sel_start > sel_end)
			std::swap(sel_start, sel_end);
		if (sel_start != sel_end && sel_start >= 0) {
			auto const& text = active_line->Text.get();
			if (static_cast<size_t>(sel_end) <= text.size())
				return to_wx(text.substr(sel_start, sel_end - sel_start));
		}
	}

	return {};
}

void DialogSearchReplace::UpdateDropDowns() {
	update_mru(find_edit, "Find");

	if (has_replace)
		update_mru(replace_edit, "Replace");
}

void DialogSearchReplace::PopulateStyleFilter() {
	wxWindowUpdateLocker freeze(style_filter_box);

	std::unordered_set<std::string> checked;
	for (unsigned int i = 0; i < style_filter_box->GetCount(); ++i)
		if (style_filter_box->IsChecked(i))
			checked.insert(from_wx(style_filter_box->GetString(i)));

	style_filter_box->Clear();
	style_filter_box->Append(to_wx(c->GetCore().ass->GetStyles()));

	for (unsigned int i = 0; i < style_filter_box->GetCount(); ++i)
		if (checked.count(from_wx(style_filter_box->GetString(i))))
			style_filter_box->Check(i, true);
}

void DialogSearchReplace::OnCommit(int type, AssDialogue const* /*changed*/) {
	if (type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_STYLES)
		PopulateStyleFilter();
}

void DialogSearchReplace::Show(agi::Context *context, bool replace) {
	wxString preselected = get_selected_text_for_search(context);

	if (search_replace_dialog && replace != search_replace_dialog->has_replace) {
		// Already opened, but wrong type - destroy and create the right one
		search_replace_dialog->Destroy();
		search_replace_dialog = nullptr;
	}

	if (!search_replace_dialog)
		search_replace_dialog = new DialogSearchReplace(context, replace);

	if (!preselected.empty()) {
		search_replace_dialog->find_edit->SetValue(preselected);
		search_replace_dialog->settings->find = from_wx(preselected);
	}

	search_replace_dialog->find_edit->SetFocus();
	search_replace_dialog->find_edit->SelectAll();
	search_replace_dialog->wxDialog::Show();
	search_replace_dialog->Raise();
}

void DialogSearchReplace::Focus(agi::Context *context) {
	if (!search_replace_dialog || search_replace_dialog->c != context ||
	    !search_replace_dialog->IsShown())
		return;

	search_replace_dialog->Raise();
	search_replace_dialog->find_edit->SetFocus();
}
