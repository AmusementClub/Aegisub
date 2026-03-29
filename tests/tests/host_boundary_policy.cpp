#include <main.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool StartsWith(std::string const& value, std::string_view prefix) {
	return value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string const& value, std::string_view suffix) {
	return value.size() >= suffix.size()
		&& value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::filesystem::path ProjectRoot() {
	return std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR).lexically_normal();
}

bool IsBoundaryCandidate(std::string const& relative_path) {
	auto filename = std::filesystem::path(relative_path).filename().string();
	if (filename == "app_runtime.cpp" || filename == "app_runtime.h")
		return true;
	if (StartsWith(filename, "headless_") && (EndsWith(filename, ".cpp") || EndsWith(filename, ".h")))
		return true;
	if (EndsWith(filename, "_service.cpp") || EndsWith(filename, "_service.h"))
		return true;
	if (EndsWith(filename, "_ops.cpp") || EndsWith(filename, "_ops.h"))
		return true;
	if (EndsWith(filename, "_host.cpp") || EndsWith(filename, "_host.h"))
		return true;
	return false;
}

std::vector<std::string> FindWxMarkers(std::filesystem::path const& path) {
	static const std::regex wx_token_pattern(R"(\bwx[A-Z][A-Za-z0-9_]*\b)");

	std::ifstream input(path);
	std::vector<std::string> hits;
	std::string line;
	int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		auto comment = line.find("//");
		auto code = line.substr(0, comment);
		if (code.find("#include <wx/") != std::string::npos
			|| code.find("#include \"wx/") != std::string::npos
			|| std::regex_search(code, wx_token_pattern)) {
			std::ostringstream hit;
			hit << path.generic_string() << ":" << line_number << ": " << line;
			hits.push_back(hit.str());
		}
	}
	return hits;
}

std::vector<std::string> FindLiteralHits(std::filesystem::path const& path, std::string_view needle) {
	std::ifstream input(path);
	std::vector<std::string> hits;
	std::string line;
	int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		if (line.find(needle) == std::string::npos)
			continue;

		std::ostringstream hit;
		hit << path.generic_string() << ":" << line_number << ": " << line;
		hits.push_back(hit.str());
	}
	return hits;
}

std::string JoinLines(std::vector<std::string> const& lines) {
	std::ostringstream out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i != 0)
			out << "\n";
		out << lines[i];
	}
	return out.str();
}

}

TEST(host_boundary_policy, service_like_sources_keep_wx_at_host_edges) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const allowed_wx_candidates = {
		"src/gui_wx_runtime_host.cpp",
		"src/headless_wx_runtime_host.cpp",
		"src/wx_audio_controller_power_host.cpp",
	};

	std::set<std::string> actual_wx_candidates;
	std::vector<std::string> unexpected_hits;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;

		auto relative = std::filesystem::relative(entry.path(), root).generic_string();
		if (!IsBoundaryCandidate(relative))
			continue;

		auto hits = FindWxMarkers(entry.path());
		if (hits.empty())
			continue;

		actual_wx_candidates.insert(relative);
		if (!allowed_wx_candidates.contains(relative))
			unexpected_hits.insert(unexpected_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(unexpected_hits.empty()) << JoinLines(unexpected_hits);
	EXPECT_EQ(allowed_wx_candidates, actual_wx_candidates);
}

TEST(host_boundary_policy, ui_service_contract_stays_split_from_wx_adapter) {
	auto const root = ProjectRoot();
	auto const ui_services_h = root / "src" / "ui_services.h";
	auto const ui_services_cpp = root / "src" / "ui_services.cpp";
	auto const wx_message_box_ui_services_h = root / "src" / "wx_message_box_ui_services.h";
	auto const wx_single_choice_dialog_h = root / "src" / "wx_single_choice_dialog.h";
	auto const wx_file_dialog_services_h = root / "src" / "wx_file_dialog_services.h";
	auto const legacy_wx_ui_services_h = root / "src" / "wx_ui_services.h";

	auto ui_header_hits = FindWxMarkers(ui_services_h);
	auto ui_cpp_hits = FindWxMarkers(ui_services_cpp);
	auto wx_message_box_hits = FindWxMarkers(wx_message_box_ui_services_h);
	auto wx_single_choice_hits = FindWxMarkers(wx_single_choice_dialog_h);
	auto wx_file_dialog_hits = FindWxMarkers(wx_file_dialog_services_h);

	EXPECT_TRUE(ui_header_hits.empty()) << JoinLines(ui_header_hits);
	EXPECT_TRUE(ui_cpp_hits.empty()) << JoinLines(ui_cpp_hits);
	EXPECT_FALSE(wx_message_box_hits.empty());
	EXPECT_FALSE(wx_single_choice_hits.empty());
	EXPECT_FALSE(wx_file_dialog_hits.empty());
	EXPECT_FALSE(std::filesystem::exists(legacy_wx_ui_services_h));
}

