#include <main.h>

#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST(perspective_visual_tool_options, default_configs_define_persistent_solve_options) {
	for (auto const& relative_path : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		std::ifstream stream(
			std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / relative_path,
			std::ios::binary);
		ASSERT_TRUE(stream) << relative_path;
		std::string const defaults(std::istreambuf_iterator<char>(stream), {});
		agi::Options options(
			"", {defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);

		EXPECT_TRUE(options.Get("Tool/Visual/Perspective/Fit Text")->GetBool())
			<< relative_path;
		EXPECT_FALSE(options.Get("Tool/Visual/Perspective/Fax Frz Only")->GetBool())
			<< relative_path;
		// 4 by default: two digits with a 0.1 px rounding budget refuses drags
		// that only need one more decimal, and CompactField already shortens
		// every field to the fewest digits that fit, so a higher cap costs
		// nothing on lines that never needed it.
		EXPECT_EQ(4, options.Get("Tool/Visual/Perspective/Decimal Places")->GetInt())
			<< relative_path;
	}
}

TEST(perspective_visual_tool_options, default_nudge_steps_are_doubles) {
	for (auto const& relative_path : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		std::ifstream stream(
			std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / relative_path,
			std::ios::binary);
		ASSERT_TRUE(stream) << relative_path;
		std::string const defaults(std::istreambuf_iterator<char>(stream), {});
		agi::Options options(
			"", {defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);

		EXPECT_DOUBLE_EQ(1.0, options.Get("Tool/Visual/Nudge/Rotate Step")->GetDouble())
			<< relative_path;
		EXPECT_DOUBLE_EQ(15.0, options.Get("Tool/Visual/Nudge/Rotate Step Large")->GetDouble())
			<< relative_path;
		EXPECT_DOUBLE_EQ(1.0, options.Get("Tool/Visual/Nudge/Scale Step")->GetDouble())
			<< relative_path;
		EXPECT_DOUBLE_EQ(10.0, options.Get("Tool/Visual/Nudge/Scale Step Large")->GetDouble())
			<< relative_path;
		EXPECT_DOUBLE_EQ(1.0, options.Get("Tool/Visual/Nudge/Origin Step")->GetDouble())
			<< relative_path;
		EXPECT_DOUBLE_EQ(10.0, options.Get("Tool/Visual/Nudge/Origin Step Large")->GetDouble())
			<< relative_path;
	}
}

TEST(perspective_visual_tool_options, preferences_expose_all_persistent_perspective_options) {
	std::ifstream stream(
		std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / "src/preferences.cpp",
		std::ios::binary);
	ASSERT_TRUE(stream);
	std::string const text(std::istreambuf_iterator<char>(stream), {});
	EXPECT_NE(std::string::npos, text.find("page_visual_tools"));
	EXPECT_NE(std::string::npos, text.find("Tool/Visual/Perspective/Fit Text"));
	EXPECT_NE(std::string::npos, text.find("Tool/Visual/Perspective/Fax Frz Only"));
	EXPECT_NE(std::string::npos, text.find("Tool/Visual/Perspective/Decimal Places"));
}
