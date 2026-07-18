#include "declarative_ui_model.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace agi::coreclr::ui {
namespace {

json::Object const& AsObject(json::UnknownElement const& value, std::string const& source) {
	try { return static_cast<json::Object const&>(value); }
	catch (...) { throw std::runtime_error(source + " must be an object"); }
}

json::Array const& AsArray(json::UnknownElement const& value, std::string const& source) {
	try { return static_cast<json::Array const&>(value); }
	catch (...) { throw std::runtime_error(source + " must be an array"); }
}

bool IsNull(json::UnknownElement const& value) {
	try {
		(void)static_cast<json::Null const&>(value);
		return true;
	}
	catch (...) {
		return false;
	}
}

json::Object const& RequireObject(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) throw std::runtime_error("Declarative UI requires '" + key + "'");
	return AsObject(it->second, "Declarative UI field '" + key + "'");
}

json::Array const& RequireArray(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) throw std::runtime_error("Declarative UI requires '" + key + "'");
	return AsArray(it->second, "Declarative UI field '" + key + "'");
}

std::string RequireString(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) throw std::runtime_error("Declarative UI requires '" + key + "'");
	try {
		auto result = static_cast<json::String const&>(it->second);
		if (result.empty()) throw std::runtime_error("Declarative UI field '" + key + "' cannot be empty");
		return result;
	}
	catch (std::runtime_error const&) { throw; }
	catch (...) { throw std::runtime_error("Declarative UI field '" + key + "' must be a string"); }
}

std::optional<std::string> FindString(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end() || IsNull(it->second)) return std::nullopt;
	try { return static_cast<json::String const&>(it->second); }
	catch (...) { throw std::runtime_error("Declarative UI field '" + key + "' must be a string"); }
}

std::optional<bool> FindBool(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end() || IsNull(it->second)) return std::nullopt;
	try { return static_cast<json::Boolean const&>(it->second); }
	catch (...) { throw std::runtime_error("Declarative UI field '" + key + "' must be boolean"); }
}

std::optional<double> FindNumber(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end() || IsNull(it->second)) return std::nullopt;
	try { return static_cast<json::Double const&>(it->second); }
	catch (...) {
		try { return static_cast<double>(static_cast<json::Integer const&>(it->second)); }
		catch (...) { throw std::runtime_error("Declarative UI field '" + key + "' must be numeric"); }
	}
}

int64_t FindInt64(json::Object const& object, std::string const& key, int64_t fallback) {
	auto it = object.find(key);
	if (it == object.end() || IsNull(it->second)) return fallback;
	try { return static_cast<json::Integer const&>(it->second); }
	catch (...) { }
	double number = 0.0;
	try { number = static_cast<json::Double const&>(it->second); }
	catch (...) {
		throw std::runtime_error("Declarative UI field '" + key + "' must be an integer");
	}
	if (!std::isfinite(number) || number < -9223372036854775808.0 ||
		number >= 9223372036854775808.0 || std::floor(number) != number)
		throw std::runtime_error("Declarative UI field '" + key + "' must be an integer");
	return static_cast<int64_t>(number);
}

int FindInt(json::Object const& object, std::string const& key, int fallback) {
	auto number = FindInt64(object, key, fallback);
	if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
		throw std::runtime_error("Declarative UI field '" + key + "' is outside the native integer range");
	return static_cast<int>(number);
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
	});
	return value;
}

ControlKind ParseControlKind(std::string value) {
	value = Lower(std::move(value));
	if (value == "label") return ControlKind::Label;
	if (value == "text") return ControlKind::Text;
	if (value == "multilinetext") return ControlKind::MultilineText;
	if (value == "integer") return ControlKind::Integer;
	if (value == "number") return ControlKind::Number;
	if (value == "checkbox") return ControlKind::Checkbox;
	if (value == "select") return ControlKind::Select;
	if (value == "progress") return ControlKind::Progress;
	throw std::runtime_error("Unsupported declarative UI control kind '" + value + "'");
}

void ValidateGrid(int row, int column, int span, std::string const& id) {
	if (row < 0 || row > 1024 || column < 0 || column > 1024 ||
		span <= 0 || span > 64 || column > 1024 - span)
		throw std::runtime_error("Declarative UI control '" + id + "' has an invalid grid position");
}

