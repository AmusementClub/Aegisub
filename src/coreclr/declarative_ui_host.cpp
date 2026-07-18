#include "declarative_ui_host.h"

#include "declarative_ui_model.h"

#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "ui_dispatch.h"
#include "utils.h"
#include "video_display.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cmath>
#include <compare>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/gbsizer.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace agi::coreclr::ui {
namespace {

constexpr char kOpenFormService[] = "aegisub.ui.openForm";
constexpr char kOpenToolViewService[] = "aegisub.ui.openToolView";
constexpr char kPatchToolViewService[] = "aegisub.ui.patchToolView";
constexpr char kCloseToolViewService[] = "aegisub.ui.closeToolView";
constexpr char kBeginVideoPointSelectionService[] = "aegisub.video.beginPointSelection";
constexpr char kToolViewEvent[] = "aegisub.ui.toolViewEvent";
constexpr char kVideoPointSelectionEvent[] = "aegisub.video.pointSelectionCompleted";

void QueuePluginEvent(
	PluginEventDispatcher dispatcher,
	uint64_t plugin_handle,
	std::string event_id,
	std::string payload_json) {
	agi::dispatch::Background().Async([
		dispatcher = std::move(dispatcher),
		plugin_handle,
		event_id = std::move(event_id),
		payload_json = std::move(payload_json)] {
		try {
			dispatcher(plugin_handle, event_id, payload_json);
		}
		catch (std::exception const& error) {
			LOG_E("automation/plugin_bridge")
				<< "Could not dispatch declarative UI event: " << error.what();
		}
	});
}

std::string JsonString(std::string const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

template<typename T>
std::string JsonScalar(T value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

std::string DisplayJsonValue(std::string const& json_value) {
	try {
		std::istringstream stream(json_value);
		json::UnknownElement value;
		json::Reader::Read(value, stream);
		try { return static_cast<json::String const&>(value); }
		catch (...) { }
		try { return static_cast<json::Boolean const&>(value) ? "true" : "false"; }
		catch (...) { }
		try { return std::to_string(static_cast<json::Integer const&>(value)); }
		catch (...) { }
		try {
			std::ostringstream number;
			number << static_cast<json::Double const&>(value);
			return number.str();
		}
		catch (...) { }
	}
	catch (...) { }
	return json_value;
}

class UiSurface final {
	struct RenderedControl {
		ControlDefinition definition;
		wxPanel* container = nullptr;
		wxWindow* widget = nullptr;
	};

	struct RenderedTable {
		TableDefinition definition;
		wxListCtrl* widget = nullptr;
		std::vector<std::string> row_ids;
	};

	std::map<std::string, RenderedControl, std::less<>> controls;
	std::map<std::string, RenderedTable, std::less<>> tables;

	static int IntBound(std::optional<double> value, int fallback) {
		if (!value) return fallback;
		return static_cast<int>(std::clamp(
			*value,
			static_cast<double>(std::numeric_limits<int>::min()),
			static_cast<double>(std::numeric_limits<int>::max())));
	}

	wxWindow* CreateWidget(wxPanel* container, ControlDefinition const& definition) {
		switch (definition.kind) {
			case ControlKind::Label:
				return new wxStaticText(container, wxID_ANY,
					to_wx(definition.text.empty() ? definition.label : definition.text));
			case ControlKind::Text:
				return new wxTextCtrl(container, wxID_ANY, to_wx(definition.text));
			case ControlKind::MultilineText:
				return new wxTextCtrl(
					container, wxID_ANY, to_wx(definition.text), wxDefaultPosition,
					wxSize(-1, 90), wxTE_MULTILINE);
			case ControlKind::Integer: {
				auto* widget = new wxSpinCtrl(
					container, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
					wxSP_ARROW_KEYS,
					IntBound(definition.minimum, std::numeric_limits<int>::min()),
					IntBound(definition.maximum, std::numeric_limits<int>::max()),
					IntBound(definition.number, 0));
				return widget;
			}
			case ControlKind::Number: {
				auto* widget = new wxSpinCtrlDouble(
					container, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
					wxSP_ARROW_KEYS,
					definition.minimum.value_or(-1e12),
					definition.maximum.value_or(1e12),
					definition.number,
					definition.step.value_or(1.0));
				widget->SetDigits(6);
				return widget;
			}
			case ControlKind::Checkbox: {
				auto* widget = new wxCheckBox(container, wxID_ANY, to_wx(definition.label));
				widget->SetValue(definition.checked);
				return widget;
			}
			case ControlKind::Select: {
				wxArrayString choices;
				for (auto const& choice : definition.choices)
					choices.Add(to_wx(choice.label));
				auto* widget = new wxChoice(
					container, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
				auto selected = std::find_if(
					definition.choices.begin(), definition.choices.end(),
					[&](ChoiceDefinition const& choice) {
						return choice.id == definition.selected_choice_id;
					});
				if (selected != definition.choices.end())
					widget->SetSelection(static_cast<int>(selected - definition.choices.begin()));
				else if (!definition.choices.empty())
					widget->SetSelection(0);
				return widget;
			}
			case ControlKind::Progress: {
				auto minimum = definition.minimum.value_or(0.0);
				auto maximum = definition.maximum.value_or(100.0);
				auto range = std::max(1, static_cast<int>(std::lround(maximum - minimum)));
				auto value = std::clamp(
					static_cast<int>(std::lround(definition.number - minimum)), 0, range);
				auto* widget = new wxGauge(
					container, wxID_ANY, range, wxDefaultPosition, wxDefaultSize,
					wxGA_HORIZONTAL);
				widget->SetValue(value);
				return widget;
			}
		}
		throw std::logic_error("Unsupported declarative UI control kind");
	}

public:
	wxSizer* BuildControls(
		wxWindow* parent,
		std::vector<ControlDefinition> const& definitions) {
		auto* grid = new wxGridBagSizer(6, 8);
		int maximum_column = 0;
		for (auto const& definition : definitions) {
			auto* container = new wxPanel(parent);
			auto* item_sizer = new wxBoxSizer(
				definition.kind == ControlKind::MultilineText ? wxVERTICAL : wxHORIZONTAL);
			if (!definition.label.empty() && definition.kind != ControlKind::Label &&
				definition.kind != ControlKind::Checkbox)
				item_sizer->Add(
					new wxStaticText(container, wxID_ANY, to_wx(definition.label)),
					wxSizerFlags().CenterVertical().Border(wxRIGHT, 6));
			auto* widget = CreateWidget(container, definition);
			if (!widget)
				throw std::logic_error("Declarative UI renderer failed to create a control");
			item_sizer->Add(widget, wxSizerFlags(1).Expand());
			container->SetSizer(item_sizer);
			container->Enable(definition.enabled);
			container->Show(definition.visible);
			if (!definition.help.empty()) {
				container->SetToolTip(to_wx(definition.help));
				widget->SetToolTip(to_wx(definition.help));
			}
			grid->Add(
				container,
				wxGBPosition(definition.row, definition.column),
				wxGBSpan(1, definition.column_span),
				wxEXPAND);
			maximum_column = std::max(
				maximum_column, definition.column + definition.column_span - 1);
			controls.emplace(
				definition.id,
				RenderedControl{definition, container, widget});
		}
		for (int column = 0; column <= maximum_column; ++column)
			grid->AddGrowableCol(column, 1);
		return grid;
	}

	wxSizer* BuildTables(
		wxWindow* parent,
		std::vector<TableDefinition> const& definitions) {
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		for (auto const& definition : definitions) {
			long style = wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
				(definition.multi_select ? 0 : wxLC_SINGLE_SEL);
			auto* widget = new wxListCtrl(
				parent, wxID_ANY, wxDefaultPosition,
				wxSize(-1, definition.minimum_height), style);
			for (size_t index = 0; index < definition.columns.size(); ++index)
				widget->InsertColumn(
					static_cast<long>(index), to_wx(definition.columns[index].label),
					wxLIST_FORMAT_LEFT, definition.columns[index].width);
			RenderedTable table{definition, widget, {}};
			tables.emplace(definition.id, std::move(table));
			ReplaceRows(definition.id, definition.rows, true);
			sizer->Add(widget, wxSizerFlags(1).Expand().Border(wxTOP, 6));
		}
		return sizer;
	}

	std::vector<Value> CollectValues() const {
		std::vector<Value> values;
		for (auto const& [id, rendered] : controls) {
			auto* widget = rendered.widget;
			switch (rendered.definition.kind) {
				case ControlKind::Label:
					values.push_back({id, JsonString(from_wx(
						static_cast<wxStaticText*>(widget)->GetLabel()))});
					break;
				case ControlKind::Text:
				case ControlKind::MultilineText:
					values.push_back({id, JsonString(from_wx(
						static_cast<wxTextCtrl*>(widget)->GetValue()))});
					break;
				case ControlKind::Integer:
					values.push_back({id, JsonScalar(static_cast<int64_t>(
						static_cast<wxSpinCtrl*>(widget)->GetValue()))});
					break;
				case ControlKind::Number:
					values.push_back({id, JsonScalar(
						static_cast<wxSpinCtrlDouble*>(widget)->GetValue())});
					break;
				case ControlKind::Checkbox:
					values.push_back({id, JsonScalar(
						static_cast<wxCheckBox*>(widget)->GetValue())});
					break;
				case ControlKind::Select: {
					auto selection = static_cast<wxChoice*>(widget)->GetSelection();
					auto selected_id = selection == wxNOT_FOUND ? std::string() :
						rendered.definition.choices[static_cast<size_t>(selection)].id;
					values.push_back({id, JsonString(selected_id)});
					break;
				}
				case ControlKind::Progress:
					values.push_back({id, JsonScalar(
						rendered.definition.minimum.value_or(0.0) +
						static_cast<wxGauge*>(widget)->GetValue())});
					break;
			}
		}
		return values;
	}

	std::vector<std::string> SelectedRowIds() const {
		std::vector<std::string> selected;
		for (auto const& [id, table] : tables) {
			(void)id;
			long item = -1;
			while ((item = table.widget->GetNextItem(
				item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
				if (static_cast<size_t>(item) < table.row_ids.size())
					selected.push_back(table.row_ids[static_cast<size_t>(item)]);
			}
		}
		return selected;
	}

	bool ValidateRequired(wxWindow* dialog) const {
		for (auto const& [id, rendered] : controls) {
			(void)id;
			if (!rendered.definition.required || !rendered.container->IsShown()) continue;
			bool empty = false;
			if (rendered.definition.kind == ControlKind::Text ||
				rendered.definition.kind == ControlKind::MultilineText)
				empty = static_cast<wxTextCtrl*>(rendered.widget)->GetValue().empty();
			else if (rendered.definition.kind == ControlKind::Select)
				empty = static_cast<wxChoice*>(rendered.widget)->GetSelection() == wxNOT_FOUND;
			if (empty) {
				wxMessageBox(
					to_wx(rendered.definition.label.empty()
						? "A required field is empty."
						: rendered.definition.label + " is required."),
					wxS("Aegisub"), wxOK | wxICON_WARNING, dialog);
				rendered.widget->SetFocus();
				return false;
			}
		}
		return true;
	}

	void BindChanges(
		std::function<void(std::string const&)> control_callback,
		std::function<void(std::string const&)> table_callback) {
		for (auto const& [id, rendered] : controls) {
			auto handler = [control_callback, id](wxCommandEvent&) {
				control_callback(id);
			};
			switch (rendered.definition.kind) {
				case ControlKind::Text:
				case ControlKind::MultilineText:
					static_cast<wxTextCtrl*>(rendered.widget)->Bind(wxEVT_TEXT, handler);
					break;
				case ControlKind::Integer:
					static_cast<wxSpinCtrl*>(rendered.widget)->Bind(wxEVT_SPINCTRL, handler);
					break;
				case ControlKind::Number:
					static_cast<wxSpinCtrlDouble*>(rendered.widget)->Bind(
						wxEVT_SPINCTRLDOUBLE, handler);
					break;
				case ControlKind::Checkbox:
					static_cast<wxCheckBox*>(rendered.widget)->Bind(wxEVT_CHECKBOX, handler);
					break;
				case ControlKind::Select:
					static_cast<wxChoice*>(rendered.widget)->Bind(wxEVT_CHOICE, handler);
					break;
				default: break;
			}
		}
		for (auto const& [id, table] : tables) {
			auto handler = [table_callback, id](wxListEvent&) { table_callback(id); };
			table.widget->Bind(wxEVT_LIST_ITEM_SELECTED, handler);
			table.widget->Bind(wxEVT_LIST_ITEM_DESELECTED, handler);
		}
	}

	bool Apply(ControlPatch const& patch) {
		auto it = controls.find(patch.id);
		if (it == controls.end()) return false;
		auto& rendered = it->second;
		Validate(patch);
		if (patch.text) {
			if (auto* text = dynamic_cast<wxTextCtrl*>(rendered.widget))
				text->ChangeValue(to_wx(*patch.text));
			else if (auto* label = dynamic_cast<wxStaticText*>(rendered.widget))
				label->SetLabel(to_wx(*patch.text));
		}
		if (patch.checked)
			if (auto* checkbox = dynamic_cast<wxCheckBox*>(rendered.widget))
				checkbox->SetValue(*patch.checked);
		if (patch.number) {
			rendered.definition.number = *patch.number;
			if (auto* integer = dynamic_cast<wxSpinCtrl*>(rendered.widget))
				integer->SetValue(static_cast<int>(*patch.number));
			else if (auto* number = dynamic_cast<wxSpinCtrlDouble*>(rendered.widget))
				number->SetValue(*patch.number);
			else if (auto* progress = dynamic_cast<wxGauge*>(rendered.widget))
				progress->SetValue(std::clamp(
					static_cast<int>(std::lround(
						*patch.number - rendered.definition.minimum.value_or(0.0))),
					0, progress->GetRange()));
		}
		if (patch.selected_choice_id) {
			auto selected = std::find_if(
				rendered.definition.choices.begin(), rendered.definition.choices.end(),
				[&](ChoiceDefinition const& choice) {
					return choice.id == *patch.selected_choice_id;
				});
			if (auto* choice = dynamic_cast<wxChoice*>(rendered.widget))
				choice->SetSelection(selected == rendered.definition.choices.end()
					? wxNOT_FOUND
					: static_cast<int>(selected - rendered.definition.choices.begin()));
		}
		if (patch.enabled) rendered.container->Enable(*patch.enabled);
		if (patch.visible) rendered.container->Show(*patch.visible);
		return true;
	}

	void Validate(ControlPatch const& patch) const {
		auto it = controls.find(patch.id);
		if (it == controls.end())
			throw std::runtime_error(
				"ToolView patch references unknown control '" + patch.id + "'");
		auto const& rendered = it->second;
		auto kind = rendered.definition.kind;
		if (patch.text && kind != ControlKind::Label && kind != ControlKind::Text &&
			kind != ControlKind::MultilineText)
			throw std::runtime_error(
				"ToolView text patch is incompatible with control '" + patch.id + "'");
		if (patch.checked && kind != ControlKind::Checkbox)
			throw std::runtime_error(
				"ToolView checked patch is incompatible with control '" + patch.id + "'");
		if (patch.number && kind != ControlKind::Integer && kind != ControlKind::Number &&
			kind != ControlKind::Progress)
			throw std::runtime_error(
				"ToolView number patch is incompatible with control '" + patch.id + "'");
		if (patch.number && kind == ControlKind::Integer &&
			(!std::isfinite(*patch.number) || std::floor(*patch.number) != *patch.number ||
			*patch.number < std::numeric_limits<int>::min() ||
			*patch.number > std::numeric_limits<int>::max()))
			throw std::runtime_error(
				"ToolView integer patch is outside the native integer range");
		if (patch.number &&
			((rendered.definition.minimum && *patch.number < *rendered.definition.minimum) ||
			(rendered.definition.maximum && *patch.number > *rendered.definition.maximum)))
			throw std::runtime_error(
				"ToolView number patch is outside the control bounds");
		if (patch.selected_choice_id) {
			if (kind != ControlKind::Select)
				throw std::runtime_error(
					"ToolView choice patch is incompatible with control '" + patch.id + "'");
			if (std::none_of(
				rendered.definition.choices.begin(), rendered.definition.choices.end(),
				[&](ChoiceDefinition const& choice) {
					return choice.id == *patch.selected_choice_id;
				}))
				throw std::runtime_error(
					"ToolView patch selects an unknown choice for control '" + patch.id + "'");
		}
	}

	bool ReplaceRows(
		std::string const& table_id,
		std::vector<TableRowDefinition> const& rows,
		bool replace) {
		auto it = tables.find(table_id);
		if (it == tables.end()) return false;
		auto& table = it->second;
		std::set<std::string, std::less<>> row_ids;
		if (!replace)
			row_ids.insert(table.row_ids.begin(), table.row_ids.end());
		for (auto const& row : rows) {
			if (!row_ids.insert(row.id).second)
				throw std::runtime_error("Duplicate ToolView row ID '" + row.id + "'");
			for (auto const& cell : row.cells)
				if (std::none_of(
					table.definition.columns.begin(), table.definition.columns.end(),
					[&](TableColumnDefinition const& column) { return column.id == cell.id; }))
					throw std::runtime_error(
						"ToolView row references unknown column '" + cell.id + "'");
		}
		if (replace) {
			table.widget->DeleteAllItems();
			table.row_ids.clear();
		}
		for (auto const& row : rows) {
			auto index = table.widget->InsertItem(
				table.widget->GetItemCount(), wxEmptyString);
			for (size_t column = 0; column < table.definition.columns.size(); ++column) {
				auto cell = std::find_if(
					row.cells.begin(), row.cells.end(), [&](Value const& value) {
						return value.id == table.definition.columns[column].id;
					});
				if (cell != row.cells.end())
					table.widget->SetItem(
						index, static_cast<int>(column), to_wx(DisplayJsonValue(cell->json_value)));
			}
			table.row_ids.push_back(row.id);
		}
		return true;
	}

	void ValidateRows(
		std::string const& table_id,
		std::vector<TableRowDefinition> const& rows,
		bool replace) const {
		auto it = tables.find(table_id);
		if (it == tables.end())
			throw std::runtime_error(
				"ToolView patch references unknown table '" + table_id + "'");
		auto const& table = it->second;
		std::set<std::string, std::less<>> row_ids;
		if (!replace) row_ids.insert(table.row_ids.begin(), table.row_ids.end());
		for (auto const& row : rows) {
			if (!row_ids.insert(row.id).second)
				throw std::runtime_error("Duplicate ToolView row ID '" + row.id + "'");
			for (auto const& cell : row.cells)
				if (std::none_of(
					table.definition.columns.begin(), table.definition.columns.end(),
					[&](TableColumnDefinition const& column) { return column.id == cell.id; }))
					throw std::runtime_error(
						"ToolView row references unknown column '" + cell.id + "'");
		}
	}

	std::set<std::string, std::less<>> ProjectedRowIds(
		std::vector<TablePatch> const& patches) const {
		std::set<std::string, std::less<>> known_rows;
		for (auto const& [table_id, table] : tables) {
			auto patch = std::find_if(
				patches.begin(), patches.end(), [&](TablePatch const& candidate) {
					return candidate.id == table_id;
				});
			auto add_row = [&](std::string const& row_id) {
				if (!known_rows.insert(row_id).second)
					throw std::runtime_error(
						"Duplicate ToolView row ID across tables '" + row_id + "'");
			};
			if (patch == patches.end() || !patch->replace)
				for (auto const& row_id : table.row_ids) add_row(row_id);
			if (patch != patches.end())
				for (auto const& row : patch->rows) add_row(row.id);
		}
		return known_rows;
	}

	void SelectRows(std::vector<std::string> const& selected_ids) {
		for (auto& [id, table] : tables) {
			(void)id;
			for (size_t index = 0; index < table.row_ids.size(); ++index) {
				auto selected = std::find(
					selected_ids.begin(), selected_ids.end(), table.row_ids[index]) !=
					selected_ids.end();
				table.widget->SetItemState(
					static_cast<long>(index), selected ? wxLIST_STATE_SELECTED : 0,
					wxLIST_STATE_SELECTED);
			}
		}
	}
};

class FormDialog final : public wxDialog {
	FormDefinition definition;
	UiSurface surface;
	std::string action_id;
	bool cancelled = true;

public:
	FormDialog(wxWindow* parent, FormDefinition definition)
	: wxDialog(
		parent, wxID_ANY, to_wx(definition.title), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | (definition.resizable ? wxRESIZE_BORDER : 0))
	, definition(std::move(definition)) {
		auto* root = new wxBoxSizer(wxVERTICAL);
		root->Add(surface.BuildControls(this, this->definition.controls),
			wxSizerFlags(1).Expand().Border());
		auto* actions = new wxBoxSizer(wxHORIZONTAL);
		actions->AddStretchSpacer();
		for (auto const& action : this->definition.actions) {
			auto* button = new wxButton(this, wxWindow::NewControlId(), to_wx(action.label));
			button->Enable(action.enabled);
			if (action.is_default) button->SetDefault();
			if (action.is_cancel) SetEscapeId(button->GetId());
			button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) {
				if (!action.is_cancel && !surface.ValidateRequired(this)) return;
				action_id = action.id;
				cancelled = action.is_cancel;
				EndModal(action.is_cancel ? wxID_CANCEL : wxID_OK);
			});
			actions->Add(button, wxSizerFlags().Border(wxLEFT, 6));
		}
		root->Add(actions, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizerAndFit(root);
		SetMinSize(wxSize(
			this->definition.minimum_width > 0 ? this->definition.minimum_width : GetMinWidth(),
			this->definition.minimum_height > 0 ? this->definition.minimum_height : GetMinHeight()));
		CentreOnParent();
	}

	std::string ResultJson() const {
		return BuildFormResult(
			definition.id, action_id, surface.CollectValues(), cancelled);
	}
};

struct ViewKey {
	uint64_t plugin_handle = 0;
	std::string view_id;
	auto operator<=>(ViewKey const&) const = default;
};

class ToolViewDialog;

class ToolViewRegistry final {
	std::map<ViewKey, ToolViewDialog*> views;

public:
	static ToolViewRegistry& Get() {
		static ToolViewRegistry registry;
		return registry;
	}

	ToolViewDialog* Find(ViewKey const& key) const {
		auto it = views.find(key);
		return it == views.end() ? nullptr : it->second;
	}

	bool Add(ViewKey key, ToolViewDialog* dialog) {
		return views.emplace(std::move(key), dialog).second;
	}

	void Forget(ViewKey const& key, ToolViewDialog* dialog) {
		auto it = views.find(key);
		if (it != views.end() && it->second == dialog) views.erase(it);
	}

	std::vector<std::pair<ViewKey, ToolViewDialog*>> ForPlugin(uint64_t plugin_handle) const {
		std::vector<std::pair<ViewKey, ToolViewDialog*>> result;
		for (auto const& item : views)
			if (item.first.plugin_handle == plugin_handle) result.push_back(item);
		return result;
	}
};

class ToolViewDialog final : public wxDialog {
	ViewKey key;
	agi::Context* context;
	ToolViewDefinition definition;
	UiSurface surface;
	PluginEventDispatcher dispatch_event;
	wxNotebook* notebook = nullptr;
	int64_t revision = 0;
	bool applying_patch = false;
	bool suppress_close_event = false;
	std::string point_selection_owner;

	std::string ActiveTabId() const {
		if (!notebook) return {};
		auto selection = notebook->GetSelection();
		if (selection == wxNOT_FOUND ||
			static_cast<size_t>(selection) >= definition.tabs.size())
			return {};
		return definition.tabs[static_cast<size_t>(selection)].id;
	}

	int FindTab(std::string const& tab_id) const {
		auto tab = std::find_if(
			definition.tabs.begin(), definition.tabs.end(),
			[&](ToolViewTabDefinition const& candidate) {
				return candidate.id == tab_id;
			});
		return tab == definition.tabs.end()
			? wxNOT_FOUND
			: static_cast<int>(tab - definition.tabs.begin());
	}

	wxSizer* BuildActions(
		wxWindow* parent,
		std::vector<ActionDefinition> const& definitions) {
		auto* actions = new wxBoxSizer(wxHORIZONTAL);
		actions->AddStretchSpacer();
		for (auto const& action : definitions) {
			auto* button = new wxButton(
				parent, wxWindow::NewControlId(), to_wx(action.label));
			button->Enable(action.enabled);
			if (action.is_default) button->SetDefault();
			if (action.is_cancel) SetEscapeId(button->GetId());
			button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) {
				Emit("action", action.id);
				if (action.is_cancel) Close();
			});
			actions->Add(button, wxSizerFlags().Border(wxLEFT, 6));
		}
		return actions;
	}

	void Emit(std::string const& event_id, std::string const& source_id) noexcept {
		if (applying_patch || suppress_close_event) return;
		try {
			QueuePluginEvent(
				dispatch_event,
				key.plugin_handle,
				kToolViewEvent,
				BuildToolViewEvent(
					key.view_id, event_id, source_id, surface.CollectValues(),
					surface.SelectedRowIds(), revision, ActiveTabId()));
		}
		catch (std::exception const& error) {
			LOG_E("automation/plugin_bridge")
				<< "Could not dispatch ToolView event: " << error.what();
		}
	}

public:
	ToolViewDialog(
		ViewKey key,
		agi::Context* context,
		ToolViewDefinition definition,
		PluginEventDispatcher dispatch_event)
	: wxDialog(
		context->GetUI().parent, wxID_ANY, to_wx(definition.title),
		wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | (definition.resizable ? wxRESIZE_BORDER : 0))
	, key(std::move(key))
	, context(context)
	, definition(std::move(definition))
	, dispatch_event(std::move(dispatch_event)) {
		auto* root = new wxBoxSizer(wxVERTICAL);
		if (!this->definition.controls.empty())
			root->Add(surface.BuildControls(this, this->definition.controls),
				wxSizerFlags().Expand().Border());
		if (!this->definition.tables.empty())
			root->Add(surface.BuildTables(this, this->definition.tables),
				wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
		if (!this->definition.tabs.empty()) {
			notebook = new wxNotebook(this, wxID_ANY);
			for (auto const& tab : this->definition.tabs) {
				auto* panel = new wxPanel(notebook);
				auto* page = new wxBoxSizer(wxVERTICAL);
				if (!tab.controls.empty())
					page->Add(surface.BuildControls(panel, tab.controls),
						wxSizerFlags().Expand().Border());
				if (!tab.tables.empty())
					page->Add(surface.BuildTables(panel, tab.tables),
						wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
				if (!tab.actions.empty())
					page->Add(BuildActions(panel, tab.actions),
						wxSizerFlags().Expand().Border());
				panel->SetSizer(page);
				notebook->AddPage(panel, to_wx(tab.label));
			}
			root->Add(notebook, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));
		}
		if (!this->definition.actions.empty())
			root->Add(BuildActions(this, this->definition.actions),
				wxSizerFlags().Expand().Border());
		SetSizerAndFit(root);
		SetMinSize(wxSize(this->definition.minimum_width, this->definition.minimum_height));
		SetSize(wxSize(
			std::max(GetSize().GetWidth(), this->definition.minimum_width),
			std::max(GetSize().GetHeight(), this->definition.minimum_height)));
		surface.BindChanges(
			[this](std::string const& source_id) { Emit("change", source_id); },
			[this](std::string const& source_id) {
				Emit("selectionChanged", source_id);
			});
		if (notebook)
			notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& event) {
				event.Skip();
				Emit("tabChanged", ActiveTabId());
			});
		Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
			Emit("closed", this->key.view_id);
			if (!IsBeingDeleted()) Destroy();
		});
	}