TEST(host_boundary_policy, context_backed_file_dialog_callers_prefer_context_service) {
	auto const root = ProjectRoot();

	struct FileDialogCallerExpectation {
		std::string path;
		std::vector<std::string> required_context_calls;
	};

	std::vector<FileDialogCallerExpectation> const expectations = {
		{
			"src/dialog_attachments.cpp",
			{"RequestOpenFiles(", "RequestSaveFile(", "RequestSelectDirectory("}
		},
		{
			"src/dialog_automation.cpp",
			{"RequestOpenFiles("}
		},
		{
			"src/dialog_fonts_collector.cpp",
			{"RequestSaveFile(", "RequestSelectDirectory("}
		},
	};

	for (auto const& expectation : expectations) {
		auto const path = root / expectation.path;
		ASSERT_TRUE(std::filesystem::exists(path)) << expectation.path;

		auto include_hits = FindLiteralHits(path, "wx_file_dialog_services.h");
		auto make_window_hits = FindLiteralHits(path, "MakeWindowFileDialogService(");
		EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
		EXPECT_TRUE(make_window_hits.empty()) << JoinLines(make_window_hits);

		for (auto const& call : expectation.required_context_calls) {
			auto call_hits = FindLiteralHits(path, call);
			EXPECT_FALSE(call_hits.empty()) << expectation.path << " missing " << call;
		}
	}
}

TEST(host_boundary_policy, shared_dispatch_timers_stay_wx_free_and_no_longer_use_host_suffix) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";

	std::set<std::string> const expected_shared_timer_sources = {
		"src/audio_controller_timer.cpp",
		"src/audio_controller_timer.h",
		"src/main_thread_timer.h",
		"src/playback_probe_timer.cpp",
		"src/playback_probe_timer.h",
		"src/playback_session_timer.cpp",
		"src/playback_session_timer.h",
		"src/video_controller_timer.cpp",
		"src/video_controller_timer.h",
	};

	std::vector<std::string> unexpected_legacy_host_files;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;
		auto const filename = entry.path().filename().string();
		if (filename.find("_timer_host.") == std::string::npos)
			continue;

		std::ostringstream hit;
		hit << std::filesystem::relative(entry.path(), root).generic_string();
		unexpected_legacy_host_files.push_back(hit.str());
	}

	std::vector<std::string> unexpected_wx_hits;
	for (auto const& relative : expected_shared_timer_sources) {
		auto const path = root / std::filesystem::path(relative);
		ASSERT_TRUE(std::filesystem::exists(path)) << relative;

		auto hits = FindWxMarkers(path);
		unexpected_wx_hits.insert(unexpected_wx_hits.end(), hits.begin(), hits.end());
	}

	EXPECT_TRUE(unexpected_legacy_host_files.empty()) << JoinLines(unexpected_legacy_host_files);
	EXPECT_TRUE(unexpected_wx_hits.empty()) << JoinLines(unexpected_wx_hits);
}

TEST(host_boundary_policy, runtime_wx_hooks_live_in_explicit_runtime_host_files) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const gui_runtime_host_cpp = root / "src" / "gui_wx_runtime_host.cpp";
	auto const headless_runtime_host_cpp = root / "src" / "headless_wx_runtime_host.cpp";

	auto main_log_hits = FindLiteralHits(main_cpp, "wxLog::GetActiveTarget");
	auto main_png_hits = FindLiteralHits(main_cpp, "wxPNGHandler");
	auto gui_log_hits = FindLiteralHits(gui_runtime_host_cpp, "wxLog::GetActiveTarget");
	auto gui_png_hits = FindLiteralHits(gui_runtime_host_cpp, "wxPNGHandler");
	auto headless_log_hits = FindLiteralHits(headless_runtime_host_cpp, "wxLog::GetActiveTarget");
	auto headless_png_hits = FindLiteralHits(headless_runtime_host_cpp, "wxPNGHandler");

	EXPECT_TRUE(main_log_hits.empty()) << JoinLines(main_log_hits);
	EXPECT_TRUE(main_png_hits.empty()) << JoinLines(main_png_hits);
	EXPECT_FALSE(gui_log_hits.empty());
	EXPECT_FALSE(gui_png_hits.empty());
	EXPECT_FALSE(headless_log_hits.empty());
	EXPECT_FALSE(headless_png_hits.empty());
}

TEST(host_boundary_policy, explicit_wx_surface_inventory_stays_current) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const expected_explicit_wx_surfaces = {
		"src/wx_file_dialog_services.h",
		"src/gui_wx_runtime_host.cpp",
		"src/gui_wx_runtime_host.h",
		"src/headless_wx_runtime_host.cpp",
		"src/headless_wx_runtime_host.h",
		"src/wx_message_box_ui_services.h",
		"src/wx_audio_controller_power_host.cpp",
		"src/wx_single_choice_dialog.h",
	};

	std::set<std::string> actual_explicit_wx_surfaces;
	for (auto const& entry : std::filesystem::directory_iterator(src_root)) {
		if (!entry.is_regular_file())
			continue;

		auto relative = std::filesystem::relative(entry.path(), root).generic_string();
		auto const filename = entry.path().filename().string();
		if (filename.find("wx") == std::string::npos)
			continue;
		actual_explicit_wx_surfaces.insert(relative);
	}

	EXPECT_EQ(expected_explicit_wx_surfaces, actual_explicit_wx_surfaces);
}