void ValidateControlLayout(std::vector<ControlDefinition> const& controls) {
	std::set<std::pair<int, int>> occupied;
	for (auto const& control : controls) {
		for (int column = control.column;
			column < control.column + control.column_span;
			++column) {
			if (!occupied.emplace(control.row, column).second)
				throw std::runtime_error(
					"Declarative UI controls overlap at row " +
					std::to_string(control.row) + ", column " +
					std::to_string(column));
		}
	}
}

void AddSurfaceIds(
	std::vector<ControlDefinition> const& controls,
	std::vector<TableDefinition> const& tables,
	std::vector<ActionDefinition> const& actions,
	std::set<std::string, std::less<>>& ids) {
	auto add = [&](std::string const& id) {
		if (!ids.insert(id).second)
			throw std::runtime_error("Duplicate declarative UI source ID '" + id + "'");
	};
	for (auto const& control : controls) add(control.id);
	for (auto const& table : tables) add(table.id);
	for (auto const& action : actions) add(action.id);
}

void ValidateSurfaceIds(
	std::vector<ControlDefinition> const& controls,
	std::vector<TableDefinition> const& tables,
	std::vector<ActionDefinition> const& actions) {
	std::set<std::string, std::less<>> ids;
	AddSurfaceIds(controls, tables, actions, ids);
}

void AddGlobalRowIds(
	std::vector<TableDefinition> const& tables,
	std::set<std::string, std::less<>>& ids) {
	for (auto const& table : tables)
		for (auto const& row : table.rows)
			if (!ids.insert(row.id).second)
				throw std::runtime_error(
					"Duplicate ToolView row ID across tables '" + row.id + "'");
}

ControlDefinition ParseControl(json::Object const& object) {
	ControlDefinition control;
	control.id = RequireString(object, "id");
	control.kind = ParseControlKind(RequireString(object, "kind"));
	control.label = FindString(object, "label").value_or("");
	control.text = FindString(object, "text").value_or("");
	control.checked = FindBool(object, "checked").value_or(false);
	control.number = FindNumber(object, "number").value_or(0.0);
	control.minimum = FindNumber(object, "minimum");
	control.maximum = FindNumber(object, "maximum");
	control.step = FindNumber(object, "step");
	control.selected_choice_id = FindString(object, "selectedChoiceId").value_or("");
	control.help = FindString(object, "help").value_or("");
	control.row = FindInt(object, "row", 0);
	control.column = FindInt(object, "column", 0);
	control.column_span = FindInt(object, "columnSpan", 1);
	control.enabled = FindBool(object, "enabled").value_or(true);
	control.visible = FindBool(object, "visible").value_or(true);
	control.required = FindBool(object, "required").value_or(false);
	ValidateGrid(control.row, control.column, control.column_span, control.id);
	if (control.minimum && control.maximum && *control.minimum > *control.maximum)
		throw std::runtime_error("Declarative UI control '" + control.id + "' has minimum above maximum");
	if (control.step && *control.step <= 0.0)
		throw std::runtime_error("Declarative UI control '" + control.id + "' requires a positive step");
	if ((control.minimum && control.number < *control.minimum) ||
		(control.maximum && control.number > *control.maximum))
		throw std::runtime_error("Declarative UI control '" + control.id + "' has a value outside its bounds");
	if (control.kind == ControlKind::Integer) {
		auto valid_integer = [](double value) {
			return std::isfinite(value) && std::floor(value) == value &&
				value >= std::numeric_limits<int>::min() &&
				value <= std::numeric_limits<int>::max();
		};
		if (!valid_integer(control.number) ||
			(control.minimum && !valid_integer(*control.minimum)) ||
			(control.maximum && !valid_integer(*control.maximum)) ||
			(control.step && !valid_integer(*control.step)))
			throw std::runtime_error(
				"Declarative UI integer control '" + control.id + "' requires native integer values");
	}
	if (control.kind == ControlKind::Progress && control.minimum && control.maximum &&
		*control.maximum - *control.minimum > std::numeric_limits<int>::max())
		throw std::runtime_error(
			"Declarative UI progress control '" + control.id + "' range is too large");
	if (auto choices = object.find("choices"); choices != object.end() &&
		!IsNull(choices->second)) {
		std::set<std::string, std::less<>> ids;
		for (auto const& item : AsArray(choices->second, "Declarative UI choices")) {
			auto const& choice = AsObject(item, "Declarative UI choice");
			ChoiceDefinition parsed{RequireString(choice, "id"), RequireString(choice, "label")};
			if (!ids.insert(parsed.id).second)
				throw std::runtime_error("Duplicate declarative UI choice ID '" + parsed.id + "'");
			control.choices.emplace_back(std::move(parsed));
		}
	}
	if (control.kind == ControlKind::Select && control.choices.empty())
		throw std::runtime_error("Declarative UI select control '" + control.id + "' requires choices");
	if (control.kind == ControlKind::Select && !control.selected_choice_id.empty() &&
		std::none_of(control.choices.begin(), control.choices.end(), [&](ChoiceDefinition const& choice) {
			return choice.id == control.selected_choice_id;
		}))
		throw std::runtime_error(
			"Declarative UI select control '" + control.id + "' has an unknown selected choice");
	return control;
}

