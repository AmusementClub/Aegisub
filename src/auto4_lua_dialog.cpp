// Copyright (c) 2006, 2007, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file auto4_lua_dialog.cpp
/// @brief Lua 5.1-based scripting engine (configuration-dialogue interface)
/// @ingroup scripting
///

#include "auto4_lua.h"

#include "colour_button.h"
#include "compat.h"
#include "perf_trace.h"
#include "string_codec.h"
#include "validators.h"

#include <libaegisub/log.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/split.h>
#include <libaegisub/string_utils.h>

#include <chrono>
#include <cfloat>
#include <map>
#include <unordered_map>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/gbsizer.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/valgen.h>

using namespace agi::lua;
namespace {
	struct ControlCreateSummary {
		int instance_count = 0;
		int item_count_total = 0;
		int item_count_max = 0;
		double duration_ms = 0.0;
	};

	inline void get_if_right_type(lua_State *L, std::string &def) {
		if (lua_isstring(L, -1))
			def = lua_tostring(L, -1);
	}

	inline void get_if_right_type(lua_State *L, double &def) {
		if (lua_isnumber(L, -1))
			def = lua_tonumber(L, -1);
	}

	inline void get_if_right_type(lua_State *L, int &def) {
		if (lua_isnumber(L, -1))
			def = lua_tointeger(L, -1);
	}

	inline void get_if_right_type(lua_State *L, bool &def) {
		if (lua_isboolean(L, -1))
			def = !!lua_toboolean(L, -1);
	}

	template<class T>
	T get_field(lua_State *L, const char *name, T def) {
		lua_getfield(L, -1, name);
		get_if_right_type(L, def);
		lua_pop(L, 1);
		return def;
	}

	inline std::string get_field(lua_State *L, const char *name) {
		return get_field(L, name, std::string());
	}

	template<class T>
	void read_string_array(lua_State *L, T &cont) {
		lua_for_each(L, [&] {
			if (lua_isstring(L, -1))
				cont.push_back(lua_tostring(L, -1));
		});
	}

	int string_to_wx_id(std::string const& str) {
		static std::unordered_map<std::string, int> ids;
		if (ids.empty()) {
			ids["ok"] = wxID_OK;
			ids["yes"] = wxID_YES;
			ids["save"] = wxID_SAVE;
			ids["apply"] = wxID_APPLY;
			ids["close"] = wxID_CLOSE;
			ids["no"] = wxID_NO;
			ids["cancel"] = wxID_CANCEL;
			ids["help"] = wxID_HELP;
			ids["context_help"] = wxID_CONTEXT_HELP;
		}
		auto it = ids.find(str);
		return it == end(ids) ? -1 : it->second;
	}

	double DurationMs(std::chrono::steady_clock::duration duration) {
		return std::chrono::duration<double, std::milli>(duration).count();
	}

	void SetTooltipIfPresent(wxControl* control, wxString const& hint) {
		if (!hint.empty())
			control->SetToolTip(hint);
	}
}

namespace Automation4 {
	// LuaDialogControl
	LuaDialogControl::LuaDialogControl(lua_State *L, char const* trace_type)
	// Assume top of stack is a control table (don't do checking)
	: trace_type(trace_type)
	, name(get_field(L, "name"))
	, hint(get_field(L, "hint"))
	, hint_wx(hint.empty() ? wxString{} : to_wx(hint))
	, x(get_field(L, "x", 0))
	, y(get_field(L, "y", 0))
	, width(get_field(L, "width", 1))
	, height(get_field(L, "height", 1))
	{
	}

	namespace LuaControl {
		/// A static text label
		class Label final : public LuaDialogControl {
			std::string label;
		public:
			Label(lua_State *L) : LuaDialogControl(L, "label"), label(get_field(L, "label")) { }

			wxControl *Create(wxWindow *parent) override {
				return new wxStaticText(parent, -1, to_wx(label));
			}

			int GetSizerFlags() const override { return wxALIGN_CENTRE_VERTICAL | wxALIGN_LEFT; }

			void LuaReadBack(lua_State *L) override {
				// Label doesn't produce output, so let it be nil
				lua_pushnil(L);
			}
		};

