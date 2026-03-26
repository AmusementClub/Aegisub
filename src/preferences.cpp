// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
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

/// @file preferences.cpp
/// @brief Preferences dialogue
/// @ingroup configuration_ui

#include "preferences.h"

#include "ass_style_storage.h"
#include "audio_provider_factory.h"
#include "audio_renderer_waveform.h"
#include "command/command.h"
#include "compat.h"
#include "help_button.h"
#include "hotkey_data_view_model.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/subtitles_provider.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "persist_location.h"
#include "preferences_base.h"
#include "video_provider_manager.h"
#include "wx_ui_services.h"

#ifdef WITH_PORTAUDIO
#include "audio_player_portaudio.h"
#endif

#include <libaegisub/hotkey.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/event.h>
#include <wx/listctrl.h>
#include <wx/propgrid/advprops.h>
#include <wx/propgrid/propgrid.h>
#include <wx/settings.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>
#include <wx/treebook.h>

namespace {
wxColour BlendColour(wxColour const& base, wxColour const& accent, int accent_percent) {
	int const base_percent = 100 - accent_percent;
	return wxColour(
		(base.Red() * base_percent + accent.Red() * accent_percent) / 100,
		(base.Green() * base_percent + accent.Green() * accent_percent) / 100,
		(base.Blue() * base_percent + accent.Blue() * accent_percent) / 100);
}

class PropertyGridOptionBinder {
	Preferences *prefs;
	wxPropertyGrid *grid;
	std::unordered_map<wxPGProperty *, std::function<void(wxVariant const&)>> updaters;

	template<typename OptionValue, typename Value>
	void QueueOptionChange(std::string const& name, Value value) {
		prefs->SetOption(agi::make_unique<OptionValue>(name, std::move(value)));
	}

	wxPGChoices MakeChoices(wxArrayString const& choices) const {
		wxPGChoices pg_choices;
		for (unsigned i = 0; i < choices.size(); ++i)
			pg_choices.Add(choices[i], i);
		return pg_choices;
	}

	int ClampChoiceSelection(int selected, size_t count) const {
		return count ? std::clamp<int>(selected, 0, static_cast<int>(count) - 1) : 0;
	}

	void ApplyTheme(wxWindow *page) {
		auto const window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
		auto const window_text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
		auto const button = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
		auto const button_text = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
		auto const highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
		auto const highlight_text = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
		auto const gray_text = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

		grid->SetBackgroundColour(window);
		grid->SetEmptySpaceColour(window);
		grid->SetCellBackgroundColour(window);
		grid->SetCellTextColour(window_text);
		grid->SetCellDisabledTextColour(gray_text);
		grid->SetMarginColour(BlendColour(button, window, 35));
		grid->SetLineColour(BlendColour(button, window_text, 12));
		grid->SetCaptionBackgroundColour(BlendColour(button, highlight, 10));
		grid->SetCaptionTextColour(button_text);
		grid->SetSelectionBackgroundColour(highlight);
		grid->SetSelectionTextColour(highlight_text);
		grid->SetVerticalSpacing(page->FromDIP(2));
	}

public:
	explicit PropertyGridOptionBinder(OptionPage *page)
	: prefs(page->parent)
	{
		static bool editors_registered = false;
		if (!editors_registered) {
			wxPropertyGrid::RegisterAdditionalEditors();
			editors_registered = true;
		}

		grid = new wxPropertyGrid(
			page,
			wxID_ANY,
			wxDefaultPosition,
			wxDefaultSize,
			wxPG_BOLD_MODIFIED | wxPG_SPLITTER_AUTO_CENTER | wxPG_TOOLTIPS);
		grid->SetExtraStyle(wxPG_EX_HELP_AS_TOOLTIPS);
		grid->SetMinSize(page->FromDIP(wxSize(520, 360)));
		ApplyTheme(page);
	}

	void BindEvents(std::shared_ptr<PropertyGridOptionBinder> self) {
		grid->Bind(wxEVT_PG_CHANGED, [self = std::move(self)](wxPropertyGridEvent& evt) {
			auto it = self->updaters.find(evt.GetProperty());
			if (it != self->updaters.end())
				it->second(evt.GetPropertyValue());
			evt.Skip();
		});
	}

	wxPropertyGrid *GetGrid() const { return grid; }

	wxPGProperty *AddCategory(wxString const& label) {
		return grid->Append(new wxPropertyCategory(label));
	}

