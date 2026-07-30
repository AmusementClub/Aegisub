#include <main.h>

#include "../../src/audio_playback_section.h"

#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace playback_section = aegisub::audio_playback_section;

TEST(audio_playback_section, creates_independent_ranges) {
	auto const before = playback_section::Before(1000, 250);
	EXPECT_EQ(750, before.begin);
	EXPECT_EQ(1000, before.end);

	auto const after = playback_section::After(3000, 350);
	EXPECT_EQ(3000, after.begin);
	EXPECT_EQ(3350, after.end);

	auto const begin = playback_section::Begin(1000, 3000, 450);
	EXPECT_EQ(1000, begin.begin);
	EXPECT_EQ(1450, begin.end);

	auto const end = playback_section::End(1000, 3000, 550);
	EXPECT_EQ(2450, end.begin);
	EXPECT_EQ(3000, end.end);
}

TEST(audio_playback_section, clips_inside_ranges_to_selection_length) {
	auto const begin = playback_section::Begin(1000, 1200, 500);
	EXPECT_EQ(1000, begin.begin);
	EXPECT_EQ(1200, begin.end);

	auto const end = playback_section::End(1000, 1200, 500);
	EXPECT_EQ(1000, end.begin);
	EXPECT_EQ(1200, end.end);
}

TEST(audio_playback_section, normalizes_invalid_durations) {
	EXPECT_EQ(0, playback_section::NormalizeDuration(-1));
	EXPECT_EQ(playback_section::MaximumDurationMs, playback_section::NormalizeDuration(100000));
}

TEST(audio_playback_section, default_configs_define_each_duration) {
	for (auto const& relative_path : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		std::ifstream stream(std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / relative_path, std::ios::binary);
		ASSERT_TRUE(stream) << relative_path;
		std::string const defaults(std::istreambuf_iterator<char>(stream), {});
		agi::Options options("", {defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);

		for (auto const *option_name : {
			playback_section::BeforeOption,
			playback_section::AfterOption,
			playback_section::BeginOption,
			playback_section::EndOption,
		})
			EXPECT_EQ(playback_section::DefaultDurationMs, options.Get(option_name)->GetInt()) << relative_path << ": " << option_name;
	}
}
