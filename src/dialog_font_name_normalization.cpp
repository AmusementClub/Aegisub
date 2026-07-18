#include "ass_file.h"
#include "compat.h"
#include "dialog_manager.h"
#include "font_family_catalog.h"
#include "font_family_catalog_cache.h"
#include "font_name_normalization.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

wxString MatchKindText(FontFamilyMatchKind kind) {
	switch (kind) {
		case FontFamilyMatchKind::None: return _("No match");
		case FontFamilyMatchKind::Exact: return _("Exact");
		case FontFamilyMatchKind::CaseInsensitiveExact: return _("Case-insensitive exact");
		case FontFamilyMatchKind::Ambiguous: return _("Ambiguous");
	}
	return _("Unknown");
}

wxString NameKindText(FontFamilyNameKind kind) {
	switch (kind) {
		case FontFamilyNameKind::Win32Family: return _("Win32 family");
		case FontFamilyNameKind::TypographicFamily: return _("Typographic family");
		case FontFamilyNameKind::FullName: return _("Full name");
		case FontFamilyNameKind::PostScript: return _("PostScript");
		case FontFamilyNameKind::PlatformAlias: return _("Platform alias");
	}
	return _("Unknown");
}

wxString ReasonText(std::string const& reason) {
	if (reason == "font_family_catalog_unavailable")
		return _("The installed font family catalog is unavailable.");
	if (reason == "ambiguous_family_alias")
		return _("The name matches more than one installed font family.");
	if (reason == "unrecognized_family_name")
		return _("The name is not a confirmed Win32 family name for an installed font.");
	if (reason == "english_win32_name_unavailable")
		return _("This family has no English Win32 family name.");
	if (reason == "preferred_family_name_unavailable")
		return _("The selected target name is unavailable for this family.");
	if (reason == "gdi_family_name_too_long")
		return _("The target name exceeds the GDI family-name length limit.");
	if (reason == "noncanonical_family_name_case")
		return _("The family name uses noncanonical letter case.");
	if (reason == "localized_win32_family_alias")
		return _("Replace the localized Win32 family name with its English form.");
	if (reason == "english_win32_family_alias")
		return _("Replace the English Win32 family name with its localized form.");
	if (reason == "win32_family_alias")
		return _("Replace this confirmed Win32 family alias with the selected target name.");
	return to_wx(reason);
}

wxString SourceText(FontNameSourceLocation const& source) {
	if (source.kind == FontNameSourceKind::Style)
		return _("Style") + wxS(" \"") + to_wx(source.style) + wxS("\"");

	auto text = source.comment ? _("Comment line ") : _("Dialogue line ");
	text += wxString::Format(wxS("%d"), source.line);
	text += wxS(", \\fn #");
	text += wxString::Format(wxS("%llu"),
		static_cast<unsigned long long>(source.override_index + 1));
	if (!source.style.empty())
		text += wxS(" (") + _("style") + wxS(" \"") + to_wx(source.style) + wxS("\")");
	return text;
}

class DialogFontNameNormalization final : public wxDialog {
	agi::Context *context;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	FontNameNormalizationPlan plan;

	wxRadioBox *target_box;
	wxCheckListBox *findings;
	wxStaticText *summary;
	wxTextCtrl *details;
	wxButton *apply_button;

	void RebuildPlan();
	void UpdateDetails();
	void UpdateApplyButton();
	void SelectSafe(bool select);
	void OnCheck(wxCommandEvent& event);
	void OnApply(wxCommandEvent& event);

public:
	explicit DialogFontNameNormalization(agi::Context *c);
};