	wxPGProperty *AddBool(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxBoolProperty(label, opt_name, opt->GetBool()));
		prop->SetAttribute(wxPG_BOOL_USE_CHECKBOX, true);
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueBool>(name, value.GetBool());
		});
		return prop;
	}

	wxPGProperty *AddInt(wxString const& label, const char *opt_name, int min, int max) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxIntProperty(label, opt_name, opt->GetInt()));
		prop->SetAttribute(wxPG_ATTR_MIN, static_cast<long>(min));
		prop->SetAttribute(wxPG_ATTR_MAX, static_cast<long>(max));
		prop->SetAttribute(wxPG_ATTR_SPINCTRL_STEP, 1L);
		prop->SetEditor("SpinCtrl");
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
		});
		return prop;
	}

	wxPGProperty *AddDouble(wxString const& label, const char *opt_name, double min, double max, double step, int precision = 2) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxFloatProperty(label, opt_name, opt->GetDouble()));
		prop->SetAttribute(wxPG_ATTR_MIN, min);
		prop->SetAttribute(wxPG_ATTR_MAX, max);
		prop->SetAttribute(wxPG_ATTR_SPINCTRL_STEP, step);
		prop->SetAttribute(wxPG_FLOAT_PRECISION, precision);
		prop->SetEditor("SpinCtrl");
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueDouble>(name, value.GetDouble());
		});
		return prop;
	}

	wxPGProperty *AddString(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxStringProperty(label, opt_name, to_wx(opt->GetString())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddFont(wxString const& label, std::string const& opt_prefix) {
		auto const face_name = opt_prefix + "Font Face";
		auto const font_size = opt_prefix + "Font Size";
		prefs->AddChangeableOption(face_name);
		prefs->AddChangeableOption(font_size);

		wxFont font;
		auto const face_opt = OPT_GET(face_name);
		auto const size_opt = OPT_GET(font_size);
		if (!face_opt->GetString().empty())
			font.SetFaceName(to_wx(face_opt->GetString()));
		if (size_opt->GetInt() > 0)
			font.SetPointSize(static_cast<int>(size_opt->GetInt()));

		auto *prop = grid->Append(new wxFontProperty(label, opt_prefix, font));
		updaters.emplace(prop, [this, face_name, font_size](wxVariant const& value) {
			wxFont font;
			font << value;
			QueueOptionChange<agi::OptionValueString>(face_name, from_wx(font.GetFaceName()));
			QueueOptionChange<agi::OptionValueInt>(font_size, font.GetPointSize());
		});
		return prop;
	}

	wxPGProperty *AddDirectory(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxDirProperty(label, opt_name, to_wx(opt->GetString())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddColour(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxColourProperty(label, opt_name, to_wx(opt->GetColor())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			wxColourPropertyValue colour;
			colour << value;
			QueueOptionChange<agi::OptionValueColor>(name, from_wx(colour.m_colour));
		});
		return prop;
	}

	wxPGProperty *AddFile(wxString const& label, const char *opt_name, wxString const& wildcard) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxFileProperty(label, opt_name, to_wx(opt->GetString())));
		prop->SetAttribute(wxPG_FILE_WILDCARD, wildcard);
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddChoice(wxString const& label, wxArrayString const& choices, const char *opt_name) {
		auto opt = OPT_GET(opt_name);
		if (opt->GetType() == agi::OptionType::String) {
			std::vector<std::pair<std::string, std::string>> mapped_choices;
			mapped_choices.reserve(choices.size());
			for (auto const& choice : choices)
				mapped_choices.emplace_back(from_wx(choice), from_wx(choice));
			return AddChoice(label, mapped_choices, opt_name);
		}

		prefs->AddChangeableOption(opt_name);
		int const selected = ClampChoiceSelection(opt->GetInt(), choices.size());
		auto pg_choices = MakeChoices(choices);
		auto *prop = grid->Append(new wxEnumProperty(label, opt_name, pg_choices, selected));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
		});
		return prop;
	}

	wxPGProperty *AddChoice(wxString const& label, std::vector<std::pair<std::string, std::string>> const& choices, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		wxPGChoices pg_choices;
		int selected = 0;

		for (unsigned i = 0; i < choices.size(); ++i) {
			pg_choices.Add(to_wx(choices[i].first), i);
			if (opt->GetType() == agi::OptionType::String && choices[i].second == opt->GetString())
				selected = i;
		}

		if (opt->GetType() == agi::OptionType::Int)
			selected = ClampChoiceSelection(opt->GetInt(), choices.size());

		auto *prop = grid->Append(new wxEnumProperty(label, opt_name, pg_choices, selected));
		if (opt->GetType() == agi::OptionType::Int) {
			std::string name = opt_name;
			updaters.emplace(prop, [this, name](wxVariant const& value) {
				QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
			});
			return prop;
		}

		std::string name = opt_name;
		std::vector<std::string> values;
		values.reserve(choices.size());
		for (auto const& choice : choices)
			values.push_back(choice.second);
		updaters.emplace(prop, [this, name, values = std::move(values)](wxVariant const& value) {
			int const index = static_cast<int>(value.GetLong());
			if (index < 0 || index >= static_cast<int>(values.size()))
				return;
			QueueOptionChange<agi::OptionValueString>(name, values[index]);
		});
		return prop;
	}
};

/// General preferences page
void BuildGeneralPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));
	p->OptionAdd(general, _("Check for updates on startup"), "App/Auto/Check For Updates");
	p->OptionAdd(general, _("Show main toolbar"), "App/Show Toolbar");
	p->OptionAdd(general, _("Save UI state in subtitles files"), "App/Save UI State");
	p->CellSkip(general);

#ifndef __WXMSW__
	p->OptionAdd(general, _("Toolbar Icon Size"), "App/Toolbar Icon Size");
#endif
	wxString autoload_modes[] = { _("Never"), _("Always"), _("Ask") };
	wxArrayString autoload_modes_arr(3, autoload_modes);
	p->OptionChoice(general, _("Automatically load linked files"), autoload_modes_arr, "App/Auto/Load Linked Files");
	p->OptionAdd(general, _("Undo Levels"), "Limits/Undo Levels", 2, 10000);

	auto recent = p->PageSizer(_("Recently Used Lists"));
	p->OptionAdd(recent, _("Files"), "Limits/MRU", 0, 16);
	p->OptionAdd(recent, _("Find/Replace"), "Limits/Find Replace");

	p->SetSizerAndFit(p->sizer);
}

void BuildGeneralDefaultStylesPage(OptionPage *p) {
	auto staticbox = new wxStaticBoxSizer(wxVERTICAL, p, _("Default style catalogs"));
	p->sizer->Add(staticbox, 0, wxEXPAND, 5);
	p->sizer->AddSpacer(8);

	auto instructions = new wxStaticText(p, wxID_ANY, _("The chosen style catalogs will be loaded when you start a new file or import files in the various formats.\n\nYou can set up style catalogs in the Style Manager."));
	p->sizer->Fit(p);
	instructions->Wrap(400);
	staticbox->Add(instructions, 0, wxALL, 5);
	staticbox->AddSpacer(16);
	
	auto general = new wxFlexGridSizer(2, 5, 5);
	general->AddGrowableCol(0, 1);
	staticbox->Add(general, 1, wxEXPAND, 5);

	// Build a list of available style catalogs, and wished-available ones
	auto const& avail_catalogs = AssStyleStorage::GetCatalogs();
	std::unordered_set<std::string> catalogs_set(begin(avail_catalogs), end(avail_catalogs));
	// Always include one named "Default" even if it doesn't exist (ensure there is at least one on the list)
	catalogs_set.insert("Default");
	// Include all catalogs named in the existing configuration
	static const char *formats[] = { "ASS", "MicroDVD", "SRT", "TTXT", "TXT" };
	for (auto formatname : formats)
		catalogs_set.insert(OPT_GET("Subtitle Format/" + std::string(formatname) + "/Default Style Catalog")->GetString());
	// Sorted version
	wxArrayString catalogs;
	for (auto const& cn : catalogs_set)
		catalogs.Add(to_wx(cn));
	catalogs.Sort();

	p->OptionChoice(general, _("New files"), catalogs, "Subtitle Format/ASS/Default Style Catalog");
	p->OptionChoice(general, _("MicroDVD import"), catalogs, "Subtitle Format/MicroDVD/Default Style Catalog");
	p->OptionChoice(general, _("SRT import"), catalogs, "Subtitle Format/SRT/Default Style Catalog");
	p->OptionChoice(general, _("TTXT import"), catalogs, "Subtitle Format/TTXT/Default Style Catalog");
	p->OptionChoice(general, _("Plain text import"), catalogs, "Subtitle Format/TXT/Default Style Catalog");

	p->SetSizerAndFit(p->sizer);
}

