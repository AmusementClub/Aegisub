#include <main.h>

#include <libaegisub/option.h>
#include <libaegisub/option_value.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST(subtitle_edit_box_options, default_configs_define_margin_spin_step) {
	for (auto const& relative_path : {
		std::filesystem::path("src/libresrc/default_config.json"),
		std::filesystem::path("src/libresrc/osx/default_config.json"),
	}) {
		std::ifstream stream(std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / relative_path, std::ios::binary);
		ASSERT_TRUE(stream) << relative_path;
		std::string const defaults(std::istreambuf_iterator<char>(stream), {});
		agi::Options options("", {defaults.data(), defaults.size()}, agi::Options::FLUSH_SKIP);

		EXPECT_EQ(1, options.Get("Subtitle/Edit Box/Margin Spin Step")->GetInt()) << relative_path;
	}
}