DialogFontNameNormalization::DialogFontNameNormalization(agi::Context *c)
: wxDialog(c->GetUI().parent, -1, _("Normalize Font Names"),
	  wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, context(c)
, catalog(font_family_catalog_cache::GetSnapshot())
{
	SetIcon(GETICON(font_collector_button_16));

	wxString targets[] = {
		_("Localized family names"),
		_("English Win32 family names")
	};
	target_box = new wxRadioBox(
		this, -1, _("Target names"), wxDefaultPosition, wxDefaultSize,
		2, targets, 1, wxRA_SPECIFY_ROWS);
	bool prefer_localized = true;
	try {
		prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
	}
	target_box->SetSelection(prefer_localized ? 0 : 1);

	summary = new wxStaticText(this, -1, wxString{});
	findings = new wxCheckListBox(
		this, -1, wxDefaultPosition, FromDIP(wxSize(680, 230)),
		0, nullptr, wxLB_HSCROLL | wxLB_SINGLE);
	details = new wxTextCtrl(
		this, -1, wxString{}, wxDefaultPosition, FromDIP(wxSize(680, 170)),
		wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP);

	auto select_safe = new wxButton(this, -1, _("Select &Safe"));
	auto clear = new wxButton(this, -1, _("&Clear"));
	auto selection_buttons = new wxBoxSizer(wxHORIZONTAL);
	selection_buttons->Add(select_safe, wxSizerFlags().Border(wxRIGHT));
	selection_buttons->Add(clear);

	auto button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	apply_button = button_sizer->GetAffirmativeButton();
	apply_button->SetLabel(_("&Apply Selected"));

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(target_box, wxSizerFlags().Expand().Border());
	main_sizer->Add(summary, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(findings, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
	main_sizer->Add(selection_buttons, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP));
	main_sizer->Add(new wxStaticText(this, -1, _("Details")),
		wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP));
	main_sizer->Add(details, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
	main_sizer->Add(button_sizer, wxSizerFlags().Right().Border());

	SetSizerAndFit(main_sizer);
	SetMinSize(FromDIP(wxSize(620, 560)));
	SetSize(FromDIP(wxSize(760, 650)));
	CenterOnParent();

	target_box->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { RebuildPlan(); });
	findings->Bind(wxEVT_CHECKLISTBOX, &DialogFontNameNormalization::OnCheck, this);
	findings->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateDetails(); });
	select_safe->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SelectSafe(true); });
	clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SelectSafe(false); });
	Bind(wxEVT_BUTTON, &DialogFontNameNormalization::OnApply, this, wxID_OK);

	RebuildPlan();
}

void DialogFontNameNormalization::RebuildPlan() {
	auto target = target_box->GetSelection() == 0
		? FontNameNormalizationTarget::Localized
		: FontNameNormalizationTarget::EnglishWin32;
	plan = BuildFontNameNormalizationPlan(*context->GetCore().ass, *catalog, target);

	std::size_t safe_count = 0;
	findings->Freeze();
	findings->Clear();
	for (auto const& change : plan.changes) {
		auto row = SourceText(change.source) + wxS(": ") + to_wx(change.current_name);
		if (!change.recommended_name.empty())
			row += wxS(" -> ") + to_wx(change.recommended_name);
		else
			row += wxS(" [") + _("Needs review") + wxS("]");
		auto index = findings->Append(row);
		if (change.safe_to_apply) {
			findings->Check(index, true);
			++safe_count;
		}
	}
	findings->Thaw();

	auto issue_count = plan.changes.size() - safe_count;
	auto status = wxString::Format(
		_("Scanned names: %llu    Safe changes: %llu    Issues: %llu"),
		static_cast<unsigned long long>(plan.scanned_name_count),
		static_cast<unsigned long long>(safe_count),
		static_cast<unsigned long long>(issue_count));
	if (!plan.catalog_available)
		status += wxS("\n") + _("The installed font family catalog is unavailable; changes cannot be applied.");
	summary->SetLabel(status);

	if (!plan.changes.empty())
		findings->SetSelection(0);
	UpdateDetails();
	UpdateApplyButton();
	Layout();
}

