#include <main.h>

#include "../../src/coreclr/declarative_ui_model.h"

#include <cmath>
#include <stdexcept>

using namespace agi::coreclr::ui;

TEST(csharp_declarative_ui, parses_form_and_tool_view_definitions) {
	auto form_request = ParseOpenFormRequest(R"json({
		"ownerViewId": "package-manager",
		"definition": {
			"id": "settings", "title": "Settings", "schemaVersion": 1,
			"controls": [
				{"id":"name","kind":"text","label":"Name","text":"Aegisub","row":0,"column":0},
				{"id":"mode","kind":"select","choices":[{"id":"fast","label":"Fast"}],"selectedChoiceId":"fast","row":1,"column":0}
			],
			"actions": [{"id":"ok","label":"OK","isDefault":true}],
			"minimumWidth": 420
		}
	})json");
	auto const& form = form_request.definition;
	ASSERT_EQ(form.controls.size(), 2U);
	EXPECT_EQ(form.id, "settings");
	EXPECT_EQ(form.controls[1].selected_choice_id, "fast");
	EXPECT_EQ(form.minimum_width, 420);
	EXPECT_EQ(form_request.owner_view_id, "package-manager");

	auto view = ParseOpenToolViewRequest(R"json({
		"definition": {
			"id":"measure","title":"Measure","schemaVersion":1,
			"controls":[],
			"tables":[{"id":"points","columns":[{"id":"x","label":"X","width":80}],"rows":[],"multiSelect":true}],
			"actions":[{"id":"pick","label":"Pick two points"}],
			"tabs":[{
				"id":"settings","label":"Settings",
				"controls":[{"id":"units","kind":"select","choices":[{"id":"px","label":"Pixels"}]}],
				"tables":[],"actions":[]
			}],
			"placement":"docked"
		}
	})json");
	ASSERT_EQ(view.tables.size(), 1U);
	ASSERT_EQ(view.tabs.size(), 1U);
	EXPECT_EQ(view.tabs[0].id, "settings");
	EXPECT_EQ(view.tabs[0].controls[0].id, "units");
	EXPECT_EQ(view.placement, "docked");
	EXPECT_TRUE(view.tables[0].multi_select);
}

TEST(csharp_declarative_ui, rejects_duplicate_ids_and_invalid_grid_or_kind) {
	EXPECT_THROW(ParseOpenFormRequest(R"json({"definition":{"id":"x","title":"X","controls":[
		{"id":"same","kind":"label"},{"id":"same","kind":"text"}],"actions":[]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenFormRequest(R"json({"definition":{"id":"x","title":"X","controls":[
		{"id":"bad","kind":"text","row":-1}],"actions":[]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenFormRequest(R"json({"definition":{"id":"x","title":"X","controls":[
		{"id":"bad","kind":"video-display"}],"actions":[]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenFormRequest(R"json({"definition":{"id":"x","title":"X","controls":[
		{"id":"left","kind":"text","row":0,"column":0,"columnSpan":2},
		{"id":"right","kind":"text","row":0,"column":1}],"actions":[]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenToolViewRequest(R"json({"definition":{"id":"x","title":"X","controls":[],"actions":[],"tables":[
		{"id":"a","columns":[{"id":"value","label":"Value"}],"rows":[{"id":"same","cells":[]}]},
		{"id":"b","columns":[{"id":"value","label":"Value"}],"rows":[{"id":"same","cells":[]}]}
	]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenToolViewRequest(R"json({"definition":{"id":"x","title":"X","controls":[],"actions":[],"tables":[
		{"id":"a","columns":[{"id":"value","label":"Value"}],"rows":[
			{"id":"row","cells":[{"id":"value","jsonValue":"not-json"}]}]}
	]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenToolViewRequest(R"json({"definition":{"id":"x","title":"X","controls":[
		{"id":"same","kind":"label"}],"actions":[],"tables":[],"tabs":[
		{"id":"network","label":"Network","controls":[{"id":"same","kind":"text"}],"tables":[],"actions":[]}
	]}})json"), std::runtime_error);
	EXPECT_THROW(ParseOpenToolViewRequest(R"json({"definition":{"id":"x","title":"X","controls":[],"actions":[],"tables":[],"tabs":[
		{"id":"first","label":"First","controls":[],"tables":[{"id":"one","columns":[{"id":"value","label":"Value"}],"rows":[{"id":"same-row","cells":[]}]}],"actions":[]},
		{"id":"second","label":"Second","controls":[],"tables":[{"id":"two","columns":[{"id":"value","label":"Value"}],"rows":[{"id":"same-row","cells":[]}]}],"actions":[]}
	]}})json"), std::runtime_error);
}

