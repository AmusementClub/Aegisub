#include "dialog_font_face.h"

#include "ass_file.h"
#include "ass_style.h"
#include "colour_button.h"
#include "compat.h"
#include "font_family_catalog.h"
#include "font_family_catalog_ui.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "subs_preview.h"
#include "utils.h"
#include "wx_style_editor_ui_host.h"

#include <algorithm>
#include <set>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/fontdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

wxString MatchName(FontFamilyMatchKind match) {
	switch (match) {
		case FontFamilyMatchKind::Exact: return _("Exact");
		case FontFamilyMatchKind::CaseInsensitiveExact: return _("Case-insensitive exact");
		case FontFamilyMatchKind::Ambiguous: return _("Ambiguous");
		case FontFamilyMatchKind::None: return _("Not found");
	}
	return _("Not found");
}

wxString NameKind(FontFamilyNameKind kind) {
	switch (kind) {
		case FontFamilyNameKind::Win32Family: return _("Win32 family");
		case FontFamilyNameKind::TypographicFamily: return _("Typographic family");
		case FontFamilyNameKind::FullName: return _("Full name");
		case FontFamilyNameKind::PostScript: return _("PostScript name");
		case FontFamilyNameKind::PlatformAlias: return _("Platform alias");
	}
	return _("Other name");
}

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial)
{
	wxFont initial_font(
		initial.point_size,
		wxFONTFAMILY_DEFAULT,
		initial.italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
		initial.bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
		initial.underline,
		to_wx(initial.face_name));
	auto font = wxGetFontFromUser(parent, initial_font, _("Select Font"));
	if (!font.IsOk())
		return std::nullopt;

	FontFaceDialogSelection result;
	result.face_name = from_wx(font.GetFaceName());
	result.point_size = font.GetPointSize();
	result.bold = font.GetWeight() == wxFONTWEIGHT_BOLD;
	result.italic = font.GetStyle() == wxFONTSTYLE_ITALIC;
	result.underline = font.GetUnderlined();
	return result;
}

class FontFaceDialog final : public wxDialog {
	FontFamilyCatalogUiModel const& font_model;
	wxComboBox *face_name;
	wxSpinCtrl *point_size;
	wxCheckBox *bold;
	wxCheckBox *italic;
	wxCheckBox *underline;
	SubtitlesPreview *preview;
	wxTextCtrl *preview_text;
	wxTextCtrl *font_information;
	AssStyle preview_style;

	std::string WithSelectedVerticalPrefix(std::string_view name) const {
		bool const vertical = !FontFamilyCatalog::SplitVerticalPrefix(
			from_wx(face_name->GetValue())).first.empty();
		auto bare = FontFamilyCatalog::SplitVerticalPrefix(name).second;
		return FontFamilyCatalog::JoinVerticalPrefix(vertical, bare);
	}

	wxString BuildFontInformation() const {
		wxString text;
		auto append = [&](wxString const& label, wxString const& value) {
			text += label;
			text += wxS(": ");
			text += value;
			text += wxS("\n");
		};

		auto const selected_wx = face_name->GetValue();
		auto const selected = from_wx(selected_wx);
		append(_("Selected ASS name"), selected_wx);

		if (!font_model.catalog || font_model.catalog->empty()) {
			append(_("Catalog match"), _("Font catalog unavailable"));
			return text;
		}

		auto const resolved = font_model.catalog->Resolve(selected);
		append(_("Catalog match"), MatchName(resolved.match));
		if (!resolved.family)
			return text;

		auto const *record = font_model.catalog->Find(*resolved.family);
		if (!record)
			return text;

		append(_("Localized family name"),
			to_wx(WithSelectedVerticalPrefix(record->localized_family_name)));
		append(_("English Win32 family name"),
			record->english_win32_family_name.empty()
				? _("Not available")
				: to_wx(WithSelectedVerticalPrefix(record->english_win32_family_name)));

		if (!record->names.empty()) {
			text += _("Known font names");
			text += wxS(":\n");
			std::set<std::pair<std::string, std::string>> seen;
			for (auto const& name : record->names) {
				auto display_name = WithSelectedVerticalPrefix(name.value);
				if (!seen.emplace(display_name, name.locale).second)
					continue;
				text += wxS("  ");
				text += to_wx(display_name);
				text += wxS(" — ");
				text += NameKind(name.kind);
				if (!name.locale.empty()) {
					text += wxS(" [");
					text += to_wx(name.locale);
					text += wxS("]");
				}
				text += wxS("\n");
			}
		}
		return text;
	}