/// Audio preferences page
void BuildAudioPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Options"));
	binder->AddBool(_("Default mouse wheel to zoom"), "Audio/Wheel Default to Zoom");
	binder->AddBool(_("Lock scroll on cursor"), "Audio/Lock Scroll on Cursor");
	binder->AddBool(_("Snap markers by default"), "Audio/Snap/Enable");
	binder->AddBool(_("Auto-focus on mouse over"), "Audio/Auto/Focus");
	binder->AddBool(_("Play audio when stepping in video"), "Audio/Plays When Stepping Video");
	binder->AddBool(_("Left-click-drag moves end marker"), "Audio/Drag Timing");
	binder->AddInt(_("Default timing length (ms)"), "Timing/Default Duration", 0, 36000);
	binder->AddInt(_("Default lead-in length (ms)"), "Audio/Lead/IN", 0, 36000);
	binder->AddInt(_("Default lead-out length (ms)"), "Audio/Lead/OUT", 0, 36000);

	binder->AddInt(_("Marker drag-start sensitivity (px)"), "Audio/Start Drag Sensitivity", 1, 15);
	binder->AddInt(_("Line boundary thickness (px)"), "Audio/Line Boundaries Thickness", 1, 5);
	binder->AddInt(_("Maximum snap distance (px)"), "Audio/Snap/Distance", 0, 25);

	const wxString dtl_arr[] = { _("Don't show"), _("Show previous"), _("Show previous and next"), _("Show all") };
	wxArrayString choice_dtl(4, dtl_arr);
	binder->AddChoice(_("Show inactive lines"), choice_dtl, "Audio/Inactive Lines Display Mode");
	binder->AddBool(_("Include commented inactive lines"), "Audio/Display/Draw/Inactive Comments");

	binder->AddCategory(_("Display Visual Options"));
	binder->AddBool(_("Keyframes in dialogue mode"), "Audio/Display/Draw/Keyframes in Dialogue Mode");
	binder->AddBool(_("Keyframes in karaoke mode"), "Audio/Display/Draw/Keyframes in Karaoke Mode");
	binder->AddBool(_("Cursor time"), "Audio/Display/Draw/Cursor Time");
	binder->AddBool(_("Video position"), "Audio/Display/Draw/Video Position");
	binder->AddBool(_("Seconds boundaries"), "Audio/Display/Draw/Seconds");
	binder->AddBool(_("Debug metrics"), "Audio/Display/Draw/Debug Metrics");
	binder->AddChoice(_("Waveform Style"), AudioWaveformRenderer::GetWaveformStyles(), "Audio/Display/Waveform Style");

	const wxString sq_arr[4] = { _("Regular quality"), _("Better quality"), _("High quality"), _("Insane quality") };
	wxArrayString sq_choice(4, sq_arr);
	binder->AddChoice(_("Spectrum Quality"), sq_choice, "Audio/Renderer/Spectrum/Quality");

	const wxString sm_arr[2] = { _("Legacy linear"), _("Frequency curve") };
	wxArrayString sm_choice(2, sm_arr);
	binder->AddChoice(_("Spectrum Computation Mode"), sm_choice, "Audio/Renderer/Spectrum/Computation Mode");

	const wxString smm_arr[3] = {
		_("Time-domain downmix"),
		_("Strongest channel per frequency bin"),
		_("Average channel energy per frequency bin")
	};
	wxArrayString smm_choice(3, smm_arr);
	binder->AddChoice(_("Spectrum mono mix method"), smm_choice, "Audio/Renderer/Spectrum/Mono Mix Mode");

	const wxString sc_arr[5] = { _("Linear"), _("Extended"), _("Medium"), _("Compressed"), _("Logarithmic") };
	wxArrayString sc_choice(5, sc_arr);
	binder->AddChoice(_("Spectrum Frequency Mapping"), sc_choice, "Audio/Renderer/Spectrum/FreqCurve");

	binder->AddCategory(_("Audio labels"));
	binder->AddFont(_("Font"), "Audio/Karaoke/");

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Video preferences page
void BuildVideoPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Options"));
	binder->AddBool(_("Show keyframes in slider"), "Video/Slider/Show Keyframes");
	binder->AddBool(_("Only show visual tools when mouse is over video"), "Tool/Visual/Autohide");
	binder->AddBool(_("Seek video to line start on selection change"), "Video/Subtitle Sync");
	binder->AddBool(_("Automatically open audio when opening video"), "Video/Open Audio");

	const wxString czoom_arr[24] = { "12.5%", "25%", "37.5%", "50%", "62.5%", "75%", "87.5%", "100%", "112.5%", "125%", "137.5%", "150%", "162.5%", "175%", "187.5%", "200%", "212.5%", "225%", "237.5%", "250%", "262.5%", "275%", "287.5%", "300%" };
	wxArrayString choice_zoom(24, czoom_arr);
	binder->AddChoice(_("Default Zoom"), choice_zoom, "Video/Default Zoom");

	binder->AddInt(_("Fast jump step in frames"), "Video/Slider/Fast Jump Step", 0, INT_MAX);

	const wxString cscr_arr[3] = { "?video", "?script", "." };
	wxArrayString scr_res(3, cscr_arr);
	binder->AddChoice(_("Screenshot save path"), scr_res, "Path/Screenshot");

	binder->AddCategory(_("Script Resolution"));
	auto *auto_prop = binder->AddBool(_("Use resolution of first video opened"), "Subtitle/Default Resolution/Auto");
	auto *width_prop = binder->AddInt(_("Default width"), "Subtitle/Default Resolution/Width", 0, INT_MAX);
	auto *height_prop = binder->AddInt(_("Default height"), "Subtitle/Default Resolution/Height", 0, INT_MAX);
	binder->AddBool(_("Prefer PlayRes over LayoutRes"), "Subtitle/Resolution/Prefer PlayRes");
	auto update_resolution_enable = [grid, width_prop, height_prop]() {
		bool const auto_enabled = OPT_GET("Subtitle/Default Resolution/Auto")->GetBool();
		grid->EnableProperty(width_prop, !auto_enabled);
		grid->EnableProperty(height_prop, !auto_enabled);
	};
	update_resolution_enable();
	grid->Bind(wxEVT_PG_CHANGED, [auto_prop, update_resolution_enable](wxPropertyGridEvent& evt) {
		if (evt.GetProperty() == auto_prop)
			update_resolution_enable();
		evt.Skip();
	});

	const wxString cres_arr[] = {_("Never"), _("Ask"), _("Always set"), _("Always resample")};
	wxArrayString choice_res(4, cres_arr);
	binder->AddChoice(_("Match video resolution on open"), choice_res, "Video/Script Resolution Mismatch");

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Interface preferences page
void BuildInterfacePage(OptionPage *p) {
	auto edit_box = p->PageSizer(_("Edit Box"));
#ifdef WITH_WXSTC
	p->OptionAdd(edit_box, _("Use styled edit box"), "Subtitle/Use STC");
	p->OptionAdd(edit_box, _("Enable call tips"), "App/Call Tips");
#endif
	p->OptionAdd(edit_box, _("Overwrite in time boxes"), "Subtitle/Time Edit/Insert Mode");
#ifdef WITH_WXSTC
	p->OptionAdd(edit_box, _("Enable syntax highlighting"), "Subtitle/Highlight/Syntax");
#else
	// Pad number of options to even
	p->CellSkip(edit_box);
#endif
	p->OptionBrowse(edit_box, _("Dictionaries path"), "Path/Dictionary");
	p->OptionFont(edit_box, "Subtitle/Edit Box/");

	auto character_count = p->PageSizer(_("Character Counter"));
	p->OptionAdd(character_count, _("Maximum characters per line"), "Subtitle/Character Limit", 0, 1000);
	p->OptionAdd(character_count, _("Characters Per Second Warning Threshold"), "Subtitle/Character Counter/CPS Warning Threshold", 0, 1000);
	p->OptionAdd(character_count, _("Characters Per Second Error Threshold"), "Subtitle/Character Counter/CPS Error Threshold", 0, 1000);
	p->OptionAdd(character_count, _("Ignore whitespace"), "Subtitle/Character Counter/Ignore Whitespace");
	p->OptionAdd(character_count, _("Ignore punctuation"), "Subtitle/Character Counter/Ignore Punctuation");

	auto grid = p->PageSizer(_("Grid"));
	p->OptionAdd(grid, _("Focus grid on click"), "Subtitle/Grid/Focus Allow");
	p->OptionAdd(grid, _("Highlight visible subtitles"), "Subtitle/Grid/Highlight Subtitles in Frame");
	p->OptionAdd(grid, _("Hide overrides symbol"), "Subtitle/Grid/Hide Overrides Char");
	p->OptionFont(grid, "Subtitle/Grid/");

	auto tl_assistant = p->PageSizer(_("Translation Assistant"));
	p->OptionAdd(tl_assistant, _("Skip over whitespace"), "Tool/Translation Assistant/Skip Whitespace");

	p->SetSizerAndFit(p->sizer);
}