TEST(csharp_declarative_ui, patch_requires_a_positive_revision) {
	auto patch = ParseToolViewPatchRequest(R"json({"patch":{"viewId":"measure","revision":2,"controls":[],"tables":[],"selectedRowIds":["p1"],"selectedTabId":"results"}})json");
	EXPECT_EQ(patch.view_id, "measure");
	EXPECT_EQ(patch.revision, 2);
	ASSERT_TRUE(patch.selected_row_ids.has_value());
	ASSERT_EQ(patch.selected_row_ids->size(), 1U);
	EXPECT_EQ((*patch.selected_row_ids)[0], "p1");
	ASSERT_TRUE(patch.selected_tab_id.has_value());
	EXPECT_EQ(*patch.selected_tab_id, "results");

	EXPECT_THROW(ParseToolViewPatchRequest(R"json({"patch":{"viewId":"measure","revision":0,"controls":[],"tables":[]}})json"), std::runtime_error);

	auto large = ParseToolViewPatchRequest(R"json({"patch":{"viewId":"measure","revision":5000000000,"controls":[],"tables":[],"selectedRowIds":null}})json");
	EXPECT_EQ(large.revision, 5000000000LL);
	EXPECT_FALSE(large.selected_row_ids.has_value());
	EXPECT_FALSE(large.selected_tab_id.has_value());
	EXPECT_THROW(ParseToolViewPatchRequest(R"json({"patch":{"viewId":"measure","revision":3,"controls":[],"tables":[],"selectedTabId":""}})json"), std::runtime_error);
}

TEST(csharp_declarative_ui, tool_view_event_reports_active_tab) {
	auto result = BuildToolViewEvent(
		"packages", "tabChanged", "network", {}, {}, 4, "network");
	EXPECT_NE(result.find("\"eventId\" : \"tabChanged\""), std::string::npos);
	EXPECT_NE(result.find("\"sourceId\" : \"network\""), std::string::npos);
	EXPECT_NE(result.find("\"activeTabId\" : \"network\""), std::string::npos);
}

TEST(csharp_declarative_ui, tool_view_operation_result_reports_status) {
	auto result = BuildToolViewOperationResult("packages", false, "unavailable");
	EXPECT_NE(result.find("\"viewId\" : \"packages\""), std::string::npos);
	EXPECT_NE(result.find("\"succeeded\" : false"), std::string::npos);
	EXPECT_NE(result.find("\"status\" : \"unavailable\""), std::string::npos);
}

TEST(csharp_declarative_ui, builds_video_two_point_distance_in_requested_coordinate_space) {
	auto request = ParseVideoPointSelectionRequest(
		R"json({"viewId":"measure","sessionId":"distance-1","pointCount":2,"coordinateSpace":"script","includeDistance":true})json");
	EXPECT_EQ(request.coordinate_space, "script");
	EXPECT_EQ(request.point_count, 2);

	auto result = BuildVideoPointSelectionEvent(
		request.view_id, request.session_id, {{0.0, 0.0}, {3.0, 4.0}}, 42, false, request.include_distance);
	EXPECT_NE(result.find("\"deltaX\" : 3.0"), std::string::npos);
	EXPECT_NE(result.find("\"deltaY\" : 4.0"), std::string::npos);
	EXPECT_NE(result.find("\"distance\" : 5.0"), std::string::npos);
	EXPECT_NE(result.find("\"frame\" : 42"), std::string::npos);
}