ActionDefinition ParseAction(json::Object const& object) {
	return {
		RequireString(object, "id"),
		RequireString(object, "label"),
		FindBool(object, "isDefault").value_or(false),
		FindBool(object, "isCancel").value_or(false),
		FindBool(object, "enabled").value_or(true)
	};
}

Value ParseValue(json::Object const& object) {
	auto value = Value{RequireString(object, "id"), RequireString(object, "jsonValue")};
	std::istringstream stream(value.json_value);
	json::UnknownElement parsed;
	json::Reader::Read(parsed, stream);
	return value;
}

TableRowDefinition ParseRow(json::Object const& object) {
	TableRowDefinition row;
	row.id = RequireString(object, "id");
	std::set<std::string, std::less<>> cell_ids;
	for (auto const& item : RequireArray(object, "cells")) {
		auto value = ParseValue(AsObject(item, "ToolView table cell"));
		if (!cell_ids.insert(value.id).second)
			throw std::runtime_error("Duplicate ToolView table cell ID '" + value.id + "'");
		row.cells.emplace_back(std::move(value));
	}
	return row;
}

TableDefinition ParseTable(json::Object const& object) {
	TableDefinition table;
	table.id = RequireString(object, "id");
	std::set<std::string, std::less<>> column_ids;
	for (auto const& item : RequireArray(object, "columns")) {
		auto const& column = AsObject(item, "ToolView column");
		TableColumnDefinition parsed{
			RequireString(column, "id"),
			RequireString(column, "label"),
			FindInt(column, "width", 120)
		};
		if (parsed.width <= 0 || parsed.width > 32768 ||
			!column_ids.insert(parsed.id).second)
			throw std::runtime_error("ToolView table has an invalid or duplicate column");
		table.columns.emplace_back(std::move(parsed));
	}
	if (table.columns.empty()) throw std::runtime_error("ToolView table requires at least one column");
	std::set<std::string, std::less<>> row_ids;
	for (auto const& item : RequireArray(object, "rows")) {
		auto row = ParseRow(AsObject(item, "ToolView row"));
		if (!row_ids.insert(row.id).second)
			throw std::runtime_error("Duplicate ToolView row ID '" + row.id + "'");
		for (auto const& cell : row.cells)
			if (!column_ids.contains(cell.id))
				throw std::runtime_error(
					"ToolView row references unknown column '" + cell.id + "'");
		table.rows.emplace_back(std::move(row));
	}
	table.multi_select = FindBool(object, "multiSelect").value_or(false);
	table.minimum_height = FindInt(object, "minimumHeight", 120);
	if (table.minimum_height < 0 || table.minimum_height > 32768)
		throw std::runtime_error("ToolView table minimumHeight is outside the supported range");
	return table;
}

template<typename Item, typename Parse>
std::vector<Item> ParseUniqueItems(
	json::Array const& items,
	std::string const& source,
	Parse&& parse) {
	std::vector<Item> result;
	std::set<std::string, std::less<>> ids;
	for (auto const& item : items) {
		auto parsed = parse(AsObject(item, source));
		if (!ids.insert(parsed.id).second)
			throw std::runtime_error("Duplicate declarative UI ID '" + parsed.id + "'");
		result.emplace_back(std::move(parsed));
	}
	return result;
}

