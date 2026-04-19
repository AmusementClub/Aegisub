#include "../../src/ass_file.h"
#include "../../src/ass_parser.h"

#include <gtest/gtest.h>

TEST(ass_project_garbage, parses_secondary_subtitles_file) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[Aegisub Project Garbage]");
	parser.AddLine("Secondary Subtitles File: ../secondary.ass");

	EXPECT_EQ("../secondary.ass", file.Properties.secondary_subtitles_file);
}