/// Interface Colours preferences subpage
void BuildInterfaceColoursPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Audio Display"));
	binder->AddColour(_("Play cursor"), "Colour/Audio Display/Play Cursor");
	binder->AddColour(_("Line boundary start"), "Colour/Audio Display/Line boundary Start");
	binder->AddColour(_("Line boundary end"), "Colour/Audio Display/Line boundary End");
	binder->AddColour(_("Line boundary inactive line"), "Colour/Audio Display/Line Boundary Inactive Line");
	binder->AddColour(_("Syllable boundaries"), "Colour/Audio Display/Syllable Boundaries");
	binder->AddColour(_("Seconds boundaries"), "Colour/Audio Display/Seconds Line");

	binder->AddCategory(_("Syntax Highlighting"));
	binder->AddColour(_("Background"), "Colour/Subtitle/Background");
	binder->AddColour(_("Normal"), "Colour/Subtitle/Syntax/Normal");
#ifdef WITH_WXSTC
	binder->AddColour(_("Comments"), "Colour/Subtitle/Syntax/Comment");
	binder->AddColour(_("Drawings"), "Colour/Subtitle/Syntax/Drawing");
	binder->AddColour(_("Brackets"), "Colour/Subtitle/Syntax/Brackets");
	binder->AddColour(_("Slashes and Parentheses"), "Colour/Subtitle/Syntax/Slashes");
	binder->AddColour(_("Tags"), "Colour/Subtitle/Syntax/Tags");
	binder->AddColour(_("Parameters"), "Colour/Subtitle/Syntax/Parameters");
	binder->AddColour(_("Error"), "Colour/Subtitle/Syntax/Error");
	binder->AddColour(_("Error Background"), "Colour/Subtitle/Syntax/Background/Error");
	binder->AddColour(_("Line Break"), "Colour/Subtitle/Syntax/Line Break");
	binder->AddColour(_("Karaoke templates"), "Colour/Subtitle/Syntax/Karaoke Template");
	binder->AddColour(_("Karaoke variables"), "Colour/Subtitle/Syntax/Karaoke Variable");