	void UpdateInformation() {
		font_information->ChangeValue(BuildFontInformation());

		auto value = face_name->GetValue();
		value.Trim(true).Trim(false);
		if (auto *ok = FindWindow(wxID_OK))
			ok->Enable(!value.empty());
	}

	void UpdatePreview() {
		preview_style.font = from_wx(face_name->GetValue());
		preview_style.fontsize = point_size->GetValue();
		preview_style.bold = bold->GetValue();
		preview_style.italic = italic->GetValue();
		preview_style.underline = underline->GetValue();
		preview->SetStyle(preview_style);
	}

	void OnFaceText(wxCommandEvent &event) {
		UpdateInformation();
		event.Skip();
	}

	void OnFaceUpdate(wxCommandEvent &event) {
		UpdateInformation();
		UpdatePreview();
		event.Skip();
	}

	void OnFaceKillFocus(wxFocusEvent &event) {
		UpdatePreview();
		event.Skip();
	}

	void OnStyleUpdate(wxCommandEvent &event) {
		UpdatePreview();
		event.Skip();
	}

	void OnSizeUpdate(wxSpinEvent &event) {
		UpdatePreview();
		event.Skip();
	}

	void OnPreviewText(wxCommandEvent &event) {
		preview->SetText(from_wx(preview_text->GetValue()));
		event.Skip();
	}

	void OnPreviewColour(ValueEvent<agi::Color> &event) {
		preview->SetColour(event.Get());
		OPT_SET("Colour/Style Editor/Background/Preview")->SetColor(event.Get());
	}

