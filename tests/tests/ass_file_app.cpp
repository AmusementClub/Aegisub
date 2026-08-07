#include <main.h>

#include "../../src/ass_file.h"
#include "../../src/ass_file_app.h"
#include "../../src/ass_style.h"
#include "../../src/ass_style_storage.h"
#include "../../src/options.h"
#include "../../src/secondary_subtitle_reload_policy.h"

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

#include <filesystem>

namespace {
class ScopedCatalogRoot {
	agi::Path path_tokens;
	agi::Path *previous_path = nullptr;

public:
	agi::fs::path root;

	ScopedCatalogRoot()
	: previous_path(config::path) {
		root = agi::fs::UniquePath(
			std::filesystem::temp_directory_path() / "aegisub-style-catalog-%%%%%%%%");
		agi::fs::CreateDirectory(root);
		path_tokens.SetToken("?user", root);
		config::path = &path_tokens;
	}

	~ScopedCatalogRoot() {
		config::path = previous_path;
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

void SaveCatalogStyle(
	std::string const& catalog_name,
	std::string const& style_name,
	std::string const& font_name) {
	AssStyleStorage storage;
	storage.LoadCatalog(catalog_name);
	storage.clear();
	auto style = agi::make_unique<AssStyle>();
	style->name = style_name;
	style->font = font_name;
	style->UpdateData();
	storage.push_back(std::move(style));
	storage.Save();
}
}

TEST(ass_file_app, default_style_catalog_is_reloaded_from_disk) {
	ScopedCatalogRoot catalog_root;
	SaveCatalogStyle("HotReload", "Default", "Before Reload");

	AssFile before;
	LoadDefaultAssFileWithAppOptions(before, false, "HotReload");
	ASSERT_NE(nullptr, before.GetStyle("Default"));
	EXPECT_EQ("Before Reload", before.GetStyle("Default")->font);

	SaveCatalogStyle("HotReload", "Default", "After Reload");
	AssFile after;
	LoadDefaultAssFileWithAppOptions(after, false, "HotReload");
	ASSERT_NE(nullptr, after.GetStyle("Default"));
	EXPECT_EQ("After Reload", after.GetStyle("Default")->font);
	EXPECT_EQ("Before Reload", before.GetStyle("Default")->font);
}

TEST(ass_file_app, non_default_catalog_styles_are_imported_without_replacing_builtin_default) {
	ScopedCatalogRoot catalog_root;
	SaveCatalogStyle("AlternateOnly", "Alternate", "Alternate Font");

	AssFile file;
	LoadDefaultAssFileWithAppOptions(file, false, "AlternateOnly");
	ASSERT_NE(nullptr, file.GetStyle("Default"));
	ASSERT_NE(nullptr, file.GetStyle("Alternate"));
	EXPECT_EQ("Arial", file.GetStyle("Default")->font);
	EXPECT_EQ("Alternate Font", file.GetStyle("Alternate")->font);
}

TEST(ass_file_app, secondary_srt_style_watch_uses_the_catalog_loader_path) {
	ScopedCatalogRoot catalog_root;
	auto const catalog_path = AssStyleStorage::GetCatalogPath("Watched");

	EXPECT_TRUE(ResolveSecondarySubtitleStyleCatalogWatchPath(false, true, "Watched").empty());
	EXPECT_TRUE(ResolveSecondarySubtitleStyleCatalogWatchPath(true, false, "Watched").empty());
	EXPECT_EQ(
		catalog_path,
		ResolveSecondarySubtitleStyleCatalogWatchPath(true, true, "Watched"));
}