ToolViewTabDefinition ParseToolViewTab(json::Object const& object) {
	ToolViewTabDefinition tab;
	tab.id = RequireString(object, "id");
	tab.label = RequireString(object, "label");
	tab.controls = ParseUniqueItems<ControlDefinition>(
		RequireArray(object, "controls"), "ToolView tab control", ParseControl);
	tab.tables = ParseUniqueItems<TableDefinition>(
		RequireArray(object, "tables"), "ToolView tab table", ParseTable);
	tab.actions = ParseUniqueItems<ActionDefinition>(
		RequireArray(object, "actions"), "ToolView tab action", ParseAction);
	return tab;
}

json::Object ParseRoot(std::string const& input) {
	std::istringstream stream(input);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	try {
		auto& object = static_cast<json::Object&>(root);
		return std::move(object);
	}
	catch (...) {
		throw std::runtime_error("Declarative UI request must be an object");
	}
}

void ValidateSchema(json::Object const& object) {
	if (FindInt(object, "schemaVersion", 1) != 1)
		throw std::runtime_error("Unsupported declarative UI schemaVersion");
}

json::UnknownElement ParseJsonValue(std::string const& input) {
	std::istringstream stream(input);
	json::UnknownElement value;
	json::Reader::Read(value, stream);
	return value;
}

template<typename T>
std::string WriteJson(T const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

json::Array ValuesToJson(std::vector<Value> const& values) {
	json::Array result;
	for (auto const& value : values) {
		json::Object item;
		item["id"] = value.id;
		item["jsonValue"] = value.json_value;
		result.emplace_back(std::move(item));
	}
	return result;
}

} // namespace

OpenFormRequest ParseOpenFormRequest(std::string const& input) {
	auto root = ParseRoot(input);
	auto const& object = RequireObject(root, "definition");
	ValidateSchema(object);
	FormDefinition form;
	form.id = RequireString(object, "id");
	form.title = RequireString(object, "title");
	form.controls = ParseUniqueItems<ControlDefinition>(
		RequireArray(object, "controls"), "Form control", ParseControl);
	form.actions = ParseUniqueItems<ActionDefinition>(
		RequireArray(object, "actions"), "Form action", ParseAction);
	form.minimum_width = FindInt(object, "minimumWidth", 0);
	form.minimum_height = FindInt(object, "minimumHeight", 0);
	form.resizable = FindBool(object, "resizable").value_or(true);
	if (form.minimum_width < 0 || form.minimum_width > 32768 ||
		form.minimum_height < 0 || form.minimum_height > 32768)
		throw std::runtime_error("Form minimum dimensions are outside the supported range");
	ValidateControlLayout(form.controls);
	ValidateSurfaceIds(form.controls, {}, form.actions);
	if (std::count_if(form.actions.begin(), form.actions.end(), [](auto const& action) {
		return action.is_default;
	}) > 1 || std::count_if(form.actions.begin(), form.actions.end(), [](auto const& action) {
		return action.is_cancel;
	}) > 1)
		throw std::runtime_error("Form may declare at most one default and one cancel action");
	return {std::move(form), FindString(root, "ownerViewId").value_or("")};
}