	~ToolViewDialog() override {
		if (!point_selection_owner.empty() && context && context->GetUI().videoDisplay)
			context->GetUI().videoDisplay->CancelPointSelection(
				point_selection_owner, false);
		ToolViewRegistry::Get().Forget(key, this);
	}

	agi::Context* Context() const { return context; }

	bool ApplyPatch(ToolViewPatch const& patch) {
		if (patch.revision <= revision) return false;
		applying_patch = true;
		try {
			if (patch.selected_tab_id && FindTab(*patch.selected_tab_id) == wxNOT_FOUND)
				throw std::runtime_error(
					"ToolView patch selects unknown tab '" + *patch.selected_tab_id + "'");
			for (auto const& control : patch.controls) surface.Validate(control);
			for (auto const& table : patch.tables)
				surface.ValidateRows(table.id, table.rows, table.replace);
			auto projected_rows = surface.ProjectedRowIds(patch.tables);
			if (patch.selected_row_ids)
				for (auto const& selected_id : *patch.selected_row_ids)
					if (!projected_rows.contains(selected_id))
						throw std::runtime_error(
							"ToolView selection references unknown row '" + selected_id + "'");

			for (auto const& control : patch.controls)
				surface.Apply(control);
			for (auto const& table : patch.tables)
				surface.ReplaceRows(table.id, table.rows, table.replace);
			if (patch.selected_row_ids) {
				surface.SelectRows(*patch.selected_row_ids);
			}
			if (patch.selected_tab_id)
				notebook->ChangeSelection(FindTab(*patch.selected_tab_id));
			revision = patch.revision;
			Layout();
			FitInside();
			applying_patch = false;
			return true;
		}
		catch (...) {
			applying_patch = false;
			throw;
		}
	}

