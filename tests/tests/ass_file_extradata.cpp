#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"

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
