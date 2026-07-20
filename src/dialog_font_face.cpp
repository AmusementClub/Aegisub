#include "dialog_font_face.h"

#include "ass_file.h"
#include "ass_style.h"
#include "colour_button.h"
#include "compat.h"
#include "font_family_catalog.h"
#include "font_family_catalog_ui.h"
#include "font_name_combo_box.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "subs_preview.h"
#include "utils.h"
#include "wx_style_editor_ui_host.h"

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <wx/fontutil.h>
#endif

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

#ifdef _WIN32

FontFaceDialogSelection SelectionFromNativeFont(LOGFONTW const& logfont, wxWindow *parent) {
	wxFont font(wxNativeFontInfo(logfont, parent));
	if (!font.IsOk())
		return {};

	FontFaceDialogSelection result;
	result.face_name = from_wx(font.GetFaceName());
	result.point_size = font.GetPointSize();
	result.bold = font.GetWeight() == wxFONTWEIGHT_BOLD;
	result.italic = font.GetStyle() == wxFONTSTYLE_ITALIC;
	result.underline = font.GetUnderlined();
	return result;
}

struct NativeFontDialogState {
	wxWindow *parent = nullptr;
	std::function<void(FontFaceDialogSelection const&)> const *on_apply = nullptr;
};

constexpr wchar_t NativeFontDialogStateProperty[] = L"AegisubFontDialogState";
constexpr int NativeFontDialogApplyButtonId = 0x0402; // psh3 in the common dialog template

UINT_PTR CALLBACK NativeFontDialogHook(
	HWND hwnd,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	if (message == WM_INITDIALOG) {
		auto const *choose_font = reinterpret_cast<CHOOSEFONTW const *>(lparam);
		auto *state = reinterpret_cast<NativeFontDialogState *>(choose_font->lCustData);
		SetPropW(hwnd, NativeFontDialogStateProperty, state);
		wxString title = _("Select Font");
		SetWindowTextW(hwnd, title.wc_str());
		return 0;
	}

	auto *state = reinterpret_cast<NativeFontDialogState *>(
		GetPropW(hwnd, NativeFontDialogStateProperty));
	if (!state)
		return 0;

	if (message == WM_COMMAND
		&& LOWORD(wparam) == NativeFontDialogApplyButtonId
		&& HIWORD(wparam) == BN_CLICKED) {
		LOGFONTW logfont{};
		SendMessageW(hwnd, WM_CHOOSEFONT_GETLOGFONT, 0,
			reinterpret_cast<LPARAM>(&logfont));
		if (state->on_apply) {
			auto selection = SelectionFromNativeFont(logfont, state->parent);
			if (!selection.face_name.empty())
				state->on_apply->operator()(selection);
		}
		return 1;
	}

	if (message == WM_DESTROY)
		RemovePropW(hwnd, NativeFontDialogStateProperty);
	return 0;
}

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const&,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply)
{
	wxFont initial_font(
		initial.point_size,
		wxFONTFAMILY_DEFAULT,
		initial.italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
		initial.bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
		initial.underline,
		to_wx(initial.face_name));
	LOGFONTW logfont = initial_font.GetNativeFontInfo()->lf;
	NativeFontDialogState state{parent, &on_apply};

	CHOOSEFONTW choose_font{};
	choose_font.lStructSize = sizeof(choose_font);
	choose_font.hwndOwner = parent ? parent->GetHWND() : nullptr;
	choose_font.lpLogFont = &logfont;
	choose_font.lCustData = reinterpret_cast<LPARAM>(&state);
	choose_font.lpfnHook = NativeFontDialogHook;
	choose_font.Flags = CF_SCREENFONTS
		| CF_INITTOLOGFONTSTRUCT
		| CF_EFFECTS
		| CF_APPLY
		| CF_ENABLEHOOK;

	if (!ChooseFontW(&choose_font))
		return std::nullopt;

	auto result = SelectionFromNativeFont(logfont, parent);
	if (result.face_name.empty())
		return std::nullopt;
	return result;
}

#else

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply);

#endif

class FontFaceDialog final : public wxDialog {
	FontFamilyCatalogUiModel const& font_model;
	std::function<void(FontFaceDialogSelection const&)> on_apply;
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
		if (auto *apply = FindWindow(wxID_APPLY))
			apply->Enable(!value.empty());
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

	void OnApply(wxCommandEvent &) {
		if (on_apply)
			on_apply(GetSelection());
	}

public:
	FontFaceDialog(
		wxWindow *parent,
		agi::Context *context,
		FontFaceDialogSelection const& initial,
		FontFamilyCatalogUiModel const& font_model,
		std::function<void(FontFaceDialogSelection const&)> on_apply)
	: wxDialog(parent, -1, _("Select Font"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, font_model(font_model)
	, on_apply(std::move(on_apply))
	{
		auto const contains_matching = OPT_GET("Subtitle/Font/Use Contains Matching")->GetBool();
		face_name = new FontNameComboBox(
			this, to_wx(initial.face_name), wxSize(400, -1),
			font_model.choices, contains_matching);
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
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxAPPLY),
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
		Bind(wxEVT_BUTTON, &FontFaceDialog::OnApply, this, wxID_APPLY);

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

#ifndef _WIN32

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply)
{
	FontFaceDialog dialog(parent, nullptr, initial, font_model,
		[&on_apply](FontFaceDialogSelection const& selection) {
			if (on_apply)
				on_apply(selection);
		});
	if (dialog.ShowModal() != wxID_OK)
		return std::nullopt;
	return dialog.GetSelection();
}

#endif

} // namespace

std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	agi::Context *context,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> on_apply)
{
	if (font_model.prefer_localized || !font_model.catalog || font_model.catalog->empty())
		return ShowNativeFontFaceDialog(parent, initial, font_model, on_apply);

	FontFaceDialog dialog(parent, context, initial, font_model, std::move(on_apply));
	auto const result = dialog.ShowModal();
	OPT_SET("Tool/Style Editor/Preview Text")->SetString(dialog.GetPreviewText());
	if (result != wxID_OK)
		return std::nullopt;
	return dialog.GetSelection();
}