	void OnCopyInformation(wxCommandEvent &) {
		SetClipboard(from_wx(font_information->GetValue()));
	}

public:
	FontFaceDialog(
		wxWindow *parent,
		agi::Context *context,
		FontFaceDialogSelection const& initial,
		FontFamilyCatalogUiModel const& font_model)
	: wxDialog(parent, -1, _("Select Font"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, font_model(font_model)
	{
		face_name = new wxComboBox(
			this, -1, to_wx(initial.face_name), wxDefaultPosition, wxSize(400, -1),
			font_model.choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
		face_name->SetToolTip(_("Font face; this exact name will be written to ASS"));
		point_size = new wxSpinCtrl(
			this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
			wxSP_ARROW_KEYS, 0, 10000, std::clamp(initial.point_size, 0, 10000));
		point_size->SetToolTip(_("Font size"));
		bold = new wxCheckBox(this, -1, _("&Bold"));
		italic = new wxCheckBox(this, -1, _("&Italic"));
		underline = new wxCheckBox(this, -1, _("&Underline"));
		bold->SetValue(initial.bold);
		italic->SetValue(initial.italic);
		underline->SetValue(initial.underline);

		auto *font_top = new wxBoxSizer(wxHORIZONTAL);
		font_top->Add(face_name, wxSizerFlags(1).Expand());
		font_top->Add(point_size, wxSizerFlags().Border(wxLEFT, 5));
		auto *font_bottom = new wxBoxSizer(wxHORIZONTAL);
		font_bottom->AddStretchSpacer();
		font_bottom->Add(bold);
		font_bottom->Add(italic, wxSizerFlags().Border(wxLEFT, 5));
		font_bottom->Add(underline, wxSizerFlags().Border(wxLEFT, 5));
		font_bottom->AddStretchSpacer();
		auto *font_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Font"));
		font_box->Add(font_top, wxSizerFlags().Expand());
		font_box->Add(font_bottom, wxSizerFlags().Expand().Border(wxTOP, 5));

		std::shared_ptr<const TransientFontSet> transient_fonts;
		if (context)
			transient_fonts = context->GetCore().ass->GetTransientFonts();
		preview = new SubtitlesPreview(
			this,
			wxSize(520, 100),
			wxSUNKEN_BORDER,
			OPT_GET("Colour/Style Editor/Background/Preview")->GetColor(),
			std::move(transient_fonts),
			agi::ResolveStyleEditorNotificationSink(context, this));
		preview_text = new wxTextCtrl(
			this, -1, to_wx(OPT_GET("Tool/Style Editor/Preview Text")->GetString()));
		auto *preview_colour = new ColourButton(
			this, wxSize(45, 16), false,
			OPT_GET("Colour/Style Editor/Background/Preview")->GetColor());
		auto *preview_bottom = new wxBoxSizer(wxHORIZONTAL);
		preview_bottom->Add(preview_text, wxSizerFlags(1).Expand().Border(wxRIGHT, 5));
		preview_bottom->Add(preview_colour, wxSizerFlags().Expand());
		auto *preview_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Preview"));
		preview_box->Add(preview, wxSizerFlags(1).Expand().Border(wxBOTTOM, 5));
		preview_box->Add(preview_bottom, wxSizerFlags().Expand());

		font_information = new wxTextCtrl(
			this, -1, wxEmptyString, wxDefaultPosition, wxSize(-1, 130),
			wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
		font_information->SetToolTip(_("Select text and copy it, or use Copy All"));
		auto *copy_information = new wxButton(this, -1, _("Copy All"));
		auto *information_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Selected Font Information"));
		information_box->Add(font_information, wxSizerFlags(1).Expand());
		information_box->Add(copy_information, wxSizerFlags().Right().Border(wxTOP, 5));

		auto *main_sizer = new wxBoxSizer(wxVERTICAL);
		main_sizer->Add(font_box, wxSizerFlags().Expand().Border(wxALL, 10));
		main_sizer->Add(preview_box, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
		main_sizer->Add(information_box, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
		SetSizerAndFit(main_sizer);
		SetMinSize(GetSize());
		CentreOnParent();

		face_name->Bind(wxEVT_TEXT, &FontFaceDialog::OnFaceText, this);
		face_name->Bind(wxEVT_COMBOBOX, &FontFaceDialog::OnFaceUpdate, this);
		face_name->Bind(wxEVT_TEXT_ENTER, &FontFaceDialog::OnFaceUpdate, this);
		face_name->Bind(wxEVT_KILL_FOCUS, &FontFaceDialog::OnFaceKillFocus, this);
		point_size->Bind(wxEVT_SPINCTRL, &FontFaceDialog::OnSizeUpdate, this);
		bold->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		italic->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		underline->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		preview_text->Bind(wxEVT_TEXT, &FontFaceDialog::OnPreviewText, this);
		preview_colour->Bind(EVT_COLOR, &FontFaceDialog::OnPreviewColour, this);
		copy_information->Bind(wxEVT_BUTTON, &FontFaceDialog::OnCopyInformation, this);

		preview->SetText(from_wx(preview_text->GetValue()));
		UpdateInformation();
		UpdatePreview();
	}

	FontFaceDialogSelection GetSelection() const {
		FontFaceDialogSelection result;
		result.face_name = from_wx(face_name->GetValue());
		result.point_size = point_size->GetValue();
		result.bold = bold->GetValue();
		result.italic = italic->GetValue();
		result.underline = underline->GetValue();
		return result;
	}

	std::string GetPreviewText() const {
		return from_wx(preview_text->GetValue());
	}
};

} // namespace

std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	agi::Context *context,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model)
{
	if (font_model.prefer_localized || !font_model.catalog || font_model.catalog->empty())
		return ShowNativeFontFaceDialog(parent, initial);

	FontFaceDialog dialog(parent, context, initial, font_model);
	auto const result = dialog.ShowModal();
	OPT_SET("Tool/Style Editor/Preview Text")->SetString(dialog.GetPreviewText());
	if (result != wxID_OK)
		return std::nullopt;
	return dialog.GetSelection();
}