#endif

	binder->AddCategory(_("Audio Color Schemes"));
	wxArrayString schemes = to_wx(OPT_GET("Audio/Colour Schemes")->GetListString());
	binder->AddChoice(_("Spectrum"), schemes, "Colour/Audio Display/Spectrum");
	binder->AddChoice(_("Waveform"), schemes, "Colour/Audio Display/Waveform");

	binder->AddCategory(_("Subtitle Grid"));
	binder->AddColour(_("Standard foreground"), "Colour/Subtitle Grid/Standard");
	binder->AddColour(_("Standard background"), "Colour/Subtitle Grid/Background/Background");
	binder->AddColour(_("Selection foreground"), "Colour/Subtitle Grid/Selection");
	binder->AddColour(_("Selection background"), "Colour/Subtitle Grid/Background/Selection");
	binder->AddColour(_("Collision foreground"), "Colour/Subtitle Grid/Collision");
	binder->AddColour(_("In frame background"), "Colour/Subtitle Grid/Background/Inframe");
	binder->AddColour(_("Comment background"), "Colour/Subtitle Grid/Background/Comment");
	binder->AddColour(_("Selected comment background"), "Colour/Subtitle Grid/Background/Selected Comment");
	binder->AddColour(_("Header background"), "Colour/Subtitle Grid/Header");
	binder->AddColour(_("Left Column"), "Colour/Subtitle Grid/Left Column");
	binder->AddColour(_("Active Line Border"), "Colour/Subtitle Grid/Active Border");
	binder->AddColour(_("Lines"), "Colour/Subtitle Grid/Lines");
	binder->AddColour(_("CPS Error"), "Colour/Subtitle Grid/CPS Error");

	binder->AddCategory(_("Visual Typesetting Tools"));
	binder->AddColour(_("Primary Lines"), "Colour/Visual Tools/Lines Primary");
	binder->AddColour(_("Secondary Lines"), "Colour/Visual Tools/Lines Secondary");
	binder->AddColour(_("Primary Highlight"), "Colour/Visual Tools/Highlight Primary");
	binder->AddColour(_("Secondary Highlight"), "Colour/Visual Tools/Highlight Secondary");
	binder->AddDouble(_("Shaded Area"), "Colour/Visual Tools/Shaded Area Alpha", 0.0, 1.0, 0.1, 2);

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Backup preferences page
void BuildBackupPage(OptionPage *p) {
	auto save = p->PageSizer(_("Automatic Save"));
	wxControl *cb = p->OptionAdd(save, _("Enable"), "App/Auto/Save");
	p->CellSkip(save);
	p->EnableIfChecked(cb,
		p->OptionAdd(save, _("Interval in seconds"), "App/Auto/Save Every Seconds", 1));
	p->OptionBrowse(save, _("Path"), "Path/Auto/Save", cb, true);
	p->OptionAdd(save, _("Autosave after every change"), "App/Auto/Save on Every Change");

	auto backup = p->PageSizer(_("Automatic Backup"));
	cb = p->OptionAdd(backup, _("Enable"), "App/Auto/Backup");
	p->CellSkip(backup);
	p->OptionBrowse(backup, _("Path"), "Path/Auto/Backup", cb, true);

	p->SetSizerAndFit(p->sizer);
}

/// Automation preferences page
void BuildAutomationPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));

	p->OptionAdd(general, _("Base path"), "Path/Automation/Base");
	p->OptionAdd(general, _("Include path"), "Path/Automation/Include");
	p->OptionAdd(general, _("Auto-load path"), "Path/Automation/Autoload");

	const wxString tl_arr[6] = { _("0: Fatal"), _("1: Error"), _("2: Warning"), _("3: Hint"), _("4: Debug"), _("5: Trace") };
	wxArrayString tl_choice(6, tl_arr);
	p->OptionChoice(general, _("Trace level"), tl_choice, "Automation/Trace Level");

	const wxString ar_arr[4] = { _("No scripts"), _("Subtitle-local scripts"), _("Global autoload scripts"), _("All scripts") };
	wxArrayString ar_choice(4, ar_arr);
	p->OptionChoice(general, _("Autoreload on Export"), ar_choice, "Automation/Autoreload Mode");

	p->SetSizerAndFit(p->sizer);
}