		/// A single-line text edit control
		class Edit : public LuaDialogControl {
		protected:
			std::string text;
			wxTextCtrl *cw = nullptr;

		public:
			Edit(lua_State *L, char const* trace_type = "edit")
			: LuaDialogControl(L, trace_type)
			, text(get_field(L, "value"))
			{
				// Undocumented behaviour, 'value' is also accepted as key for text,
				// mostly so a text control can stand in for other things.
				// This shouldn't be exploited and might change later.
				text = get_field(L, "text", text);
			}

			bool CanSerialiseValue() const override { return true; }
			std::string SerialiseValue() const override { return inline_string_encode(text); }
			void UnserialiseValue(const std::string &serialised) override { text = inline_string_decode(serialised); }

			wxControl *Create(wxWindow *parent) override {
				cw = new wxTextCtrl(parent, -1, to_wx(text));
				cw->SetMaxLength(0);
				cw->SetValidator(StringBinder(&text));
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushstring(L, text.c_str());
			}
		};

		/// A color-picker button
		class Color final : public LuaDialogControl {
			agi::Color color;
			bool alpha;

		public:
			Color(lua_State *L, bool alpha)
			: LuaDialogControl(L, alpha ? "coloralpha" : "color")
			, color(get_field(L, "value"))
			, alpha(alpha)
			{
			}

			bool CanSerialiseValue() const override { return true; }
			std::string SerialiseValue() const override { return inline_string_encode(color.GetHexFormatted(alpha)); }
			void UnserialiseValue(const std::string &serialised) override { color = inline_string_decode(serialised); }

			wxControl *Create(wxWindow *parent) override {
				wxControl *cw = new ColourButton(parent, wxSize(50*width,10*height), alpha, color, ColorValidator(&color));
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushstring(L, color.GetHexFormatted(alpha).c_str());
			}
		};

		/// A multiline text edit control
		class Textbox final : public Edit {
		public:
			Textbox(lua_State *L) : Edit(L, "textbox") { }

			// Same serialisation interface as single-line edit
			wxControl *Create(wxWindow *parent) override {
				cw = new wxTextCtrl(parent, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE, StringBinder(&text));
				cw->SetMinSize(wxSize(0, parent->FromDIP(30)));
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}
		};

		/// Integer only edit
		class IntEdit final : public Edit {
			wxSpinCtrl *cw = nullptr;
			int value;
			int min, max;

		public:
			IntEdit(lua_State *L)
			: Edit(L, "intedit")
			, value(get_field(L, "value", 0))
			, min(get_field(L, "min", INT_MIN))
			, max(get_field(L, "max", INT_MAX))
			{
				if (min >= max) {
					max = INT_MAX;
					min = INT_MIN;
				}
			}

			bool CanSerialiseValue() const override  { return true; }
			std::string SerialiseValue() const override { return std::to_string(value); }
			void UnserialiseValue(const std::string &serialised) override { value = atoi(serialised.c_str()); }

			wxControl *Create(wxWindow *parent) override {
				cw = new wxSpinCtrl(parent, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value);
				cw->SetValidator(wxGenericValidator(&value));
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushinteger(L, value);
			}
		};

		// Float only edit
		class FloatEdit final : public Edit {
			double value;
			double min;
			double max;
			double step;
			wxSpinCtrlDouble *scd = nullptr;

		public:
			FloatEdit(lua_State *L)
			: Edit(L, "floatedit")
			, value(get_field(L, "value", 0.0))
			, min(get_field(L, "min", -DBL_MAX))
			, max(get_field(L, "max", DBL_MAX))
			, step(get_field(L, "step", 0.0))
			{
				if (min >= max) {
					max = DBL_MAX;
					min = -DBL_MAX;
				}
				// wxSpinCtrlDouble sizes itself to fit the full range. For
				// step-based float edits with unspecified bounds, use wx's
				// practical defaults instead of +/- DBL_MAX to avoid absurdly
				// wide controls on wx 3.1.4+.
				if (step != 0.0) {
					min = min == -DBL_MAX ? 0.0 : min;
					max = max == DBL_MAX ? 100.0 : max;
				}
			}