ToolViewDefinition ParseOpenToolViewRequest(std::string const& input) {
	auto root = ParseRoot(input);
	auto const& object = RequireObject(root, "definition");
	ValidateSchema(object);
	ToolViewDefinition view;
	view.id = RequireString(object, "id");
	view.title = RequireString(object, "title");
	view.controls = ParseUniqueItems<ControlDefinition>(
		RequireArray(object, "controls"), "ToolView control", ParseControl);
	view.tables = ParseUniqueItems<TableDefinition>(
		RequireArray(object, "tables"), "ToolView table", ParseTable);
	view.actions = ParseUniqueItems<ActionDefinition>(
		RequireArray(object, "actions"), "ToolView action", ParseAction);
	if (auto tabs = object.find("tabs"); tabs != object.end() && !IsNull(tabs->second))
		view.tabs = ParseUniqueItems<ToolViewTabDefinition>(
			AsArray(tabs->second, "ToolView tabs"), "ToolView tab", ParseToolViewTab);
	view.placement = Lower(FindString(object, "placement").value_or("auto"));
	if (view.placement != "auto" && view.placement != "floating" && view.placement != "docked")
		throw std::runtime_error("Unsupported ToolView placement '" + view.placement + "'");
	view.minimum_width = FindInt(object, "minimumWidth", 360);
	view.minimum_height = FindInt(object, "minimumHeight", 240);
	view.resizable = FindBool(object, "resizable").value_or(true);
	if (view.minimum_width < 0 || view.minimum_width > 32768 ||
		view.minimum_height < 0 || view.minimum_height > 32768)
		throw std::runtime_error("ToolView minimum dimensions are outside the supported range");
	std::set<std::string, std::less<>> source_ids;
	ValidateControlLayout(view.controls);
	AddSurfaceIds(view.controls, view.tables, view.actions, source_ids);
	std::set<std::string, std::less<>> row_ids;
	AddGlobalRowIds(view.tables, row_ids);
	int default_actions = static_cast<int>(std::count_if(
		view.actions.begin(), view.actions.end(), [](auto const& action) {
			return action.is_default;
		}));
	int cancel_actions = static_cast<int>(std::count_if(
		view.actions.begin(), view.actions.end(), [](auto const& action) {
			return action.is_cancel;
		}));
	for (auto const& tab : view.tabs) {
		if (!source_ids.insert(tab.id).second)
			throw std::runtime_error(
				"Duplicate declarative UI source ID '" + tab.id + "'");
		ValidateControlLayout(tab.controls);
		AddSurfaceIds(tab.controls, tab.tables, tab.actions, source_ids);
		AddGlobalRowIds(tab.tables, row_ids);
		default_actions += static_cast<int>(std::count_if(
			tab.actions.begin(), tab.actions.end(), [](auto const& action) {
				return action.is_default;
			}));
		cancel_actions += static_cast<int>(std::count_if(
			tab.actions.begin(), tab.actions.end(), [](auto const& action) {
				return action.is_cancel;
			}));
	}
	if (default_actions > 1 || cancel_actions > 1)
		throw std::runtime_error("ToolView may declare at most one default and one cancel action");
	return view;
}

ToolViewPatch ParseToolViewPatchRequest(std::string const& input) {
	auto root = ParseRoot(input);
	auto const& object = RequireObject(root, "patch");
	ValidateSchema(object);
	ToolViewPatch patch;
	patch.view_id = RequireString(object, "viewId");
	patch.revision = FindInt64(object, "revision", 0);
	if (patch.revision <= 0) throw std::runtime_error("ToolView patch revision must be positive");
	std::set<std::string, std::less<>> control_ids;
	for (auto const& item : RequireArray(object, "controls")) {
		auto const& value = AsObject(item, "ToolView control patch");
		ControlPatch parsed;
		parsed.id = RequireString(value, "id");
		parsed.text = FindString(value, "text");
		parsed.checked = FindBool(value, "checked");
		parsed.number = FindNumber(value, "number");
		parsed.selected_choice_id = FindString(value, "selectedChoiceId");
		parsed.enabled = FindBool(value, "enabled");
		parsed.visible = FindBool(value, "visible");
		if (!control_ids.insert(parsed.id).second)
			throw std::runtime_error("Duplicate ToolView control patch ID '" + parsed.id + "'");
		patch.controls.emplace_back(std::move(parsed));
	}
	std::set<std::string, std::less<>> table_ids;
	for (auto const& item : RequireArray(object, "tables")) {
		auto const& value = AsObject(item, "ToolView table patch");
		TablePatch parsed;
		parsed.id = RequireString(value, "id");
		if (!table_ids.insert(parsed.id).second)
			throw std::runtime_error("Duplicate ToolView table patch ID '" + parsed.id + "'");
		parsed.replace = FindBool(value, "replace").value_or(true);
		std::set<std::string, std::less<>> row_ids;
		for (auto const& row : RequireArray(value, "rows"))
		{
			auto parsed_row = ParseRow(AsObject(row, "ToolView patch row"));
			if (!row_ids.insert(parsed_row.id).second)
				throw std::runtime_error("Duplicate ToolView patch row ID '" + parsed_row.id + "'");
			parsed.rows.emplace_back(std::move(parsed_row));
		}
		patch.tables.emplace_back(std::move(parsed));
	}
	if (auto selected = object.find("selectedRowIds"); selected != object.end() &&
		!IsNull(selected->second)) {
		patch.selected_row_ids.emplace();
		std::set<std::string, std::less<>> selected_ids;
		for (auto const& item : AsArray(selected->second, "selectedRowIds")) {
			auto id = static_cast<json::String const&>(item);
			if (!selected_ids.insert(id).second)
				throw std::runtime_error("Duplicate selected ToolView row ID '" + id + "'");
			patch.selected_row_ids->emplace_back(std::move(id));
		}
	}
	patch.selected_tab_id = FindString(object, "selectedTabId");
	if (patch.selected_tab_id && patch.selected_tab_id->empty())
		throw std::runtime_error("ToolView selectedTabId cannot be empty");
	return patch;
}