/// Advanced preferences page
void BuildAdvancedPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));

	auto warning = new wxStaticText(p, wxID_ANY ,_("Changing these settings might result in bugs and/or crashes.  Do not touch these unless you know what you're doing."));
	warning->SetFont(wxFont(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
	p->sizer->Fit(p);
	warning->Wrap(400);
	general->Add(warning, 0, wxALL, 5);

	p->SetSizerAndFit(p->sizer);
}

/// Advanced Audio preferences subpage
void BuildAdvancedAudioPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Expert"));
	binder->AddChoice(_("Audio provider"), GetAudioProviderChoices(), "Audio/Provider");

	wxArrayString apl_choice = to_wx(AudioPlayerFactory::GetClasses());
	binder->AddChoice(_("Audio player"), apl_choice, "Audio/Player");

	binder->AddCategory(_("Cache"));
	const wxString ct_arr[3] = { _("None (Not recommended with Avisynth)"), _("RAM"), _("Hard Disk") };
	wxArrayString ct_choice(3, ct_arr);
	binder->AddChoice(_("Cache type"), ct_choice, "Audio/Cache/Type");
	binder->AddDirectory(_("Path"), "Audio/Cache/HD/Location");

	binder->AddCategory(_("Spectrum Cache"));
	binder->AddInt(_("Cache memory max (MB)"), "Audio/Renderer/Spectrum/Memory Max", 2, 1024);

#ifdef WITH_AVISYNTH
	binder->AddCategory("Avisynth");
	const wxString adm_arr[4] = { "None", "ConvertToMono", "GetLeftChannel", "GetRightChannel" };
	wxArrayString adm_choice(4, adm_arr);
	binder->AddChoice(_("Avisynth down-mixer"), adm_choice, "Audio/Downmixer");
	binder->AddInt(_("Force sample rate"), "Provider/Audio/AVS/Sample Rate", 0, INT_MAX);
#endif

#ifdef WITH_FFMS2
	binder->AddCategory("FFmpegSource");

	const wxString error_modes[] = { _("Ignore"), _("Clear"), _("Stop"), _("Abort") };
	wxArrayString error_modes_choice(4, error_modes);
	binder->AddChoice(_("Audio indexing error handling mode"), error_modes_choice, "Provider/Audio/FFmpegSource/Decode Error Handling");

	binder->AddBool(_("Always index all audio tracks"), "Provider/FFmpegSource/Index All Tracks");
	binder->AddBool(_("Downmix to 16bit mono audio"), "Provider/Audio/FFmpegSource/Downmix");
#endif

#ifdef WITH_PORTAUDIO
	binder->AddCategory("Portaudio");
	binder->AddChoice(_("Portaudio device"), PortAudioPlayer::GetOutputDevices(), "Player/Audio/PortAudio/Device Name");
#endif

#ifdef WITH_OSS
	binder->AddCategory("OSS");
	binder->AddDirectory(_("OSS Device"), "Player/Audio/OSS/Device");
#endif

#if defined(WITH_DIRECTSOUND) && defined(WITH_XAUDIO2)
	binder->AddCategory("DirectSound / XAudio2");
#elif defined(WITH_DIRECTSOUND)
	binder->AddCategory("DirectSound");
#elif defined(WITH_XAUDIO2)
	binder->AddCategory("XAudio2");
#endif
#if defined(WITH_DIRECTSOUND) || defined(WITH_XAUDIO2)
	binder->AddInt(_("Buffer latency"), "Player/Audio/DirectSound/Buffer Latency", 1, 1000);
	binder->AddInt(_("Buffer length"), "Player/Audio/DirectSound/Buffer Length", 1, 100);
#endif

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Advanced Video preferences subpage
void BuildAdvancedVideoPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Expert"));
	binder->AddChoice(_("Video provider"), VideoProviderFactory::GetChoices(), "Video/Provider");
	wxArrayString renderer_choices;
	renderer_choices.Add("opengl");
#ifdef WITH_LIBPLACEBO
	renderer_choices.Add("libplacebo");
#endif
	binder->AddChoice(_("Video renderer"), renderer_choices, "Video/Renderer/Backend");

	wxArrayString sp_choice = to_wx(SubtitlesProviderFactory::GetClasses());
	binder->AddChoice(_("Subtitles provider"), sp_choice, "Subtitle/Provider");

#ifdef WITH_AVISYNTH
	binder->AddCategory("Avisynth");
	binder->AddBool(_("Allow pre-2.56a Avisynth"), "Provider/Avisynth/Allow Ancient");
	binder->AddFile(_("Avisynth runtime library path"), "Provider/Avisynth/Runtime Path",
#ifdef _WIN32
		_("Dynamic libraries (*.dll)|*.dll|All files (*.*)|*.*")
#elif defined(__APPLE__)
		_("Dynamic libraries (*.dylib)|*.dylib|All files (*.*)|*.*")
#else
		_("Shared objects (*.so;*.so.*)|*.so;*.so.*|All files (*.*)|*.*")
#endif
	);
	binder->AddInt(_("Avisynth memory limit"), "Provider/Avisynth/Memory Max", 0, INT_MAX);
#endif

#ifdef WITH_FFMS2
	binder->AddCategory("FFmpegSource");

	const wxString log_levels[] = { "Quiet", "Panic", "Fatal", "Error", "Warning", "Info", "Verbose", "Debug" };
	wxArrayString log_levels_choice(8, log_levels);
	binder->AddChoice(_("Debug log verbosity"), log_levels_choice, "Provider/FFmpegSource/Log Level");

	binder->AddInt(_("Decoding threads"), "Provider/Video/FFmpegSource/Decoding Threads", -1, INT_MAX);
	binder->AddBool(_("Enable unsafe seeking"), "Provider/Video/FFmpegSource/Unsafe Seeking");
#endif

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// wxDataViewIconTextRenderer with command name autocompletion
class CommandRenderer final : public wxDataViewCustomRenderer {
	wxArrayString autocomplete;
	wxDataViewIconText value;
	static const int icon_width = 20;

	int GetIconWidth() const {
		auto view = GetView();
		return view ? view->FromDIP(icon_width) : icon_width;
	}

	wxSize GetDefaultSize() const {
		auto view = GetView();
		return view ? view->FromDIP(wxSize(80, 20)) : wxSize(80, 20);
	}

public:
	CommandRenderer()
	: wxDataViewCustomRenderer("wxDataViewIconText", wxDATAVIEW_CELL_EDITABLE)
	, autocomplete(to_wx(cmd::get_registered_commands()))
	{
	}

	wxWindow *CreateEditorCtrl(wxWindow *parent, wxRect label_rect, wxVariant const& value) override {
		wxDataViewIconText iconText;
		iconText << value;

		wxString text = iconText.GetText();
		int iconWidth = GetIconWidth();

		// adjust the label rect to take the width of the icon into account
		label_rect.x += iconWidth;
		label_rect.width -= iconWidth;

		wxTextCtrl* ctrl = new wxTextCtrl(parent, -1, text, label_rect.GetPosition(), label_rect.GetSize(), wxTE_PROCESS_ENTER);
		ctrl->SetInsertionPointEnd();
		ctrl->SelectAll();
		ctrl->AutoComplete(autocomplete);
		return ctrl;
	}

	bool SetValue(wxVariant const& var) override {
		value << var;
		return true;
	}

	bool Render(wxRect rect, wxDC *dc, int state) override {
		wxIcon const& icon = value.GetIcon();
		int iconWidth = GetIconWidth();
		if (icon.IsOk())
			dc->DrawIcon(icon, rect.x, rect.y + (rect.height - icon.GetHeight()) / 2);

		RenderText(value.GetText(), iconWidth, rect, dc, state);

		return true;
	}

	wxSize GetSize() const override {
		if (!value.GetText().empty()) {
			wxSize size = GetTextExtent(value.GetText());
			size.x += GetIconWidth();
			return size;
		}
		return GetDefaultSize();
	}

	bool GetValueFromEditorCtrl(wxWindow* editor, wxVariant &var) override {
		wxTextCtrl *text = static_cast<wxTextCtrl*>(editor);
		wxDataViewIconText iconText(text->GetValue(), value.GetIcon());
		var << iconText;
		return true;
	}

	bool GetValue(wxVariant &) const override { return false; }
	bool HasEditorCtrl() const override { return true; }
};

class HotkeyRenderer final : public wxDataViewCustomRenderer {
	wxString value;
	wxTextCtrl *ctrl = nullptr;

	wxSize GetDefaultSize() const {
		auto view = GetView();
		return view ? view->FromDIP(wxSize(80, 20)) : wxSize(80, 20);
	}

public:
	HotkeyRenderer()
	: wxDataViewCustomRenderer("string", wxDATAVIEW_CELL_EDITABLE)
	{ }

	wxWindow *CreateEditorCtrl(wxWindow *parent, wxRect label_rect, wxVariant const& var) override {
		ctrl = new wxTextCtrl(parent, -1, var.GetString(), label_rect.GetPosition(), label_rect.GetSize(), wxTE_PROCESS_ENTER);
		ctrl->SetInsertionPointEnd();
		ctrl->SelectAll();
		ctrl->Bind(wxEVT_CHAR_HOOK, &HotkeyRenderer::OnKeyDown, this);
		return ctrl;
	}

	void OnKeyDown(wxKeyEvent &evt) {
		ctrl->ChangeValue(to_wx(hotkey::keypress_to_str(evt.GetKeyCode(), evt.GetModifiers())));
	}

	bool SetValue(wxVariant const& var) override {
		value = var.GetString();
		return true;
	}

	bool Render(wxRect rect, wxDC *dc, int state) override {
		RenderText(value, 0, rect, dc, state);
		return true;
	}

	bool GetValueFromEditorCtrl(wxWindow*, wxVariant &var) override {
		var = ctrl->GetValue();
		return true;
	}

	bool GetValue(wxVariant &) const override { return false; }
	wxSize GetSize() const override { return !value ? GetDefaultSize() : GetTextExtent(value); }
	bool HasEditorCtrl() const override { return true; }
};

static void edit_item(wxDataViewCtrl *dvc, wxDataViewItem item) {
	dvc->EditItem(item, dvc->GetColumn(0));
}

class Interface_Hotkeys final : public OptionPage {
	wxDataViewCtrl *dvc;
	wxObjectDataPtr<HotkeyDataViewModel> model;
	wxSearchCtrl *quick_search;

	void OnNewButton(wxCommandEvent&);
	void OnUpdateFilter(wxCommandEvent&);
public:
	Interface_Hotkeys(wxTreebook *book, Preferences *parent);
};

/// Interface Hotkeys preferences subpage
Interface_Hotkeys::Interface_Hotkeys(wxTreebook *book, Preferences *parent)
: OptionPage(book, parent, _("Hotkeys"), OptionPage::PAGE_SUB)
, model(new HotkeyDataViewModel(parent))
{
	quick_search = new wxSearchCtrl(this, -1);
	auto new_button = new wxButton(this, -1, _("&New"));
	auto edit_button = new wxButton(this, -1, _("&Edit"));
	auto delete_button = new wxButton(this, -1, _("&Delete"));

	new_button->Bind(wxEVT_BUTTON, &Interface_Hotkeys::OnNewButton, this);
	edit_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) { edit_item(dvc, dvc->GetSelection()); });
	delete_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) { model->Delete(dvc->GetSelection()); });

	quick_search->Bind(wxEVT_TEXT, &Interface_Hotkeys::OnUpdateFilter, this);
	quick_search->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [=](wxCommandEvent&) { quick_search->SetValue(""); });

	dvc = new wxDataViewCtrl(this, -1);
	dvc->AssociateModel(model.get());
