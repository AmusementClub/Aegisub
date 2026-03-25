#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/subtitle_editor_ops.h"

#include <memory>
#include <vector>

namespace {

struct subtitle_editor_fixture {
	EntryList<AssDialogue> events;
	std::vector<std::unique_ptr<AssDialogue>> storage;

	AssDialogue *AddLine(int start, int end, std::string text = "", std::string style = "Default") {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Start = start;
		line->End = end;
		line->Text = std::move(text);
		line->Style = std::move(style);
		events.push_back(*line);
		return line;
	}
};

}

TEST(subtitle_editor_ops, create_line_at_video_time_copies_style_and_uses_default_duration) {
	AssDialogue active;
	active.Style = "Alt";

	auto created = aegisub::subtitle_editor_ops::CreateLineAtVideoTime(active, 2500, 700);

	ASSERT_TRUE(created);
	EXPECT_EQ(2500, static_cast<int>(created->Start));
	EXPECT_EQ(3200, static_cast<int>(created->End));
	EXPECT_EQ("Alt", created->Style.get());
}

TEST(subtitle_editor_ops, create_line_after_active_clamps_to_next_available_start) {
	subtitle_editor_fixture fixture;
	auto *previous = fixture.AddLine(0, 500);
	auto *active = fixture.AddLine(1000, 2000, "", "Main");
	auto *next = fixture.AddLine(2300, 2600);
	(void)previous;
	(void)next;

	auto created = aegisub::subtitle_editor_ops::CreateLineAfterActive(*active, fixture.events, 800);

	ASSERT_TRUE(created);
	EXPECT_EQ(2000, static_cast<int>(created->Start));
	EXPECT_EQ(2300, static_cast<int>(created->End));
	EXPECT_EQ("Main", created->Style.get());
}

TEST(subtitle_editor_ops, create_line_before_active_clamps_to_previous_available_end) {
	subtitle_editor_fixture fixture;
	auto *previous = fixture.AddLine(700, 1500);
	auto *active = fixture.AddLine(2000, 2600, "", "Alt");
	auto *later = fixture.AddLine(4000, 4500);
	(void)previous;
	(void)later;

	auto created = aegisub::subtitle_editor_ops::CreateLineBeforeActive(*active, fixture.events, 800);

	ASSERT_TRUE(created);
	EXPECT_EQ(1500, static_cast<int>(created->Start));
	EXPECT_EQ(2000, static_cast<int>(created->End));
	EXPECT_EQ("Alt", created->Style.get());
}

TEST(subtitle_editor_ops, select_matching_lines_preserves_event_order_and_first_match_as_active) {
	subtitle_editor_fixture fixture;
	auto *first = fixture.AddLine(0, 500, "hidden");
	auto *second = fixture.AddLine(500, 1000, "show one");
	auto *third = fixture.AddLine(1000, 1500, "show two");

	auto result = aegisub::subtitle_editor_ops::SelectMatchingLines(fixture.events, [](AssDialogue const& line) {
		return line.Text.get().find("show") != std::string::npos;
	});

	ASSERT_EQ(2u, result.ordered_lines.size());
	EXPECT_EQ(second, result.active_line);
	EXPECT_EQ(second, result.ordered_lines[0]);
	EXPECT_EQ(third, result.ordered_lines[1]);
	EXPECT_NE(first, result.active_line);
}
