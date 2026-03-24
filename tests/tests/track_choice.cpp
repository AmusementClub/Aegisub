#include <main.h>

#include "../../src/track_choice.h"

TEST(track_choice, build_request_preserves_subtitle_choices) {
	std::vector<std::string> choices = {"Track 00: ASS", "Track 01: UTF8"};

	auto request = aegisub::track_choice::BuildRequest(aegisub::track_choice::DialogKind::Subtitle, choices);

	EXPECT_EQ(choices, request.choices);
	EXPECT_FALSE(request.title.empty());
	EXPECT_FALSE(request.message.empty());
}

TEST(track_choice, build_request_uses_distinct_titles_for_audio_and_video) {
	auto audio_request = aegisub::track_choice::BuildRequest(aegisub::track_choice::DialogKind::Audio, {"Track 01"});
	auto video_request = aegisub::track_choice::BuildRequest(aegisub::track_choice::DialogKind::Video, {"Track 00"});

	EXPECT_NE(audio_request.title, video_request.title);
	EXPECT_NE(audio_request.message, video_request.message);
}

TEST(track_choice, resolve_selection_accepts_in_range_index) {
	auto choice = aegisub::track_choice::ResolveSelection(3, 2);

	ASSERT_TRUE(choice.has_value());
	EXPECT_EQ(2, *choice);
}

TEST(track_choice, resolve_selection_rejects_cancel_and_out_of_range_indexes) {
	EXPECT_EQ(std::nullopt, aegisub::track_choice::ResolveSelection(2, std::nullopt));
	EXPECT_EQ(std::nullopt, aegisub::track_choice::ResolveSelection(2, -1));
	EXPECT_EQ(std::nullopt, aegisub::track_choice::ResolveSelection(2, 2));
}