	void CloseFromHost() {
		suppress_close_event = true;
		if (!point_selection_owner.empty() && context && context->GetUI().videoDisplay) {
			context->GetUI().videoDisplay->CancelPointSelection(point_selection_owner, false);
			point_selection_owner.clear();
		}
		Destroy();
	}

	bool BeginPointSelection(VideoPointSelectionRequest const& request) {
		auto* display = context ? context->GetUI().videoDisplay : nullptr;
		if (!display) return false;
		point_selection_owner = std::to_string(key.plugin_handle) + ":" +
			key.view_id + ":" + request.session_id;
		auto owner = point_selection_owner;
		auto dispatcher = dispatch_event;
		auto plugin_handle = key.plugin_handle;
		auto view_id = key.view_id;
		auto session_id = request.session_id;
		auto include_distance = request.include_distance;
		display->BeginPointSelection(
			owner,
			request.point_count,
			request.coordinate_space == "script",
			[dispatcher = std::move(dispatcher), plugin_handle, view_id,
				session_id = std::move(session_id), include_distance](
					std::vector<std::pair<double, double>> points,
					int frame,
					bool cancelled) {
				QueuePluginEvent(
					dispatcher,
					plugin_handle,
					kVideoPointSelectionEvent,
					BuildVideoPointSelectionEvent(
						view_id, session_id, points, frame, cancelled, include_distance));
			});
		return true;
	}
};

