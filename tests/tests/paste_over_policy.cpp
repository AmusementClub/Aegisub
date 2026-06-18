#include <main.h>

#include "../../src/paste_over_policy.h"

#include <algorithm>

TEST(paste_over_policy, normalize_inserts_missing_legacy_comment_field) {
	std::vector<bool> legacy = {true, false, true, false, true, false, true, false, true, false};

	auto fields = aegisub::paste_over_policy::NormalizeFields(legacy);

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, fields.size());
	EXPECT_FALSE(fields[aegisub::paste_over_policy::Comment]);
	EXPECT_TRUE(fields[aegisub::paste_over_policy::Layer]);
	EXPECT_FALSE(fields[aegisub::paste_over_policy::StartTime]);
	EXPECT_FALSE(fields[aegisub::paste_over_policy::Text]);
}

TEST(paste_over_policy, normalize_pads_short_vectors_and_trims_long_vectors) {
	auto short_fields = aegisub::paste_over_policy::NormalizeFields({true, true});
	auto long_fields = aegisub::paste_over_policy::NormalizeFields(std::vector<bool>(20, true));

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, short_fields.size());
	EXPECT_TRUE(short_fields[0]);
	EXPECT_TRUE(short_fields[1]);
	EXPECT_FALSE(short_fields.back());

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, long_fields.size());
	EXPECT_TRUE(long_fields.back());
}

TEST(paste_over_policy, all_preset_sets_every_field) {
	auto all = aegisub::paste_over_policy::BuildAllFields(true);
	auto none = aegisub::paste_over_policy::BuildAllFields(false);

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, all.size());
	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, none.size());
	EXPECT_TRUE(std::all_of(all.begin(), all.end(), [](bool value) { return value; }));
	EXPECT_TRUE(std::none_of(none.begin(), none.end(), [](bool value) { return value; }));
}

TEST(paste_over_policy, times_preset_selects_start_and_end_only) {
	auto fields = aegisub::paste_over_policy::BuildTimesFields();

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, fields.size());
	for (std::size_t i = 0; i < fields.size(); ++i) {
		bool const expected = i == aegisub::paste_over_policy::StartTime
			|| i == aegisub::paste_over_policy::EndTime;
		EXPECT_EQ(expected, fields[i]) << i;
	}
}

TEST(paste_over_policy, text_preset_selects_text_only) {
	auto fields = aegisub::paste_over_policy::BuildTextFields();

	ASSERT_EQ(aegisub::paste_over_policy::FieldCount, fields.size());
	for (std::size_t i = 0; i < fields.size(); ++i)
		EXPECT_EQ(i == aegisub::paste_over_policy::Text, fields[i]) << i;
}