#ifndef __APPLE__
	dvc->AppendColumn(new wxDataViewColumn("Hotkey", new HotkeyRenderer, 0, 125, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
	dvc->AppendColumn(new wxDataViewColumn("Command", new CommandRenderer, 1, 250, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
#else
	auto col = new wxDataViewColumn("Hotkey", new wxDataViewTextRenderer("string", wxDATAVIEW_CELL_EDITABLE), 0, 150, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE);
	col->SetMinWidth(150);
	dvc->AppendColumn(col);
	dvc->AppendColumn(new wxDataViewColumn("Command", new wxDataViewIconTextRenderer("wxDataViewIconText", wxDATAVIEW_CELL_EDITABLE), 1, 250, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
#endif
	dvc->AppendTextColumn("Description", 2, wxDATAVIEW_CELL_INERT, 300, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE);

	wxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(quick_search, wxSizerFlags().Border());
	buttons->AddStretchSpacer(1);
	buttons->Add(new_button, wxSizerFlags().Border());
	buttons->Add(edit_button, wxSizerFlags().Border());
	buttons->Add(delete_button, wxSizerFlags().Border());

	sizer->Add(buttons, wxSizerFlags().Expand());
	sizer->Add(dvc, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));

	SetSizerAndFit(sizer);
}

void Interface_Hotkeys::OnNewButton(wxCommandEvent&) {
	wxDataViewItem sel = dvc->GetSelection();
	dvc->ExpandAncestors(sel);
	dvc->Expand(sel);

	wxDataViewItem new_item = model->New(sel);
	if (new_item.IsOk()) {
		dvc->Select(new_item);
		dvc->EnsureVisible(new_item);
		edit_item(dvc, new_item);
	}
}

void Interface_Hotkeys::OnUpdateFilter(wxCommandEvent&) {
	model->SetFilter(quick_search->GetValue());

	if (!quick_search->GetValue().empty()) {
		wxDataViewItemArray contexts;
		model->GetChildren(wxDataViewItem(nullptr), contexts);
		for (auto const& context : contexts)
			dvc->Expand(context);
	}
}
}

void Preferences::RegisterDeferredPageBuilder(Thunk builder, bool built) {
	deferred_page_builders.push_back(std::move(builder));
	deferred_page_built.push_back(built);
}

void Preferences::EnsureDeferredPageBuilt(int page) {
	if (page < 0 || page >= static_cast<int>(deferred_page_builders.size()))
		return;
	if (deferred_page_built[page])
		return;

	bool const should_freeze = IsShownOnScreen();
	bool const is_current_page = book && book->GetSelection() == page;
	wxSize const old_size = GetSize();
	if (should_freeze)
		Freeze();

	deferred_page_builders[page]();
	deferred_page_built[page] = true;

	book->InvalidateBestSize();
	book->Layout();
	if (auto* sizer = GetSizer())
		sizer->Layout();
	Layout();
	if (is_current_page) {
		if (auto* sizer = GetSizer()) {
			sizer->Fit(this);
			auto const fitted_size = GetSize();
			SetSize(std::max(old_size.x, fitted_size.x), std::max(old_size.y, fitted_size.y));
		}
	}

	if (should_freeze)
		Thaw();
}

void Preferences::EnsureAllDeferredPagesBuilt() {
	for (int page = 0; page < static_cast<int>(deferred_page_builders.size()); ++page)
		EnsureDeferredPageBuilt(page);
}

void Preferences::SetOption(std::unique_ptr<agi::OptionValue> new_value) {
	pending_changes[new_value->GetName()] = std::move(new_value);
	if (applyButton)
		applyButton->Enable(true);
}

void Preferences::AddPendingChange(Thunk const& callback) {
	pending_callbacks.push_back(callback);
	if (applyButton)
		applyButton->Enable(true);
}

void Preferences::AddChangeableOption(std::string const& name) {
	option_names.push_back(name);
}

void Preferences::OnOK(wxCommandEvent &event) {
	OnApply(event);
	EndModal(0);
}

void Preferences::OnApply(wxCommandEvent &) {
	for (auto const& change : pending_changes)
		OPT_SET(change.first)->Set(change.second.get());
	pending_changes.clear();

	for (auto const& thunk : pending_callbacks)
		thunk();
	pending_callbacks.clear();

	applyButton->Enable(false);
	config::opt->Flush();
}

void Preferences::OnResetDefault(wxCommandEvent&) {
	auto interaction = agi::MakeWindowInteractionSink(this);
	if (interaction->Request({
		from_wx(_("Restore defaults?")),
		from_wx(_("Are you sure that you want to restore the defaults? All your settings will be overridden.")),
		agi::InteractionButtons::YesNo,
		agi::InteractionIcon::Question
	}) != agi::InteractionResult::Yes)
		return;

	EnsureAllDeferredPagesBuilt();

	for (auto const& opt_name : option_names) {
		agi::OptionValue *opt = OPT_SET(opt_name);
		if (!opt->IsDefault())
			opt->Reset();
	}
	config::opt->Flush();

	agi::hotkey::Hotkey def_hotkeys("", GET_DEFAULT_CONFIG(default_hotkey));
	hotkey::inst->SetHotkeyMap(def_hotkeys.GetHotkeyMap());

	// Close and reopen the dialog to update all the controls with the new values
	OPT_SET("Tool/Preferences/Page")->SetInt(book->GetSelection());
	EndModal(-1);
}

Preferences::Preferences(wxWindow *parent): wxDialog(parent, -1, _("Preferences"), wxDefaultPosition, wxSize(-1, -1), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
	SetIcon(GETICON(options_button_16));

	auto duration_ms = [](auto const& started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	};
	auto observe_phase = [&](char const* phase, auto&& callback) {
		auto const started = std::chrono::steady_clock::now();
		callback();
		perf_trace::ObserveWindowOpenPhase("preferences", phase, duration_ms(started));
	};

	observe_phase("treebook_create", [&] {
		book = new wxTreebook(this, -1, wxDefaultPosition, wxDefaultSize);
		book->SetDoubleBuffered(true);
		if (auto *tree = book->GetTreeCtrl())
			tree->SetDoubleBuffered(true);
	});

	auto register_deferred_page = [&](char const* phase, wxString const& name, int style, auto builder) {
		auto* page = new OptionPage(book, this, name, style);
		RegisterDeferredPageBuilder([&, page, phase, builder] {
			auto const started = std::chrono::steady_clock::now();
			builder(page);
			perf_trace::ObserveWindowOpenPhase("preferences", phase, duration_ms(started));
		});
	};

	register_deferred_page("page_general", _("General"), OptionPage::PAGE_DEFAULT, BuildGeneralPage);
	register_deferred_page("page_default_styles", _("Default styles"), OptionPage::PAGE_SUB, BuildGeneralDefaultStylesPage);
	register_deferred_page("page_audio", _("Audio"), OptionPage::PAGE_DEFAULT, BuildAudioPage);
	register_deferred_page("page_video", _("Video"), OptionPage::PAGE_DEFAULT, BuildVideoPage);
	register_deferred_page("page_interface", _("Interface"), OptionPage::PAGE_DEFAULT, BuildInterfacePage);
	register_deferred_page("page_interface_colours", _("Colors"), OptionPage::PAGE_SCROLL | OptionPage::PAGE_SUB, BuildInterfaceColoursPage);
	observe_phase("page_hotkeys", [&] { new Interface_Hotkeys(book, this); });
	RegisterDeferredPageBuilder({}, true);
	register_deferred_page("page_backup", _("Backup"), OptionPage::PAGE_DEFAULT, BuildBackupPage);
	register_deferred_page("page_automation", _("Automation"), OptionPage::PAGE_DEFAULT, BuildAutomationPage);
	register_deferred_page("page_advanced", _("Advanced"), OptionPage::PAGE_DEFAULT, BuildAdvancedPage);
	register_deferred_page("page_advanced_audio", _("Audio"), OptionPage::PAGE_SUB, BuildAdvancedAudioPage);
	register_deferred_page("page_advanced_video", _("Video"), OptionPage::PAGE_SUB, BuildAdvancedVideoPage);

	int initial_page = OPT_GET("Tool/Preferences/Page")->GetInt();
	if (initial_page < 0 || initial_page >= static_cast<int>(deferred_page_builders.size()))
		initial_page = 0;
	book->ChangeSelection(initial_page);
	EnsureDeferredPageBuilt(initial_page);

	observe_phase("book_fit", [&] {
		book->Fit();
	});

	wxSizer *mainSizer = nullptr;
	wxButton *defaultButton = nullptr;
	observe_phase("dialog_chrome", [&] {
		book->Bind(wxEVT_TREEBOOK_PAGE_CHANGING, [this](wxBookCtrlEvent &evt) {
			EnsureDeferredPageBuilt(evt.GetSelection());
		});
		book->Bind(wxEVT_TREEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &evt) {
			OPT_SET("Tool/Preferences/Page")->SetInt(evt.GetSelection());
		});

		// Bottom Buttons
		auto stdButtonSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxAPPLY | wxHELP);
		applyButton = stdButtonSizer->GetApplyButton();
		wxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
		defaultButton = new wxButton(this, -1, _("&Restore Defaults"));
		buttonSizer->Add(defaultButton, wxSizerFlags(0).Expand());
		buttonSizer->AddStretchSpacer(1);
		buttonSizer->Add(stdButtonSizer, wxSizerFlags(0).Expand());

		// Main Sizer
		mainSizer = new wxBoxSizer(wxVERTICAL);
		mainSizer->Add(book, wxSizerFlags(1).Expand().Border());
		mainSizer->Add(buttonSizer, wxSizerFlags(0).Expand().Border(wxALL & ~wxTOP));
	});

	observe_phase("dialog_fit", [&] {
		SetSizerAndFit(mainSizer);
		wxSize const fitted = GetSize();
		SetMinSize(fitted);
		SetSize(std::max(fitted.x, FromDIP(520)), std::max(fitted.y, FromDIP(720)));
	});
	observe_phase("dialog_center", [&] {
		persist = agi::make_unique<PersistLocation>(this, "Tool/Preferences", true);
	});

	applyButton->Enable(false);

	Bind(wxEVT_BUTTON, &Preferences::OnOK, this, wxID_OK);
	Bind(wxEVT_BUTTON, &Preferences::OnApply, this, wxID_APPLY);
	Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Options"), wxID_HELP);
	defaultButton->Bind(wxEVT_BUTTON, &Preferences::OnResetDefault, this);
}

void ShowPreferences(wxWindow *parent) {
	while (true) {
		auto const open_started = std::chrono::steady_clock::now();
		perf_trace::TraceWindowOpenBegin("preferences");
		try {
			Preferences dialog(parent);
			auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count();
			perf_trace::TraceWindowOpenEnd("preferences", duration_ms, true);
			if (dialog.ShowModal() >= 0)
				break;
		}
		catch (...) {
			auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count();
			perf_trace::TraceWindowOpenEnd("preferences", duration_ms, false);
			throw;
		}
	}
}
