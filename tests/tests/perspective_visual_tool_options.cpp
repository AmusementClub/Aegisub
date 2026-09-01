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
		EXPECT_EQ(2, options.Get("Tool/Visual/Perspective/Decimal Places")->GetInt())
			<< relative_path;
	}
}
