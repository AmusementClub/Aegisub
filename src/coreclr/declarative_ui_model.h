#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agi::coreclr::ui {

enum class ControlKind {
	Label,
	Text,
	MultilineText,
	Integer,
	Number,
	Checkbox,
	Select,
	Progress
};

struct ChoiceDefinition {
	std::string id;
	std::string label;
};

struct ControlDefinition {
	std::string id;
	ControlKind kind = ControlKind::Label;
	std::string label;
	std::string text;
	bool checked = false;
	double number = 0.0;
	std::optional<double> minimum;
	std::optional<double> maximum;
	std::optional<double> step;
	std::vector<ChoiceDefinition> choices;
	std::string selected_choice_id;
	std::string help;
	int row = 0;
	int column = 0;
	int column_span = 1;
	bool enabled = true;
	bool visible = true;
	bool required = false;
};

struct ActionDefinition {
	std::string id;
	std::string label;
	bool is_default = false;
	bool is_cancel = false;
	bool enabled = true;
};

struct Value {
	std::string id;
	std::string json_value;
};

struct TableColumnDefinition {
	std::string id;
	std::string label;
	int width = 120;
};

struct TableRowDefinition {
	std::string id;
	std::vector<Value> cells;
};

struct TableDefinition {
	std::string id;
	std::vector<TableColumnDefinition> columns;
	std::vector<TableRowDefinition> rows;
	bool multi_select = false;
	int minimum_height = 120;
};

struct ToolViewTabDefinition {
	std::string id;
	std::string label;
	std::vector<ControlDefinition> controls;
	std::vector<TableDefinition> tables;
	std::vector<ActionDefinition> actions;
};

struct FormDefinition {
	std::string id;
	std::string title;
	std::vector<ControlDefinition> controls;
	std::vector<ActionDefinition> actions;
	int minimum_width = 0;
	int minimum_height = 0;
	bool resizable = true;
};

struct OpenFormRequest {
	FormDefinition definition;
	std::string owner_view_id;
};

struct ToolViewDefinition {
	std::string id;
	std::string title;
	std::vector<ControlDefinition> controls;
	std::vector<TableDefinition> tables;
	std::vector<ActionDefinition> actions;
	std::vector<ToolViewTabDefinition> tabs;
	std::string placement = "auto";
	int minimum_width = 360;
	int minimum_height = 240;
	bool resizable = true;
};

struct ControlPatch {
	std::string id;
	std::optional<std::string> text;
	std::optional<bool> checked;
	std::optional<double> number;
	std::optional<std::string> selected_choice_id;
	std::optional<bool> enabled;
	std::optional<bool> visible;
};

struct TablePatch {
	std::string id;
	std::vector<TableRowDefinition> rows;
	bool replace = true;
};

struct ToolViewPatch {
	std::string view_id;
	int64_t revision = 0;
	std::vector<ControlPatch> controls;
	std::vector<TablePatch> tables;
	std::optional<std::vector<std::string>> selected_row_ids;
	std::optional<std::string> selected_tab_id;
};

struct VideoPointSelectionRequest {
	std::string view_id;
	std::string session_id;
	int point_count = 2;
	std::string coordinate_space = "script";
	bool include_distance = true;
};

OpenFormRequest ParseOpenFormRequest(std::string const& json);
ToolViewDefinition ParseOpenToolViewRequest(std::string const& json);
ToolViewPatch ParseToolViewPatchRequest(std::string const& json);
std::string ParseCloseToolViewRequest(std::string const& json);
VideoPointSelectionRequest ParseVideoPointSelectionRequest(std::string const& json);

std::string BuildToolViewOperationResult(
	std::string const& view_id,
	bool succeeded,
	std::string const& status = "");
std::string BuildFormResult(
	std::string const& form_id,
	std::string const& action_id,
	std::vector<Value> const& values,
	bool cancelled);
std::string BuildToolViewEvent(
	std::string const& view_id,
	std::string const& event_id,
	std::string const& source_id,
	std::vector<Value> const& values,
	std::vector<std::string> const& selected_row_ids,
	int64_t revision,
	std::string const& active_tab_id);
std::string BuildVideoPointSelectionEvent(
	std::string const& view_id,
	std::string const& session_id,
	std::vector<std::pair<double, double>> const& points,
	int frame,
	bool cancelled,
	bool include_distance);

} // namespace agi::coreclr::ui
