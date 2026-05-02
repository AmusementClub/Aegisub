#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_style.h"

TEST(ass_file_extradata, cleaning_copy_does_not_mutate_original_file_state) {
	AssFile original;

	auto duplicate_a = original.AddExtradata("Key", "Value A");
	auto duplicate_b = original.AddExtradata("Key", "Value B");
	original.AddExtradata("Unused", "Orphaned");

	auto line = new AssDialogue;
	line->ExtradataIds = std::vector<uint32_t>{duplicate_a, duplicate_b};
	original.Events.push_back(*line);

	AssFile copy(original);
	copy.CleanExtradata();

	ASSERT_EQ(3u, original.Extradata.size());
	ASSERT_EQ(2u, original.Events.front().ExtradataIds.get().size());

	EXPECT_EQ(1u, copy.Extradata.size());
	EXPECT_EQ(1u, copy.Events.front().ExtradataIds.get().size());
}

TEST(ass_file_style_lookup, matches_renderer_compatible_default_and_starred_names) {
	AssFile file;
	file.Styles.push_back(*new AssStyle("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));
	file.Styles.push_back(*new AssStyle("Style: *Starred,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));

	EXPECT_EQ("Default", file.GetStyle("*default")->name);
	EXPECT_EQ("*Starred", file.GetStyle("Starred")->name);
	EXPECT_EQ("*Starred", file.GetStyle("**Starred")->name);
}

TEST(ass_file_style_lookup, keeps_non_default_style_names_case_sensitive) {
	AssFile file;
	file.Styles.push_back(*new AssStyle("Style: Foo,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));
	file.Styles.push_back(*new AssStyle("Style: foo,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1"));

	EXPECT_EQ("Foo", file.GetStyle("Foo")->name);
	EXPECT_EQ("foo", file.GetStyle("foo")->name);
	EXPECT_EQ(nullptr, file.GetStyle("FOO"));
}