void DialogFontNameNormalization::UpdateDetails() {
	auto selection = findings->GetSelection();
	if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= plan.changes.size()) {
		details->SetValue(plan.changes.empty() ? _("No font-name findings.") : wxString{});
		return;
	}

	auto const& change = plan.changes[selection];
	wxString text;
	text += _("Source: ") + SourceText(change.source) + wxS("\n");
	text += _("Current name: ") + to_wx(change.current_name) + wxS("\n");
	text += _("Recommended name: ") +
		(change.recommended_name.empty() ? _("Not available") : to_wx(change.recommended_name)) + wxS("\n");
	text += _("Status: ") +
		(change.safe_to_apply ? _("Safe to apply") : _("Review only")) + wxS("\n");
	text += _("Match: ") + MatchKindText(change.match_kind) + wxS("\n");
	text += _("Reason: ") + ReasonText(change.reason_code) + wxS("\n");
	text += _("Rule: ") + to_wx(change.reason_code);

	if (catalog && !catalog->empty()) {
		auto resolved = catalog->Resolve(change.current_name);
		if (resolved.family) {
			if (auto const *record = catalog->Find(*resolved.family)) {
				text += wxS("\n\n") + _("Installed family information") + wxS("\n");
				text += _("Localized family name: ") + to_wx(record->localized_family_name) + wxS("\n");
				text += _("English Win32 family name: ") +
					(record->english_win32_family_name.empty()
						? _("Not available")
						: to_wx(record->english_win32_family_name));
				for (auto const& name : record->names) {
					text += wxS("\n") + NameKindText(name.kind);
					if (!name.locale.empty())
						text += wxS(" [") + to_wx(name.locale) + wxS("]");
					text += wxS(": ") + to_wx(name.value);
				}
			}
		}
	}

	details->SetValue(text);
	details->SetInsertionPoint(0);
}

void DialogFontNameNormalization::UpdateApplyButton() {
	bool has_selection = false;
	for (unsigned int i = 0; i < findings->GetCount(); ++i) {
		if (findings->IsChecked(i) && plan.changes[i].safe_to_apply) {
			has_selection = true;
			break;
		}
	}
	apply_button->Enable(plan.catalog_available && has_selection);
}

void DialogFontNameNormalization::SelectSafe(bool select) {
	for (unsigned int i = 0; i < findings->GetCount(); ++i)
		findings->Check(i, select && plan.changes[i].safe_to_apply);
	UpdateApplyButton();
}

void DialogFontNameNormalization::OnCheck(wxCommandEvent& event) {
	auto index = event.GetInt();
	if (index >= 0 && static_cast<std::size_t>(index) < plan.changes.size() &&
	    !plan.changes[index].safe_to_apply)
		findings->Check(static_cast<unsigned int>(index), false);
	UpdateDetails();
	UpdateApplyButton();
}

void DialogFontNameNormalization::OnApply(wxCommandEvent&) {
	std::vector<std::size_t> selected;
	selected.reserve(findings->GetCount());
	for (unsigned int i = 0; i < findings->GetCount(); ++i) {
		if (findings->IsChecked(i) && plan.changes[i].safe_to_apply)
			selected.push_back(i);
	}
	if (selected.empty())
		return;

	auto core = context->GetCore();
	auto result = ApplyFontNameNormalizationChanges(*core.ass, plan, selected);
	if (!result.success) {
		wxMessageBox(
			_("The subtitles changed after the scan. The plan was not applied.\n\n") +
				to_wx(result.error),
			_("Normalize Font Names"), wxOK | wxICON_ERROR | wxCENTER, this);
		RebuildPlan();
		return;
	}

	int commit_type = 0;
	if (result.styles_changed)
		commit_type |= AssFile::COMMIT_STYLES;
	if (result.dialogue_text_changed)
		commit_type |= AssFile::COMMIT_DIAG_TEXT;
	core.ass->Commit(from_wx(_("normalize font family names")), commit_type);
	EndModal(wxID_OK);
}

} // namespace

void ShowFontNameNormalizationDialog(agi::Context *c) {
	c->GetUI().dialog->ShowModal<DialogFontNameNormalization>(c);
}