ToolViewDialog* RequireView(uint64_t plugin_handle, std::string const& view_id) {
	auto* view = ToolViewRegistry::Get().Find({plugin_handle, view_id});
	if (!view)
		throw std::runtime_error("Declarative ToolView '" + view_id + "' is not open");
	return view;
}

} // namespace

std::optional<std::string> InvokeDeclarativeUiHostService(
	uint64_t plugin_handle,
	Context* invocation_context,
	std::string const& service_id,
	std::string const& request_json,
	PluginEventDispatcher dispatch_event) {
	if (service_id != kOpenFormService && service_id != kOpenToolViewService &&
		service_id != kPatchToolViewService && service_id != kCloseToolViewService &&
		service_id != kBeginVideoPointSelectionService)
		return std::nullopt;
	if (service_id == kOpenToolViewService &&
		(!invocation_context || !invocation_context->GetUI().parent)) {
		auto definition = ParseOpenToolViewRequest(request_json);
		return BuildToolViewOperationResult(definition.id, false, "unavailable");
	}

	struct MainThreadResult {
		std::string value;
		std::exception_ptr error;
	};
	auto result = agi::ui::MainInvoke([
		=, dispatch_event = std::move(dispatch_event)]() mutable -> MainThreadResult {
		try {
			auto invoke = [&]() -> std::string {
				if (service_id == kOpenFormService) {
					auto request = ParseOpenFormRequest(request_json);
					wxWindow* parent = nullptr;
					if (!request.owner_view_id.empty())
						parent = RequireView(plugin_handle, request.owner_view_id);
					else if (invocation_context && invocation_context->GetUI().parent)
						parent = invocation_context->GetUI().parent;
					else
						throw std::runtime_error(
							"Opening a Form requires an active Aegisub UI or owner ToolView");
					FormDialog dialog(parent, std::move(request.definition));
					dialog.ShowModal();
					return dialog.ResultJson();
				}
				if (service_id == kOpenToolViewService) {
					auto definition = ParseOpenToolViewRequest(request_json);
					if (definition.placement == "docked")
						LOG_I("automation/plugin_bridge")
							<< "ToolView '" << definition.id
							<< "' requested Docked placement; using the Floating renderer";
					ViewKey key{plugin_handle, definition.id};
					if (auto* existing = ToolViewRegistry::Get().Find(key)) {
						existing->Show();
						existing->Raise();
						return BuildToolViewOperationResult(
							definition.id, false, "alreadyOpen");
					}
					auto* dialog = new ToolViewDialog(
						key,
						invocation_context,
						std::move(definition),
						std::move(dispatch_event));
					if (!ToolViewRegistry::Get().Add(key, dialog)) {
						dialog->Destroy();
						return BuildToolViewOperationResult(
							key.view_id, false, "alreadyOpen");
					}
					dialog->Show();
					SetFloatOnParent(dialog);
					dialog->CentreOnParent();
					dialog->Raise();
					return BuildToolViewOperationResult(key.view_id, true, "opened");
				}
				if (service_id == kPatchToolViewService) {
					auto patch = ParseToolViewPatchRequest(request_json);
					auto succeeded =
						RequireView(plugin_handle, patch.view_id)->ApplyPatch(patch);
					return BuildToolViewOperationResult(
						patch.view_id,
						succeeded,
						succeeded ? "patched" : "staleRevision");
				}
				if (service_id == kCloseToolViewService) {
					auto view_id = ParseCloseToolViewRequest(request_json);
					auto* view = RequireView(plugin_handle, view_id);
					view->CloseFromHost();
					return BuildToolViewOperationResult(view_id, true, "closed");
				}

				auto request = ParseVideoPointSelectionRequest(request_json);
				auto succeeded =
					RequireView(plugin_handle, request.view_id)->BeginPointSelection(request);
				return BuildToolViewOperationResult(
					request.view_id,
					succeeded,
					succeeded ? "started" : "unavailable");
			};
			return {invoke(), {}};
		}
		catch (...) {
			return {{}, std::current_exception()};
		}
	});
	if (result.error) std::rethrow_exception(result.error);
	return std::move(result.value);
}

void CloseDeclarativeUiViewsForPlugin(uint64_t plugin_handle) {
	agi::ui::MainInvoke([=] {
		for (auto const& [key, dialog] : ToolViewRegistry::Get().ForPlugin(plugin_handle)) {
			(void)key;
			dialog->CloseFromHost();
		}
	});
}

} // namespace agi::coreclr::ui