			bool CanSerialiseValue() const override { return true; }
			std::string SerialiseValue() const override { return std::to_string(value); }
			void UnserialiseValue(const std::string &serialised) override { value = atof(serialised.c_str()); }

			wxControl *Create(wxWindow *parent) override {
				if (step > 0) {
					scd = new wxSpinCtrlDouble(parent, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value, step);
					scd->SetValidator(DoubleSpinValidator(&value));
					SetTooltipIfPresent(scd, hint_wx);
					return scd;
				}

				DoubleValidator val(&value, min, max);
				cw = new wxTextCtrl(parent, -1, "", wxDefaultPosition, wxDefaultSize, 0, val);
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushnumber(L, value);
			}
		};

		/// A dropdown list
		class Dropdown final : public LuaDialogControl {
			std::vector<std::string> items;
			std::string value;
			wxArrayString items_wx;
			wxString value_wx;
			wxComboBox *cw = nullptr;

		public:
			Dropdown(lua_State *L)
			: LuaDialogControl(L, "dropdown")
			, value(get_field(L, "value"))
			{
				lua_getfield(L, -1, "items");
				read_string_array(L, items);
				items_wx = to_wx(items);
				value_wx = to_wx(value);
			}

			int GetTraceItemCount() const override { return static_cast<int>(items.size()); }

			bool CanSerialiseValue() const override { return true; }
			std::string SerialiseValue() const override { return inline_string_encode(value); }
			void UnserialiseValue(const std::string &serialised) override {
				value = inline_string_decode(serialised);
				value_wx = to_wx(value);
			}

			wxControl *Create(wxWindow *parent) override {
				cw = new wxComboBox(parent, -1, value_wx, wxDefaultPosition, wxDefaultSize, items_wx, wxCB_READONLY, StringBinder(&value));
				SetTooltipIfPresent(cw, hint_wx);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushstring(L, value.c_str());
			}
		};

		class Checkbox final : public LuaDialogControl {
			std::string label;
			bool value;
			wxCheckBox *cw = nullptr;

		public:
			Checkbox(lua_State *L)
			: LuaDialogControl(L, "checkbox")
			, label(get_field(L, "label"))
			, value(get_field(L, "value", false))
			{
			}

			bool CanSerialiseValue() const override { return true; }
			std::string SerialiseValue() const override { return value ? "1" : "0"; }
			void UnserialiseValue(const std::string &serialised) override { value = serialised != "0"; }

			wxControl *Create(wxWindow *parent) override {
				cw = new wxCheckBox(parent, -1, to_wx(label));
				cw->SetValidator(wxGenericValidator(&value));
				SetTooltipIfPresent(cw, hint_wx);
				cw->SetValue(value);
				return cw;
			}

			void LuaReadBack(lua_State *L) override {
				lua_pushboolean(L, value);
			}
		};
	}

