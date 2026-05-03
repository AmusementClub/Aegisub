#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_style.h"
#include "../../src/ass_style_resolution.h"

#include <string>
#include <vector>

namespace style_resolution = aegisub::ass_style_resolution;

namespace {

std::string style_line(std::string const& name, std::string const& font) {
	return "Style: " + name + "," + font + ",48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1";
}

AssStyle *add_style(AssFile& file, std::string const& name, std::string const& font = "Arial") {
	auto *style = new AssStyle(style_line(name, font));
	file.Styles.push_back(*style);
	return style;
}

AssDialogue *add_line(AssFile& file, std::string const& style, std::string const& text = "text") {
	auto *line = new AssDialogue;
	line->Style = style;
	line->Text = text;
	file.Events.push_back(*line);
	return line;
}

std::vector<style_resolution::StyleUsage> reset_usages(style_resolution::StyleResolutionGraph const& graph) {
	std::vector<style_resolution::StyleUsage> usages;
	for (auto const& usage : graph.usages) {
		if (usage.kind == style_resolution::StyleUsageKind::ResetTag)
			usages.push_back(usage);
	}
	return usages;
}

}

TEST(ass_style_resolution, event_lookup_uses_last_renderer_match) {
	AssFile file;
	add_style(file, "Foo", "Arial");
	auto *winner = add_style(file, "*Foo", "Times New Roman");

	EXPECT_EQ(winner, style_resolution::ResolveEventStyle(file, "Foo"));
	EXPECT_EQ(winner, file.GetStyle("**Foo"));
}

TEST(ass_style_resolution, reset_lookup_is_strict_and_distinct_from_event_style_lookup) {
	AssFile file;
	add_style(file, "Default", "Arial");
	add_style(file, "default", "Times New Roman");
	add_line(file, "default", "{\\rdefault}x{\\rDefault}y{\\r*Default}z");

	auto graph = style_resolution::BuildStyleResolutionGraph(file);
	ASSERT_EQ(2u, graph.styles.size());
	ASSERT_GE(graph.usages.size(), 1u);

	EXPECT_EQ(style_resolution::StyleUsageKind::EventStyle, graph.usages[0].kind);
	EXPECT_TRUE(graph.usages[0].resolved);
	EXPECT_EQ(0, graph.usages[0].resolved_style);

	auto resets = reset_usages(graph);
	ASSERT_EQ(3u, resets.size());
	EXPECT_EQ("default", resets[0].raw_name);
	EXPECT_EQ(1, resets[0].resolved_style);
	EXPECT_EQ("Default", resets[1].raw_name);
	EXPECT_EQ(0, resets[1].resolved_style);
	EXPECT_EQ("*Default", resets[2].raw_name);
	EXPECT_FALSE(resets[2].resolved);
}

TEST(ass_style_resolution, clean_plan_renames_starred_winner_and_updates_events) {
	AssFile file;
	auto *style = add_style(file, "*Foo");
	auto *line = add_line(file, "*Foo");
	add_line(file, "*Foo");
	add_line(file, "Foo");
	add_line(file, "*Foo");
	add_line(file, "*Foo");

	auto graph = style_resolution::BuildStyleResolutionGraph(file);
	auto plan = style_resolution::BuildStyleCleanPlan(graph);
	ASSERT_EQ(1u, plan.renames.size());
	ASSERT_EQ(4u, plan.event_updates.size());
	EXPECT_EQ(style, plan.renames[0].style);
	EXPECT_EQ("Foo", plan.renames[0].new_name);
	EXPECT_EQ(line, plan.event_updates[0].line);
	EXPECT_EQ("*Foo", plan.event_updates[0].old_name);
	EXPECT_EQ("Foo", plan.event_updates[0].new_name);
	EXPECT_EQ("*Foo", plan.event_updates[0].resolved_style_name);
	EXPECT_EQ(1, plan.event_updates[0].line_number);

	auto preview = style_resolution::BuildStyleCleanPreview(plan);
	EXPECT_NE(std::string::npos, preview.find("lines 1-2, 4-5: *Foo -> Foo"));
	EXPECT_NE(std::string::npos, preview.find("Reset tags are not renamed automatically"));

	style_resolution::ApplyStyleCleanPlan(plan);
	EXPECT_EQ("Foo", style->name);
	EXPECT_EQ("Foo", line->Style.get());
}

TEST(ass_style_resolution, clean_plan_removes_shadowed_duplicate_and_keeps_winner_content) {
	AssFile file;
	add_style(file, "Foo", "Arial");
	add_style(file, "*Foo", "Times New Roman");
	add_line(file, "Foo");

	auto graph = style_resolution::BuildStyleResolutionGraph(file);
	auto plan = style_resolution::BuildStyleCleanPlan(graph);
	ASSERT_EQ(1u, plan.renames.size());
	ASSERT_EQ(1u, plan.deletions.size());

	style_resolution::ApplyStyleCleanPlan(plan);
	ASSERT_EQ(1u, file.Styles.size());
	EXPECT_EQ("Foo", file.Styles.front().name);
	EXPECT_EQ("Times New Roman", file.Styles.front().font);
	EXPECT_EQ("Foo", file.Events.front().Style.get());
}

TEST(ass_style_resolution, clean_plan_keeps_case_confusing_default_when_reset_tag_uses_it) {
	AssFile file;
	add_style(file, "Default", "Arial");
	add_style(file, "default", "Times New Roman");
	auto *line = add_line(file, "default", "{\\rdefault}x");

	auto graph = style_resolution::BuildStyleResolutionGraph(file);
	auto plan = style_resolution::BuildStyleCleanPlan(graph);
	ASSERT_EQ(1u, plan.event_updates.size());
	EXPECT_TRUE(plan.renames.empty());
	EXPECT_TRUE(plan.deletions.empty());

	style_resolution::ApplyStyleCleanPlan(plan);
	EXPECT_EQ("Default", line->Style.get());
	ASSERT_EQ(2u, file.Styles.size());
	auto *reset_style = style_resolution::ResolveResetStyle(file, "default");
	ASSERT_NE(nullptr, reset_style);
	EXPECT_EQ("Times New Roman", reset_style->font);
}