std::string ParseCloseToolViewRequest(std::string const& input) {
	return RequireString(ParseRoot(input), "viewId");
}

VideoPointSelectionRequest ParseVideoPointSelectionRequest(std::string const& input) {
	auto object = ParseRoot(input);
	VideoPointSelectionRequest request;
	request.view_id = RequireString(object, "viewId");
	request.session_id = RequireString(object, "sessionId");
	request.point_count = FindInt(object, "pointCount", 2);
	request.coordinate_space = Lower(FindString(object, "coordinateSpace").value_or("script"));
	request.include_distance = FindBool(object, "includeDistance").value_or(true);
	if (request.point_count < 1 || request.point_count > 16)
		throw std::runtime_error("Video point selection pointCount must be between 1 and 16");
	if (request.coordinate_space != "script" && request.coordinate_space != "frame")
		throw std::runtime_error("Unsupported video point-selection coordinate space");
	return request;
}

std::string BuildToolViewOperationResult(
	std::string const& view_id,
	bool succeeded,
	std::string const& status) {
	json::Object root;
	root["viewId"] = view_id;
	root["succeeded"] = succeeded;
	if (!status.empty()) root["status"] = status;
	return WriteJson(root);
}

std::string BuildFormResult(
	std::string const& form_id,
	std::string const& action_id,
	std::vector<Value> const& values,
	bool cancelled) {
	json::Object root;
	root["formId"] = form_id;
	root["actionId"] = action_id;
	root["values"] = ValuesToJson(values);
	root["cancelled"] = cancelled;
	root["schemaVersion"] = int64_t{1};
	return WriteJson(root);
}

std::string BuildToolViewEvent(
	std::string const& view_id,
	std::string const& event_id,
	std::string const& source_id,
	std::vector<Value> const& values,
	std::vector<std::string> const& selected_row_ids,
	int64_t revision,
	std::string const& active_tab_id) {
	json::Object root;
	root["viewId"] = view_id;
	root["eventId"] = event_id;
	root["sourceId"] = source_id;
	root["values"] = ValuesToJson(values);
	json::Array selected;
	for (auto const& id : selected_row_ids) selected.emplace_back(id);
	root["selectedRowIds"] = std::move(selected);
	root["revision"] = revision;
	root["activeTabId"] = active_tab_id;
	root["schemaVersion"] = int64_t{1};
	return WriteJson(root);
}

std::string BuildVideoPointSelectionEvent(
	std::string const& view_id,
	std::string const& session_id,
	std::vector<std::pair<double, double>> const& points,
	int frame,
	bool cancelled,
	bool include_distance) {
	json::Object root;
	root["viewId"] = view_id;
	root["sessionId"] = session_id;
	json::Array point_values;
	for (auto const& [x, y] : points) {
		json::Object point;
		point["x"] = x;
		point["y"] = y;
		point_values.emplace_back(std::move(point));
	}
	root["points"] = std::move(point_values);
	double delta_x = 0.0;
	double delta_y = 0.0;
	double distance = 0.0;
	if (include_distance && points.size() >= 2) {
		delta_x = points.back().first - points.front().first;
		delta_y = points.back().second - points.front().second;
		distance = std::hypot(delta_x, delta_y);
	}
	root["deltaX"] = delta_x;
	root["deltaY"] = delta_y;
	root["distance"] = distance;
	root["frame"] = int64_t{frame};
	root["cancelled"] = cancelled;
	return WriteJson(root);
}

} // namespace agi::coreclr::ui