	// LuaDialog
	LuaDialog::LuaDialog(lua_State *L, bool include_buttons)
	: use_buttons(include_buttons)
	{
		auto const build_model_started = std::chrono::steady_clock::now();
		LOG_D("automation/lua/dialog") << "creating LuaDialoug, addr: " << this;

		// assume top of stack now contains a dialog table
		if (!lua_istable(L, 1))
			error(L, "Cannot create config dialog from something non-table");

		// Ok, so there is a table with controls
		controls.reserve(static_cast<size_t>(lua_objlen(L, 1)));
		lua_pushvalue(L, 1);
		lua_for_each(L, [&] {
			if (!lua_istable(L, -1))
				error(L, "bad control table entry");

			std::string controlclass = get_field(L, "class");
			agi::util::strings::to_lower_inplace(controlclass);

			std::unique_ptr<LuaDialogControl> ctl;

			// Check control class and create relevant control
			if (controlclass == "label")
				ctl = agi::make_unique<LuaControl::Label>(L);
			else if (controlclass == "edit")
				ctl = agi::make_unique<LuaControl::Edit>(L);
			else if (controlclass == "intedit")
				ctl = agi::make_unique<LuaControl::IntEdit>(L);
			else if (controlclass == "floatedit")
				ctl = agi::make_unique<LuaControl::FloatEdit>(L);
			else if (controlclass == "textbox")
				ctl = agi::make_unique<LuaControl::Textbox>(L);
			else if (controlclass == "dropdown")
				ctl = agi::make_unique<LuaControl::Dropdown>(L);
			else if (controlclass == "checkbox")
				ctl = agi::make_unique<LuaControl::Checkbox>(L);
			else if (controlclass == "color")
				ctl = agi::make_unique<LuaControl::Color>(L, false);
			else if (controlclass == "coloralpha")
				ctl = agi::make_unique<LuaControl::Color>(L, true);
			else if (controlclass == "alpha")
				// FIXME
				ctl = agi::make_unique<LuaControl::Edit>(L);
			else
				error(L, "bad control table entry");

			controls.emplace_back(std::move(ctl));
		});

		if (include_buttons && lua_istable(L, 2)) {
			buttons.reserve(static_cast<size_t>(lua_objlen(L, 2)));
			lua_pushvalue(L, 2);
			lua_for_each(L, [&]{
				buttons.emplace_back(-1, check_string(L, -1));
			});
		}

		if (include_buttons && lua_istable(L, 3)) {
			std::unordered_map<std::string, size_t> button_indices;
			button_indices.reserve(buttons.size());
			for (size_t i = 0; i < buttons.size(); ++i)
				button_indices.emplace(buttons[i].second, i);

			lua_pushvalue(L, 3);
			lua_for_each(L, [&]{
				int id = string_to_wx_id(check_string(L, -2));
				std::string label = check_string(L, -1);
				auto const btn = button_indices.find(label);
				if (btn == end(button_indices))
					error(L, "Invalid button for id %s", lua_tostring(L, -2));
				buttons[btn->second].first = id;
			});
		}

		perf_trace::ObserveLuaDialogPhase(
			"build_model",
			static_cast<int>(controls.size()),
			static_cast<int>(buttons.size()),
			DurationMs(std::chrono::steady_clock::now() - build_model_started));
	}

	wxWindow* LuaDialog::CreateWindow(wxWindow *parent) {
		auto const create_window_started = std::chrono::steady_clock::now();
		window = new wxPanel(parent);

		auto s = new wxGridBagSizer(4, 4);
		auto const create_controls_started = std::chrono::steady_clock::now();
		std::map<std::string, ControlCreateSummary> control_summaries;
		for (auto& c : controls) {
			auto const control_started = std::chrono::steady_clock::now();
			auto* created_control = c->Create(window);
			auto const control_duration_ms = DurationMs(std::chrono::steady_clock::now() - control_started);

			auto& summary = control_summaries[c->GetTraceType()];
			++summary.instance_count;
			summary.duration_ms += control_duration_ms;

			auto const item_count = c->GetTraceItemCount();
			summary.item_count_total += item_count;
			summary.item_count_max = std::max(summary.item_count_max, item_count);

			s->Add(created_control, wxGBPosition(c->y, c->x),
				wxGBSpan(c->height, c->width), c->GetSizerFlags());
		}
		for (auto const& [control_type, summary] : control_summaries) {
			perf_trace::ObserveLuaDialogControlTypeSummary(
				control_type.c_str(),
				static_cast<int>(controls.size()),
				static_cast<int>(buttons.size()),
				summary.instance_count,
				summary.item_count_total,
				summary.item_count_max,
				summary.duration_ms);
		}
		perf_trace::ObserveLuaDialogPhase(
			"create_controls",
			static_cast<int>(controls.size()),
			static_cast<int>(buttons.size()),
			DurationMs(std::chrono::steady_clock::now() - create_controls_started));

		if (!use_buttons) {
			auto const layout_started = std::chrono::steady_clock::now();
			window->SetSizerAndFit(s);
			auto const layout_duration_ms = DurationMs(std::chrono::steady_clock::now() - layout_started);
			perf_trace::ObserveLuaDialogPhase("layout_window", static_cast<int>(controls.size()), 0, layout_duration_ms);
			perf_trace::ObserveLuaDialogPhase(
				"create_window_total",
				static_cast<int>(controls.size()),
				0,
				DurationMs(std::chrono::steady_clock::now() - create_window_started));
			perf_trace::TraceLuaDialogOpenEnd(static_cast<int>(controls.size()), 0, -1.0, true);
			return window;
		}

		auto const create_buttons_started = std::chrono::steady_clock::now();
		if (buttons.size() == 0) {
			buttons.emplace_back(wxID_OK, "");
			buttons.emplace_back(wxID_CANCEL, "");
		}

		auto dialog = static_cast<wxDialog *>(parent);
		auto bs = new wxStdDialogButtonSizer;

		auto make_button = [&](wxWindowID id, int button_pushed, std::string const& text) -> wxButton *{
			auto button = new wxButton(window, id, to_wx(text));
			button->Bind(wxEVT_BUTTON, [=](wxCommandEvent &evt) {
				this->button_pushed = button_pushed;
				dialog->TransferDataFromWindow();
				dialog->EndModal(0);
			});

			if (id == wxID_OK || id == wxID_YES || id == wxID_SAVE) {
				button->SetDefault();
				dialog->SetAffirmativeId(id);
			}

			if (id == wxID_CLOSE || id == wxID_NO)
				dialog->SetEscapeId(id);

			return button;
		};

		if (std::none_of(buttons.begin(), buttons.end(), [](std::pair<int, std::string> const& button) { return button.first == -1; })) {
			for (size_t i = 0; i < buttons.size(); ++i)
				bs->AddButton(make_button(buttons[i].first, i, buttons[i].second));
			bs->Realize();
		}
		else {
			for (size_t i = 0; i < buttons.size(); ++i)
				bs->Add(make_button(buttons[i].first, i, buttons[i].second));
		}
		perf_trace::ObserveLuaDialogPhase(
			"create_buttons",
			static_cast<int>(controls.size()),
			static_cast<int>(buttons.size()),
			DurationMs(std::chrono::steady_clock::now() - create_buttons_started));

		auto const layout_started = std::chrono::steady_clock::now();
		auto ms = new wxBoxSizer(wxVERTICAL);
		ms->Add(s, 0, wxBOTTOM, 5);
		ms->Add(bs);
		window->SetSizerAndFit(ms);
		auto const layout_duration_ms = DurationMs(std::chrono::steady_clock::now() - layout_started);
		perf_trace::ObserveLuaDialogPhase(
			"layout_window",
			static_cast<int>(controls.size()),
			static_cast<int>(buttons.size()),
			layout_duration_ms);
		perf_trace::ObserveLuaDialogPhase(
			"create_window_total",
			static_cast<int>(controls.size()),
			static_cast<int>(buttons.size()),
			DurationMs(std::chrono::steady_clock::now() - create_window_started));
		perf_trace::TraceLuaDialogOpenEnd(static_cast<int>(controls.size()), static_cast<int>(buttons.size()), -1.0, true);

		return window;
	}

	int LuaDialog::LuaReadBack(lua_State *L) {
		// First read back which button was pressed, if any
		if (use_buttons) {
			if (button_pushed == -1 || buttons[button_pushed].first == wxID_CANCEL)
				lua_pushboolean(L, false);
			else
				lua_pushstring(L, buttons[button_pushed].second.c_str());
		}

		// Then read controls back
		lua_createtable(L, 0, controls.size());
		for (auto& control : controls) {
			control->LuaReadBack(L);
			lua_setfield(L, -2, control->name.c_str());
		}

		return use_buttons ? 2 : 1;
	}

	std::string LuaDialog::Serialise() {
		std::string res;

		// Format into "name1:value1|name2:value2|name3:value3"
		for (auto& control : controls) {
			if (control->CanSerialiseValue()) {
				if (!res.empty())
					res += "|";
				res += inline_string_encode(control->name) + ":" + control->SerialiseValue();
			}
		}

		return res;
	}

	void LuaDialog::Unserialise(const std::string &serialised) {
		for (auto tok : agi::Split(serialised, '|')) {
			auto pos = std::find(begin(tok), end(tok), ':');
			if (pos == end(tok)) continue;

			std::string name = inline_string_decode(std::string(begin(tok), pos));
			std::string value(pos + 1, end(tok));

			// Hand value to all controls matching name
			for (auto& control : controls) {
				if (control->name == name && control->CanSerialiseValue())
					control->UnserialiseValue(value);
			}
		}
	}
}
