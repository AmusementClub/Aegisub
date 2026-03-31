#include <main.h>

#include <cctype>
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

std::set<std::string> FindFilesContainingLiteralInTree(
	std::filesystem::path const& project_root,
	std::filesystem::path const& search_root,
	std::string_view needle) {
	std::set<std::string> files;
	for (auto const& entry : std::filesystem::directory_iterator(search_root)) {
		if (!entry.is_regular_file())
			continue;

		auto const extension = entry.path().extension().string();
		if (extension != ".cpp" && extension != ".h")
			continue;

		if (!FindLiteralHits(entry.path(), needle).empty())
			files.insert(std::filesystem::relative(entry.path(), project_root).generic_string());
	}
	return files;
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

std::string TrimCopy(std::string value) {
	auto const not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::set<std::string> ReadNamedCMakeSetEntries(std::filesystem::path const& path, std::string const& variable_name) {
	std::ifstream input(path);
	std::set<std::string> entries;
	std::string line;
	bool in_block = false;
	auto const begin_marker = "set(" + variable_name;

	while (std::getline(input, line)) {
		auto trimmed = TrimCopy(line);
		if (!in_block) {
			if (trimmed == begin_marker)
				in_block = true;
			continue;
		}

		if (trimmed == ")")
			break;
		if (trimmed.empty() || StartsWith(trimmed, "#"))
			continue;
		entries.insert(trimmed);
	}

	return entries;
}

}

TEST(host_boundary_policy, service_like_sources_keep_wx_at_host_edges) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const allowed_wx_candidates = {
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_runtime_host.cpp",
		"src/wx_automation_file_dialog_service.h",
		"src/wx_audio_controller_power_host.cpp",
		"src/wx_frame_main_dialog_ui_host.h",
		"src/wx_frame_main_request_host.h",
		"src/wx_frame_main_runtime_host.h",
		"src/wx_preferences_ui_host.h",
		"src/wx_style_editor_ui_host.h",
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

TEST(host_boundary_policy, preferences_file_dialog_usage_stays_centralized) {
	auto const root = ProjectRoot();
	auto const preferences_cpp = root / "src" / "preferences.cpp";
	auto const preferences_base_cpp = root / "src" / "preferences_base.cpp";
	auto const wx_preferences_ui_host_h = root / "src" / "wx_preferences_ui_host.h";

	auto preferences_include_hits = FindLiteralHits(preferences_cpp, "wx_file_dialog_services.h");
	auto preferences_make_hits = FindLiteralHits(preferences_cpp, "MakeWindowFileDialogService(");
	auto preferences_seam_hits = FindLiteralHits(preferences_cpp, "MakePreferencesFileDialogService(");
	auto preferences_request_hits = FindLiteralHits(preferences_cpp, "RequestSelectDirectory(");
	auto preferences_open_hits = FindLiteralHits(preferences_cpp, "RequestOpenFile(");

	auto preferences_base_include_hits = FindLiteralHits(preferences_base_cpp, "wx_file_dialog_services.h");
	auto preferences_base_make_hits = FindLiteralHits(preferences_base_cpp, "MakeWindowFileDialogService(");
	auto preferences_base_request_hits = FindLiteralHits(preferences_base_cpp, "RequestSelectDirectory(");
	auto preferences_base_open_hits = FindLiteralHits(preferences_base_cpp, "RequestOpenFile(");

	auto seam_include_hits = FindLiteralHits(wx_preferences_ui_host_h, "wx_file_dialog_services.h");
	auto seam_make_hits = FindLiteralHits(wx_preferences_ui_host_h, "MakeWindowFileDialogService(");

	EXPECT_TRUE(preferences_include_hits.empty()) << JoinLines(preferences_include_hits);
	EXPECT_TRUE(preferences_make_hits.empty()) << JoinLines(preferences_make_hits);
	EXPECT_FALSE(preferences_seam_hits.empty());
	EXPECT_FALSE(preferences_request_hits.empty());
	EXPECT_FALSE(preferences_open_hits.empty());

	EXPECT_TRUE(preferences_base_include_hits.empty()) << JoinLines(preferences_base_include_hits);
	EXPECT_TRUE(preferences_base_make_hits.empty()) << JoinLines(preferences_base_make_hits);
	EXPECT_FALSE(preferences_base_request_hits.empty());
	EXPECT_FALSE(preferences_base_open_hits.empty());

	EXPECT_FALSE(seam_include_hits.empty());
	EXPECT_FALSE(seam_make_hits.empty());
}

TEST(host_boundary_policy, automation_file_dialog_fallback_lives_in_explicit_wx_service_seam) {
	auto const root = ProjectRoot();
	auto const auto4_lua_cpp = root / "src" / "auto4_lua.cpp";
	auto const wx_automation_service_h = root / "src" / "wx_automation_file_dialog_service.h";

	auto auto4_include_hits = FindLiteralHits(auto4_lua_cpp, "wx_file_dialog_services.h");
	auto auto4_make_hits = FindLiteralHits(auto4_lua_cpp, "MakeWindowFileDialogService(");
	auto auto4_seam_hits = FindLiteralHits(auto4_lua_cpp, "ResolveAutomationFileDialogService(");

	auto seam_make_hits = FindLiteralHits(wx_automation_service_h, "MakeWindowFileDialogService(");
	auto seam_include_hits = FindLiteralHits(wx_automation_service_h, "wx_file_dialog_services.h");

	EXPECT_TRUE(auto4_include_hits.empty()) << JoinLines(auto4_include_hits);
	EXPECT_TRUE(auto4_make_hits.empty()) << JoinLines(auto4_make_hits);
	EXPECT_FALSE(auto4_seam_hits.empty());

	EXPECT_FALSE(seam_make_hits.empty());
	EXPECT_FALSE(seam_include_hits.empty());
}

TEST(host_boundary_policy, app_bootstrap_ui_fallback_lives_in_explicit_wx_service_seam) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const gui_wx_bootstrap_ui_host_h = root / "src" / "gui_wx_bootstrap_ui_host.h";
	auto const gui_wx_bootstrap_ui_host_cpp = root / "src" / "gui_wx_bootstrap_ui_host.cpp";
	auto const wx_app_bootstrap_ui_services_h = root / "src" / "wx_app_bootstrap_ui_services.h";

	auto main_message_box_include_hits = FindLiteralHits(main_cpp, "wx_message_box_ui_services.h");
	auto main_single_choice_include_hits = FindLiteralHits(main_cpp, "wx_single_choice_dialog.h");
	auto main_notification_hits = FindLiteralHits(main_cpp, "WxMessageBoxNotificationSink");
	auto main_interaction_hits = FindLiteralHits(main_cpp, "WxMessageBoxInteractionSink");
	auto main_make_single_choice_hits = FindLiteralHits(main_cpp, "MakeWindowSingleChoiceInteractionSink(");
	auto main_bootstrap_ui_include_hits = FindLiteralHits(main_cpp, "wx_app_bootstrap_ui_services.h");
	auto main_gui_bootstrap_ui_host_include_hits = FindLiteralHits(main_cpp, "gui_wx_bootstrap_ui_host.h");
	auto main_build_bootstrap_ui_host_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeBootstrapUiHost()");
	auto host_runtime_bootstrap_host_include_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_h, "runtime_bootstrap_ui_host.h");
	auto host_bootstrap_ui_include_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "wx_app_bootstrap_ui_services.h");
	auto host_bootstrap_notification_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "AppBootstrapNotificationSink()");
	auto host_bootstrap_interaction_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "AppBootstrapInteractionSink()");
	auto host_bootstrap_single_choice_hits = FindLiteralHits(gui_wx_bootstrap_ui_host_cpp, "MakeAppBootstrapSingleChoiceInteractionSink()");

	auto seam_notification_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "WxMessageBoxNotificationSink");
	auto seam_interaction_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "WxMessageBoxInteractionSink");
	auto seam_single_choice_hits = FindLiteralHits(wx_app_bootstrap_ui_services_h, "MakeWindowSingleChoiceInteractionSink(");

	EXPECT_TRUE(main_message_box_include_hits.empty()) << JoinLines(main_message_box_include_hits);
	EXPECT_TRUE(main_single_choice_include_hits.empty()) << JoinLines(main_single_choice_include_hits);
	EXPECT_TRUE(main_notification_hits.empty()) << JoinLines(main_notification_hits);
	EXPECT_TRUE(main_interaction_hits.empty()) << JoinLines(main_interaction_hits);
	EXPECT_TRUE(main_make_single_choice_hits.empty()) << JoinLines(main_make_single_choice_hits);
	EXPECT_TRUE(main_bootstrap_ui_include_hits.empty()) << JoinLines(main_bootstrap_ui_include_hits);
	EXPECT_FALSE(main_gui_bootstrap_ui_host_include_hits.empty());
	EXPECT_FALSE(main_build_bootstrap_ui_host_hits.empty());

	EXPECT_FALSE(host_runtime_bootstrap_host_include_hits.empty());
	EXPECT_FALSE(host_bootstrap_ui_include_hits.empty());
	EXPECT_FALSE(host_bootstrap_notification_hits.empty());
	EXPECT_FALSE(host_bootstrap_interaction_hits.empty());
	EXPECT_FALSE(host_bootstrap_single_choice_hits.empty());

	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());
	EXPECT_FALSE(seam_single_choice_hits.empty());
}

TEST(host_boundary_policy, preferences_interaction_usage_stays_centralized) {
	auto const root = ProjectRoot();
	auto const preferences_cpp = root / "src" / "preferences.cpp";
	auto const wx_preferences_ui_host_h = root / "src" / "wx_preferences_ui_host.h";

	auto include_hits = FindLiteralHits(preferences_cpp, "wx_message_box_ui_services.h");
	auto make_hits = FindLiteralHits(preferences_cpp, "MakeWindowInteractionSink(");
	auto seam_hits = FindLiteralHits(preferences_cpp, "MakePreferencesInteractionSink(");
	auto request_hits = FindLiteralHits(preferences_cpp, "RequestInteraction(");
	auto seam_include_hits = FindLiteralHits(wx_preferences_ui_host_h, "wx_message_box_ui_services.h");
	auto seam_make_hits = FindLiteralHits(wx_preferences_ui_host_h, "MakeWindowInteractionSink(");

	EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
	EXPECT_TRUE(make_hits.empty()) << JoinLines(make_hits);
	EXPECT_FALSE(seam_hits.empty());
	EXPECT_FALSE(request_hits.empty());
	EXPECT_FALSE(seam_include_hits.empty());
	EXPECT_FALSE(seam_make_hits.empty());
}

TEST(host_boundary_policy, style_editor_ui_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const dialog_style_editor_cpp = root / "src" / "dialog_style_editor.cpp";
	auto const wx_style_editor_ui_host_h = root / "src" / "wx_style_editor_ui_host.h";

	auto include_hits = FindLiteralHits(dialog_style_editor_cpp, "wx_message_box_ui_services.h");
	auto make_notification_hits = FindLiteralHits(dialog_style_editor_cpp, "MakeWindowNotificationSink(");
	auto make_interaction_hits = FindLiteralHits(dialog_style_editor_cpp, "MakeWindowInteractionSink(");
	auto seam_notification_hits = FindLiteralHits(dialog_style_editor_cpp, "ResolveStyleEditorNotificationSink(");
	auto seam_interaction_hits = FindLiteralHits(dialog_style_editor_cpp, "ResolveStyleEditorInteractionSink(");

	auto seam_make_notification_hits = FindLiteralHits(wx_style_editor_ui_host_h, "MakeWindowNotificationSink(");
	auto seam_make_interaction_hits = FindLiteralHits(wx_style_editor_ui_host_h, "MakeWindowInteractionSink(");
	auto seam_include_hits = FindLiteralHits(wx_style_editor_ui_host_h, "wx_message_box_ui_services.h");

	EXPECT_TRUE(include_hits.empty()) << JoinLines(include_hits);
	EXPECT_TRUE(make_notification_hits.empty()) << JoinLines(make_notification_hits);
	EXPECT_TRUE(make_interaction_hits.empty()) << JoinLines(make_interaction_hits);
	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());

	EXPECT_FALSE(seam_make_notification_hits.empty());
	EXPECT_FALSE(seam_make_interaction_hits.empty());
	EXPECT_FALSE(seam_include_hits.empty());
}

TEST(host_boundary_policy, frame_main_dialog_ui_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_dialog_ui_host_h = root / "src" / "wx_frame_main_dialog_ui_host.h";

	auto message_box_include_hits = FindLiteralHits(frame_main_cpp, "wx_message_box_ui_services.h");
	auto single_choice_include_hits = FindLiteralHits(frame_main_cpp, "wx_single_choice_dialog.h");
	auto notification_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainNotificationSink");
	auto interaction_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainInteractionSink");
	auto single_choice_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainSingleChoiceInteractionSink");
	auto startup_log_hits = FindLiteralHits(frame_main_cpp, "ShowFrameMainStartupLogDialog(");
	auto seam_notification_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainNotificationSink(");
	auto seam_interaction_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainInteractionSink(");
	auto seam_single_choice_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainSingleChoiceInteractionSink(");

	auto seam_message_box_include_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wx_message_box_ui_services.h");
	auto seam_single_choice_include_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wx_single_choice_dialog.h");
	auto seam_message_box_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "wxMessageBox(");
	auto seam_single_choice_dialog_hits = FindLiteralHits(wx_frame_main_dialog_ui_host_h, "ShowSingleChoiceDialog(");

	EXPECT_TRUE(message_box_include_hits.empty()) << JoinLines(message_box_include_hits);
	EXPECT_TRUE(single_choice_include_hits.empty()) << JoinLines(single_choice_include_hits);
	EXPECT_TRUE(notification_class_hits.empty()) << JoinLines(notification_class_hits);
	EXPECT_TRUE(interaction_class_hits.empty()) << JoinLines(interaction_class_hits);
	EXPECT_TRUE(single_choice_class_hits.empty()) << JoinLines(single_choice_class_hits);
	EXPECT_FALSE(startup_log_hits.empty());
	EXPECT_FALSE(seam_notification_hits.empty());
	EXPECT_FALSE(seam_interaction_hits.empty());
	EXPECT_FALSE(seam_single_choice_hits.empty());

	EXPECT_FALSE(seam_message_box_include_hits.empty());
	EXPECT_FALSE(seam_single_choice_include_hits.empty());
	EXPECT_FALSE(seam_message_box_hits.empty());
	EXPECT_FALSE(seam_single_choice_dialog_hits.empty());
}

TEST(host_boundary_policy, frame_main_request_selection_fallback_lives_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_request_host_h = root / "src" / "wx_frame_main_request_host.h";

	auto file_dialog_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainFileDialogService");
	auto video_source_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainVideoSourceRequestService");
	auto open_file_hits = FindLiteralHits(frame_main_cpp, "OpenFileSelector(");
	auto open_files_hits = FindLiteralHits(frame_main_cpp, "OpenFilesSelector(");
	auto save_file_hits = FindLiteralHits(frame_main_cpp, "SaveFileSelector(");
	auto select_directory_hits = FindLiteralHits(frame_main_cpp, "SelectDirectorySelector(");
	auto dummy_video_hits = FindLiteralHits(frame_main_cpp, "CreateDummyVideo(");
	auto seam_file_dialog_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainFileDialogService(");
	auto seam_video_source_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainVideoSourceRequestService(");

	auto seam_wx_file_dialog_include_hits = FindLiteralHits(wx_frame_main_request_host_h, "wx_file_dialog_services.h");
	auto seam_make_window_file_dialog_hits = FindLiteralHits(wx_frame_main_request_host_h, "MakeWindowFileDialogService(");
	auto seam_dummy_video_hits = FindLiteralHits(wx_frame_main_request_host_h, "CreateDummyVideo(");

	EXPECT_TRUE(file_dialog_class_hits.empty()) << JoinLines(file_dialog_class_hits);
	EXPECT_TRUE(video_source_class_hits.empty()) << JoinLines(video_source_class_hits);
	EXPECT_TRUE(open_file_hits.empty()) << JoinLines(open_file_hits);
	EXPECT_TRUE(open_files_hits.empty()) << JoinLines(open_files_hits);
	EXPECT_TRUE(save_file_hits.empty()) << JoinLines(save_file_hits);
	EXPECT_TRUE(select_directory_hits.empty()) << JoinLines(select_directory_hits);
	EXPECT_TRUE(dummy_video_hits.empty()) << JoinLines(dummy_video_hits);
	EXPECT_FALSE(seam_file_dialog_hits.empty());
	EXPECT_FALSE(seam_video_source_hits.empty());

	EXPECT_FALSE(seam_wx_file_dialog_include_hits.empty());
	EXPECT_FALSE(seam_make_window_file_dialog_hits.empty());
	EXPECT_FALSE(seam_dummy_video_hits.empty());
}

TEST(host_boundary_policy, frame_main_runtime_capabilities_live_in_explicit_wx_host_seam) {
	auto const root = ProjectRoot();
	auto const frame_main_cpp = root / "src" / "frame_main.cpp";
	auto const wx_frame_main_runtime_host_h = root / "src" / "wx_frame_main_runtime_host.h";

	auto background_runner_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainBackgroundRunner");
	auto background_runner_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainBackgroundRunnerFactory");
	auto project_ui_state_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainProjectUiStateSink");
	auto audio_player_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainAudioPlayerFactoryService");
	auto automation_runner_factory_class_hits = FindLiteralHits(frame_main_cpp, "class FrameMainAutomationBackgroundScriptRunnerFactory");
	auto dialog_progress_hits = FindLiteralHits(frame_main_cpp, "DialogProgress");
	auto audio_player_factory_hits = FindLiteralHits(frame_main_cpp, "AudioPlayerFactory::GetAudioPlayer(");
	auto automation_runner_hits = FindLiteralHits(frame_main_cpp, "Automation4::BackgroundScriptRunner");
	auto seam_background_runner_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainBackgroundRunnerFactory(");
	auto seam_project_ui_state_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainProjectUiStateSink(");
	auto seam_audio_player_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainAudioPlayerFactoryService(");
	auto seam_automation_runner_factory_hits = FindLiteralHits(frame_main_cpp, "MakeFrameMainAutomationBackgroundScriptRunnerFactory(");

	auto seam_dialog_progress_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "DialogProgress");
	auto seam_audio_player_factory_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "AudioPlayerFactory::GetAudioPlayer(");
	auto seam_automation_runner_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "Automation4::BackgroundScriptRunner");
	auto seam_file_dialog_factory_hits = FindLiteralHits(wx_frame_main_runtime_host_h, "MakeFrameMainFileDialogService(");

	EXPECT_TRUE(background_runner_class_hits.empty()) << JoinLines(background_runner_class_hits);
	EXPECT_TRUE(background_runner_factory_class_hits.empty()) << JoinLines(background_runner_factory_class_hits);
	EXPECT_TRUE(project_ui_state_class_hits.empty()) << JoinLines(project_ui_state_class_hits);
	EXPECT_TRUE(audio_player_factory_class_hits.empty()) << JoinLines(audio_player_factory_class_hits);
	EXPECT_TRUE(automation_runner_factory_class_hits.empty()) << JoinLines(automation_runner_factory_class_hits);
	EXPECT_TRUE(dialog_progress_hits.empty()) << JoinLines(dialog_progress_hits);
	EXPECT_TRUE(audio_player_factory_hits.empty()) << JoinLines(audio_player_factory_hits);
	EXPECT_TRUE(automation_runner_hits.empty()) << JoinLines(automation_runner_hits);
	EXPECT_FALSE(seam_background_runner_hits.empty());
	EXPECT_FALSE(seam_project_ui_state_hits.empty());
	EXPECT_FALSE(seam_audio_player_hits.empty());
	EXPECT_FALSE(seam_automation_runner_factory_hits.empty());

	EXPECT_FALSE(seam_dialog_progress_hits.empty());
	EXPECT_FALSE(seam_audio_player_factory_hits.empty());
	EXPECT_FALSE(seam_automation_runner_hits.empty());
	EXPECT_FALSE(seam_file_dialog_factory_hits.empty());
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
	auto const gui_locale_host_cpp = root / "src" / "gui_wx_locale_host.cpp";
	auto const gui_runtime_host_cpp = root / "src" / "gui_wx_runtime_host.cpp";
	auto const aegisublocale_cpp = root / "src" / "aegisublocale.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto const legacy_headless_runtime_host_cpp = root / "src" / "headless_wx_runtime_host.cpp";

	auto main_log_hits = FindLiteralHits(main_cpp, "wxLog::GetActiveTarget");
	auto main_png_hits = FindLiteralHits(main_cpp, "wxPNGHandler");
	auto main_locale_host_hits = FindLiteralHits(main_cpp, "wxTranslations");
	auto gui_log_hits = FindLiteralHits(gui_runtime_host_cpp, "wxLog::GetActiveTarget");
	auto gui_png_hits = FindLiteralHits(gui_runtime_host_cpp, "wxPNGHandler");
	auto gui_locale_translation_hits = FindLiteralHits(gui_locale_host_cpp, "wxTranslations");
	auto gui_locale_catalog_hits = FindLiteralHits(gui_locale_host_cpp, "AddCatalogLookupPathPrefix");
	auto locale_cpp_wx_hits = FindWxMarkers(aegisublocale_cpp);
	auto headless_log_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxLog::GetActiveTarget");
	auto headless_png_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxPNGHandler");
	auto headless_locale_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxTranslations");
	auto headless_host_include_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_wx_runtime_host.h");

	EXPECT_TRUE(main_log_hits.empty()) << JoinLines(main_log_hits);
	EXPECT_TRUE(main_png_hits.empty()) << JoinLines(main_png_hits);
	EXPECT_TRUE(main_locale_host_hits.empty()) << JoinLines(main_locale_host_hits);
	EXPECT_FALSE(gui_log_hits.empty());
	EXPECT_FALSE(gui_png_hits.empty());
	EXPECT_FALSE(gui_locale_translation_hits.empty());
	EXPECT_FALSE(gui_locale_catalog_hits.empty());
	EXPECT_TRUE(locale_cpp_wx_hits.empty()) << JoinLines(locale_cpp_wx_hits);
	EXPECT_TRUE(headless_log_hits.empty()) << JoinLines(headless_log_hits);
	EXPECT_TRUE(headless_png_hits.empty()) << JoinLines(headless_png_hits);
	EXPECT_TRUE(headless_locale_hits.empty()) << JoinLines(headless_locale_hits);
	EXPECT_TRUE(headless_host_include_hits.empty()) << JoinLines(headless_host_include_hits);
	EXPECT_FALSE(std::filesystem::exists(legacy_headless_runtime_host_cpp));
}

TEST(host_boundary_policy, runtime_process_and_optional_facility_contracts_stay_split) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_runtime_h = root / "src" / "app_runtime.h";
	auto const app_runtime_cpp = root / "src" / "app_runtime.cpp";
	auto const app_runtime_facilities_cpp = root / "src" / "app_runtime_facilities.cpp";
	auto const gui_runtime_host_h = root / "src" / "gui_wx_runtime_host.h";
	auto const gui_runtime_host_cpp = root / "src" / "gui_wx_runtime_host.cpp";
	auto const runtime_process_host_h = root / "src" / "runtime_process_host.h";
	auto const runtime_optional_facility_host_h = root / "src" / "runtime_optional_facility_host.h";

	auto app_runtime_process_hook_hits = FindLiteralHits(app_runtime_cpp, "process_host.prime_process_logging");
	auto app_runtime_optional_png_hits = FindLiteralHits(app_runtime_cpp, "optional_facility_host.install_png_image_handler");
	auto app_runtime_legacy_png_hits = FindLiteralHits(app_runtime_cpp, "host_hooks.install_png_image_handler");
	auto facilities_process_hook_hits = FindLiteralHits(app_runtime_facilities_cpp, "process_host.prime_process_logging");
	auto facilities_optional_png_hits = FindLiteralHits(app_runtime_facilities_cpp, "optional_facility_host.install_png_image_handler");
	auto facilities_legacy_png_hits = FindLiteralHits(app_runtime_facilities_cpp, "host_hooks.install_png_image_handler");
	auto runtime_header_process_hits = FindLiteralHits(app_runtime_h, "RuntimeProcessHost process_host");
	auto runtime_header_optional_hits = FindLiteralHits(app_runtime_h, "RuntimeOptionalFacilityHost optional_facility_host");
	auto gui_runtime_header_process_hits = FindLiteralHits(gui_runtime_host_h, "runtime_process_host.h");
	auto gui_runtime_header_optional_hits = FindLiteralHits(gui_runtime_host_h, "runtime_optional_facility_host.h");
	auto gui_runtime_cpp_process_builder_hits = FindLiteralHits(gui_runtime_host_cpp, "BuildGuiWxRuntimeProcessHost()");
	auto gui_runtime_cpp_optional_builder_hits = FindLiteralHits(gui_runtime_host_cpp, "BuildGuiWxRuntimeOptionalFacilityHost()");
	auto main_process_builder_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeProcessHost()");
	auto main_optional_builder_hits = FindLiteralHits(main_cpp, "BuildGuiWxRuntimeOptionalFacilityHost()");
	auto process_header_wx_hits = FindWxMarkers(runtime_process_host_h);
	auto optional_header_wx_hits = FindWxMarkers(runtime_optional_facility_host_h);

	EXPECT_FALSE(app_runtime_process_hook_hits.empty());
	EXPECT_TRUE(app_runtime_optional_png_hits.empty()) << JoinLines(app_runtime_optional_png_hits);
	EXPECT_TRUE(app_runtime_legacy_png_hits.empty()) << JoinLines(app_runtime_legacy_png_hits);
	EXPECT_TRUE(facilities_process_hook_hits.empty()) << JoinLines(facilities_process_hook_hits);
	EXPECT_FALSE(facilities_optional_png_hits.empty());
	EXPECT_TRUE(facilities_legacy_png_hits.empty()) << JoinLines(facilities_legacy_png_hits);
	EXPECT_FALSE(runtime_header_process_hits.empty());
	EXPECT_FALSE(runtime_header_optional_hits.empty());
	EXPECT_FALSE(gui_runtime_header_process_hits.empty());
	EXPECT_FALSE(gui_runtime_header_optional_hits.empty());
	EXPECT_FALSE(gui_runtime_cpp_process_builder_hits.empty());
	EXPECT_FALSE(gui_runtime_cpp_optional_builder_hits.empty());
	EXPECT_FALSE(main_process_builder_hits.empty());
	EXPECT_FALSE(main_optional_builder_hits.empty());
	EXPECT_TRUE(process_header_wx_hits.empty()) << JoinLines(process_header_wx_hits);
	EXPECT_TRUE(optional_header_wx_hits.empty()) << JoinLines(optional_header_wx_hits);
}

TEST(host_boundary_policy, headless_runtime_bootstrap_uses_minimal_runtime_init_options) {
	auto const root = ProjectRoot();
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";

	auto commands_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.initialize_commands = false;");
	auto locale_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.initialize_ui_locale = false;");
	auto automation_factory_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.register_automation_script_factory = false;");
	auto font_warmup_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.warm_subtitles_provider_font_cache = false;");
	auto export_filters_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.register_export_filters = false;");
	auto png_disabled_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "options.install_png_handler = false;");

	EXPECT_FALSE(commands_disabled_hits.empty());
	EXPECT_FALSE(locale_disabled_hits.empty());
	EXPECT_FALSE(automation_factory_disabled_hits.empty());
	EXPECT_FALSE(font_warmup_disabled_hits.empty());
	EXPECT_FALSE(export_filters_disabled_hits.empty());
	EXPECT_FALSE(png_disabled_hits.empty());
}

TEST(host_boundary_policy, shared_exe_headless_entry_flows_directly_to_plain_process_host) {
	auto const root = ProjectRoot();
	auto const app_entry_cpp = root / "src" / "app_entry.cpp";
	auto const headless_process_entry_cpp = root / "src" / "headless_process_entry.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto const wx_headless_process_host_cpp = root / "src" / "wx_headless_process_host.cpp";
	auto const wx_headless_process_host_h = root / "src" / "wx_headless_process_host.h";

	auto app_entry_initializer_hits = FindLiteralHits(app_entry_cpp, "wxInitializer");
	auto app_entry_plain_entry_include_hits = FindLiteralHits(app_entry_cpp, "headless_process_entry.h");
	auto app_entry_runtime_include_hits = FindLiteralHits(app_entry_cpp, "headless_runtime_bootstrap.h");
	auto app_entry_plain_entry_call_hits = FindLiteralHits(app_entry_cpp, "IsHeadlessEntryCommandLine(args)");
	auto app_entry_plain_host_call_hits = FindLiteralHits(app_entry_cpp, "RunHeadlessCommandLineInPlainProcessHost(args)");
	auto process_entry_runtime_include_hits = FindLiteralHits(headless_process_entry_cpp, "headless_runtime_bootstrap.h");
	auto process_entry_runtime_call_hits = FindLiteralHits(headless_process_entry_cpp, "RunHeadlessCommandLine(args)");
	auto process_entry_wx_hits = FindWxMarkers(headless_process_entry_cpp);
	auto bootstrap_initializer_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wxInitializer");

	EXPECT_TRUE(app_entry_initializer_hits.empty()) << JoinLines(app_entry_initializer_hits);
	EXPECT_FALSE(app_entry_plain_entry_include_hits.empty());
	EXPECT_TRUE(app_entry_runtime_include_hits.empty()) << JoinLines(app_entry_runtime_include_hits);
	EXPECT_FALSE(app_entry_plain_entry_call_hits.empty());
	EXPECT_FALSE(app_entry_plain_host_call_hits.empty());
	EXPECT_FALSE(process_entry_runtime_include_hits.empty());
	EXPECT_FALSE(process_entry_runtime_call_hits.empty());
	EXPECT_TRUE(process_entry_wx_hits.empty()) << JoinLines(process_entry_wx_hits);
	EXPECT_TRUE(bootstrap_initializer_hits.empty()) << JoinLines(bootstrap_initializer_hits);
	EXPECT_FALSE(std::filesystem::exists(wx_headless_process_host_cpp));
	EXPECT_FALSE(std::filesystem::exists(wx_headless_process_host_h));
}

TEST(host_boundary_policy, shared_process_config_globals_live_outside_gui_main) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_process_config_cpp = root / "src" / "app_process_config.cpp";

	auto main_config_namespace_hits = FindLiteralHits(main_cpp, "namespace config {");
	auto main_opt_hits = FindLiteralHits(main_cpp, "agi::Options *opt = nullptr;");
	auto main_mru_hits = FindLiteralHits(main_cpp, "agi::MRUManager *mru = nullptr;");
	auto main_path_hits = FindLiteralHits(main_cpp, "agi::Path *path = nullptr;");
	auto main_scripts_hits = FindLiteralHits(main_cpp, "Automation4::AutoloadScriptManager *global_scripts");

	auto config_namespace_hits = FindLiteralHits(app_process_config_cpp, "namespace config {");
	auto config_opt_hits = FindLiteralHits(app_process_config_cpp, "agi::Options *opt = nullptr;");
	auto config_mru_hits = FindLiteralHits(app_process_config_cpp, "agi::MRUManager *mru = nullptr;");
	auto config_path_hits = FindLiteralHits(app_process_config_cpp, "agi::Path *path = nullptr;");
	auto config_scripts_hits = FindLiteralHits(app_process_config_cpp, "Automation4::AutoloadScriptManager *global_scripts = nullptr;");

	EXPECT_TRUE(main_config_namespace_hits.empty()) << JoinLines(main_config_namespace_hits);
	EXPECT_TRUE(main_opt_hits.empty()) << JoinLines(main_opt_hits);
	EXPECT_TRUE(main_mru_hits.empty()) << JoinLines(main_mru_hits);
	EXPECT_TRUE(main_path_hits.empty()) << JoinLines(main_path_hits);
	EXPECT_TRUE(main_scripts_hits.empty()) << JoinLines(main_scripts_hits);

	EXPECT_FALSE(config_namespace_hits.empty());
	EXPECT_FALSE(config_opt_hits.empty());
	EXPECT_FALSE(config_mru_hits.empty());
	EXPECT_FALSE(config_path_hits.empty());
	EXPECT_FALSE(config_scripts_hits.empty());
}

TEST(host_boundary_policy, shared_headless_entry_bootstrap_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_entry_sources = {
		"src/app_entry.cpp",
	};
	std::set<std::string> const expected_headless_entry_bootstrap_entries = {
		"${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}",
		"${AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES}",
		"src/headless_process_entry.cpp",
		"src/headless_runtime_bootstrap.cpp",
	};

	auto entry_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_EXE_ENTRY_SOURCES");
	auto headless_entry_bootstrap_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES");
	auto entry_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_EXE_ENTRY_SOURCES}");
	auto runtime_common_init_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}");
	auto headless_entry_bootstrap_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_HEADLESS_ENTRY_BOOTSTRAP_SOURCES}");

	EXPECT_EQ(expected_entry_sources, entry_sources);
	EXPECT_EQ(expected_headless_entry_bootstrap_entries, headless_entry_bootstrap_sources);
	EXPECT_FALSE(entry_expansion_hits.empty());
	EXPECT_FALSE(runtime_common_init_expansion_hits.empty());
	EXPECT_FALSE(headless_entry_bootstrap_expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_common_init_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}",
		"src/app_process_config.cpp",
		"src/app_runtime.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES");
	auto support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(support_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_common_init_support_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}",
		"src/app_runtime_init.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES");
	auto optional_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_COMMON_INIT_SUPPORT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(optional_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_optional_facility_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/app_runtime_facilities.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_OPTIONAL_FACILITY_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_locale_core_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}",
		"src/aegisublocale.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES");
	auto locale_support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_CORE_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(locale_support_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_locale_support_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/locale_pick.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_RUNTIME_LOCALE_SUPPORT_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, gui_runtime_wx_host_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_runtime_host.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_GUI_RUNTIME_WX_HOST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, gui_bootstrap_wx_host_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/gui_wx_bootstrap_ui_host.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_GUI_BOOTSTRAP_WX_HOST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_selection_request_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/charset_choice.cpp",
		"src/locale_choice.cpp",
		"src/subtitle_fps_choice.cpp",
		"src/track_choice.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_SELECTION_REQUEST_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_SELECTION_REQUEST_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_cli_inspect_service_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/ass_info_service.cpp",
		"src/media_inspect_service.cpp",
		"src/trace_inspect_service.cpp",
		"src/trace_summary_service.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_headless_cli_inspect_batch_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_entries = {
		"${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}",
		"src/headless_cli.cpp",
		"src/headless_cli_batch.cpp",
		"src/headless_cli_internal.cpp",
		"src/headless_cli_execute.cpp",
		"src/headless_playback_probe.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_HEADLESS_CLI_INSPECT_BATCH_SOURCES");
	auto cli_inspect_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_CLI_INSPECT_SERVICE_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_HEADLESS_CLI_INSPECT_BATCH_SOURCES}");

	EXPECT_EQ(expected_entries, sources);
	EXPECT_FALSE(cli_inspect_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_headless_bootstrap_depends_on_narrow_cli_headers) {
	auto const root = ProjectRoot();
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";
	auto const headless_cli_h = root / "src" / "headless_cli.h";
	auto const headless_cli_parse_h = root / "src" / "headless_cli_parse.h";
	auto const headless_cli_execute_h = root / "src" / "headless_cli_execute.h";
	auto const headless_cli_internal_h = root / "src" / "headless_cli_internal.h";
	auto const headless_cli_command_model_h = root / "src" / "headless_cli_command_model.h";

	auto bootstrap_umbrella_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_cli.h");
	auto bootstrap_parse_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_cli_parse.h");
	auto bootstrap_execute_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "headless_cli_execute.h");

	auto internal_umbrella_hits = FindLiteralHits(headless_cli_internal_h, "headless_cli.h");
	auto internal_command_model_hits = FindLiteralHits(headless_cli_internal_h, "headless_cli_command_model.h");

	auto umbrella_parse_hits = FindLiteralHits(headless_cli_h, "headless_cli_parse.h");
	auto umbrella_execute_hits = FindLiteralHits(headless_cli_h, "headless_cli_execute.h");
	auto umbrella_service_hits = FindLiteralHits(headless_cli_h, "playback_session_service.h");
	auto parse_model_hits = FindLiteralHits(headless_cli_parse_h, "headless_cli_command_model.h");
	auto execute_model_hits = FindLiteralHits(headless_cli_execute_h, "headless_cli_command_model.h");

	EXPECT_TRUE(bootstrap_umbrella_hits.empty()) << JoinLines(bootstrap_umbrella_hits);
	EXPECT_FALSE(bootstrap_parse_hits.empty());
	EXPECT_FALSE(bootstrap_execute_hits.empty());

	EXPECT_TRUE(internal_umbrella_hits.empty()) << JoinLines(internal_umbrella_hits);
	EXPECT_FALSE(internal_command_model_hits.empty());

	EXPECT_FALSE(umbrella_parse_hits.empty());
	EXPECT_FALSE(umbrella_execute_hits.empty());
	EXPECT_TRUE(umbrella_service_hits.empty()) << JoinLines(umbrella_service_hits);
	EXPECT_FALSE(parse_model_hits.empty());
	EXPECT_FALSE(execute_model_hits.empty());
}

TEST(host_boundary_policy, shared_playback_project_session_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_support_sources = {
		"src/headless_playback_session_host.cpp",
		"src/playback_probe_timer.cpp",
		"src/playback_session_timer.cpp",
		"${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}",
		"src/project_query_service.cpp",
	};
	std::set<std::string> const expected_core_sources = {
		"src/playback_probe_service.cpp",
		"src/playback_session_service.cpp",
		"src/project_session_service.cpp",
	};
	std::set<std::string> const expected_aggregate_entries = {
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES}",
		"${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}",
	};

	auto support_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES");
	auto project_media_open_query_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}");
	auto core_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES");
	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SOURCES");
	auto support_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SUPPORT_SOURCES}");
	auto core_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_CORE_SOURCES}");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PLAYBACK_PROJECT_SESSION_SOURCES}");

	EXPECT_EQ(expected_support_sources, support_sources);
	EXPECT_EQ(expected_core_sources, core_sources);
	EXPECT_EQ(expected_aggregate_entries, sources);
	EXPECT_FALSE(project_media_open_query_expansion_hits.empty());
	EXPECT_FALSE(support_expansion_hits.empty());
	EXPECT_FALSE(core_expansion_hits.empty());
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_project_media_open_query_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/playback_query_service.cpp",
		"src/project_open_service.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROJECT_MEDIA_OPEN_QUERY_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_selection_request_helpers_keep_wx_at_single_choice_adapter_edge) {
	auto const root = ProjectRoot();
	auto const charset_choice_cpp = root / "src" / "charset_choice.cpp";
	auto const locale_choice_cpp = root / "src" / "locale_choice.cpp";
	auto const subtitle_fps_choice_cpp = root / "src" / "subtitle_fps_choice.cpp";
	auto const track_choice_cpp = root / "src" / "track_choice.cpp";
	auto const wx_single_choice_dialog_h = root / "src" / "wx_single_choice_dialog.h";

	auto charset_choice_wx_hits = FindWxMarkers(charset_choice_cpp);
	auto locale_choice_wx_hits = FindWxMarkers(locale_choice_cpp);
	auto subtitle_fps_choice_wx_hits = FindWxMarkers(subtitle_fps_choice_cpp);
	auto track_choice_wx_hits = FindWxMarkers(track_choice_cpp);
	auto charset_choice_request_hits = FindLiteralHits(charset_choice_cpp, "charset_choice.detected_charsets");
	auto locale_choice_request_hits = FindLiteralHits(locale_choice_cpp, "locale_choice.ui_language");
	auto subtitle_fps_choice_request_hits = FindLiteralHits(subtitle_fps_choice_cpp, "subtitle_fps_choice.selection");
	auto adapter_charset_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "charset_choice.detected_charsets");
	auto adapter_locale_choice_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "locale_choice.ui_language");
	auto adapter_subtitle_fps_request_hits = FindLiteralHits(wx_single_choice_dialog_h, "subtitle_fps_choice.selection");
	auto adapter_track_audio_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.audio");
	auto adapter_track_subtitle_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.subtitle");
	auto adapter_track_video_hits = FindLiteralHits(wx_single_choice_dialog_h, "track_choice.video");

	EXPECT_TRUE(charset_choice_wx_hits.empty()) << JoinLines(charset_choice_wx_hits);
	EXPECT_TRUE(locale_choice_wx_hits.empty()) << JoinLines(locale_choice_wx_hits);
	EXPECT_TRUE(subtitle_fps_choice_wx_hits.empty()) << JoinLines(subtitle_fps_choice_wx_hits);
	EXPECT_TRUE(track_choice_wx_hits.empty()) << JoinLines(track_choice_wx_hits);
	EXPECT_FALSE(charset_choice_request_hits.empty());
	EXPECT_FALSE(locale_choice_request_hits.empty());
	EXPECT_FALSE(subtitle_fps_choice_request_hits.empty());
	EXPECT_FALSE(adapter_charset_request_hits.empty());
	EXPECT_FALSE(adapter_locale_choice_request_hits.empty());
	EXPECT_FALSE(adapter_subtitle_fps_request_hits.empty());
	EXPECT_FALSE(adapter_track_audio_hits.empty());
	EXPECT_FALSE(adapter_track_subtitle_hits.empty());
	EXPECT_FALSE(adapter_track_video_hits.empty());
}

TEST(host_boundary_policy, shared_provider_track_selection_sources_live_in_named_cmake_pack) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_sources = {
		"src/ffmpegsource_common.cpp",
	};

	auto sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES");
	auto expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_PROVIDER_TRACK_SELECTION_SOURCES}");

	EXPECT_EQ(expected_sources, sources);
	EXPECT_FALSE(expansion_hits.empty());
}

TEST(host_boundary_policy, shared_mkv_subtitle_conditional_backend_sources_live_in_named_cmake_packs) {
	auto const root = ProjectRoot();
	auto const cmake_lists = root / "CMakeLists.txt";

	std::set<std::string> const expected_common_sources = {
		"src/mkv_wrap_common.cpp",
	};
	std::set<std::string> const expected_legacy_sources = {
		"src/MatroskaParser.c",
		"src/mkv_wrap.cpp",
	};
	std::set<std::string> const expected_libmatroska_sources = {
		"src/mkv_wrap_libmatroska.cpp",
	};

	auto common_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES");
	auto legacy_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES");
	auto libmatroska_sources = ReadNamedCMakeSetEntries(cmake_lists, "AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES");
	auto common_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_COMMON_SOURCES}");
	auto legacy_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_LEGACY_BACKEND_SOURCES}");
	auto libmatroska_expansion_hits = FindLiteralHits(cmake_lists, "${AEGISUB_SHARED_MKV_SUBTITLE_LIBMATROSKA_BACKEND_SOURCES}");

	EXPECT_EQ(expected_common_sources, common_sources);
	EXPECT_EQ(expected_legacy_sources, legacy_sources);
	EXPECT_EQ(expected_libmatroska_sources, libmatroska_sources);
	EXPECT_FALSE(common_expansion_hits.empty());
	EXPECT_FALSE(legacy_expansion_hits.empty());
	EXPECT_FALSE(libmatroska_expansion_hits.empty());
}

TEST(host_boundary_policy, shared_inspect_open_query_services_and_provider_diagnostics_stay_wx_free) {
	auto const root = ProjectRoot();
	std::vector<std::filesystem::path> const expected_wx_free_paths = {
		root / "src" / "app_runtime_facilities.cpp",
		root / "src" / "app_runtime_init.cpp",
		root / "src" / "locale_pick.cpp",
		root / "src" / "media_inspect_service.cpp",
		root / "src" / "playback_query_service.cpp",
		root / "src" / "project_open_service.cpp",
		root / "src" / "provider_selection_diagnostics.h",
	};

	for (auto const& path : expected_wx_free_paths) {
		auto hits = FindWxMarkers(path);
		EXPECT_TRUE(hits.empty()) << JoinLines(hits);
	}

	auto const media_inspect_service_cpp = root / "src" / "media_inspect_service.cpp";
	auto media_inspect_provider_diagnostics_hits = FindLiteralHits(media_inspect_service_cpp, "provider_selection_diagnostics.h");
	EXPECT_FALSE(media_inspect_provider_diagnostics_hits.empty());
}

TEST(host_boundary_policy, shared_runtime_common_init_reachable_set_stays_split_from_gui_shell) {
	auto const root = ProjectRoot();
	auto const main_cpp = root / "src" / "main.cpp";
	auto const app_runtime_h = root / "src" / "app_runtime.h";
	auto const app_runtime_cpp = root / "src" / "app_runtime.cpp";
	auto const runtime_bootstrap_ui_host_h = root / "src" / "runtime_bootstrap_ui_host.h";
	auto const runtime_process_host_h = root / "src" / "runtime_process_host.h";
	auto const runtime_optional_facility_host_h = root / "src" / "runtime_optional_facility_host.h";
	auto const app_runtime_facilities_cpp = root / "src" / "app_runtime_facilities.cpp";
	auto const app_runtime_init_cpp = root / "src" / "app_runtime_init.cpp";
	auto const aegisublocale_cpp = root / "src" / "aegisublocale.cpp";
	auto const headless_process_entry_cpp = root / "src" / "headless_process_entry.cpp";
	auto const headless_runtime_bootstrap_cpp = root / "src" / "headless_runtime_bootstrap.cpp";

	auto main_gui_runtime_host_hits = FindLiteralHits(main_cpp, "gui_wx_runtime_host.h");
	auto main_gui_locale_host_hits = FindLiteralHits(main_cpp, "gui_wx_locale_host.h");
	auto main_gui_bootstrap_ui_host_hits = FindLiteralHits(main_cpp, "gui_wx_bootstrap_ui_host.h");

	auto app_runtime_header_wx_hits = FindWxMarkers(app_runtime_h);
	auto runtime_bootstrap_ui_header_wx_hits = FindWxMarkers(runtime_bootstrap_ui_host_h);
	auto runtime_process_host_header_wx_hits = FindWxMarkers(runtime_process_host_h);
	auto runtime_optional_facility_host_header_wx_hits = FindWxMarkers(runtime_optional_facility_host_h);
	auto app_runtime_gui_runtime_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_gui_locale_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_locale_host.h");
	auto app_runtime_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_bootstrap_ui_hits = FindLiteralHits(app_runtime_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_main_header_hits = FindLiteralHits(app_runtime_cpp, "main.h");
	auto app_runtime_facilities_wx_hits = FindWxMarkers(app_runtime_facilities_cpp);
	auto app_runtime_facilities_gui_runtime_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_facilities_gui_locale_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_locale_host.h");
	auto app_runtime_facilities_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_facilities_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_facilities_bootstrap_ui_hits = FindLiteralHits(app_runtime_facilities_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_facilities_main_header_hits = FindLiteralHits(app_runtime_facilities_cpp, "main.h");
	auto app_runtime_init_wx_hits = FindWxMarkers(app_runtime_init_cpp);
	auto app_runtime_init_gui_runtime_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_runtime_host.h");
	auto app_runtime_init_gui_locale_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_locale_host.h");
	auto app_runtime_init_gui_bootstrap_ui_host_hits = FindLiteralHits(app_runtime_init_cpp, "gui_wx_bootstrap_ui_host.h");
	auto app_runtime_init_bootstrap_ui_hits = FindLiteralHits(app_runtime_init_cpp, "wx_app_bootstrap_ui_services.h");
	auto app_runtime_init_main_header_hits = FindLiteralHits(app_runtime_init_cpp, "main.h");
	auto app_runtime_locale_wx_hits = FindWxMarkers(aegisublocale_cpp);

	auto process_entry_wx_hits = FindWxMarkers(headless_process_entry_cpp);
	auto process_entry_main_header_hits = FindLiteralHits(headless_process_entry_cpp, "main.h");
	auto process_entry_gui_runtime_host_hits = FindLiteralHits(headless_process_entry_cpp, "gui_wx_runtime_host.h");

	auto bootstrap_wx_hits = FindWxMarkers(headless_runtime_bootstrap_cpp);
	auto bootstrap_main_header_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "main.h");
	auto bootstrap_gui_runtime_host_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "gui_wx_runtime_host.h");
	auto bootstrap_gui_bootstrap_ui_host_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "gui_wx_bootstrap_ui_host.h");
	auto bootstrap_bootstrap_ui_hits = FindLiteralHits(headless_runtime_bootstrap_cpp, "wx_app_bootstrap_ui_services.h");

	EXPECT_FALSE(main_gui_runtime_host_hits.empty());
	EXPECT_FALSE(main_gui_locale_host_hits.empty());
	EXPECT_FALSE(main_gui_bootstrap_ui_host_hits.empty());

	EXPECT_TRUE(app_runtime_header_wx_hits.empty()) << JoinLines(app_runtime_header_wx_hits);
	EXPECT_TRUE(runtime_bootstrap_ui_header_wx_hits.empty()) << JoinLines(runtime_bootstrap_ui_header_wx_hits);
	EXPECT_TRUE(runtime_process_host_header_wx_hits.empty()) << JoinLines(runtime_process_host_header_wx_hits);
	EXPECT_TRUE(runtime_optional_facility_host_header_wx_hits.empty()) << JoinLines(runtime_optional_facility_host_header_wx_hits);
	EXPECT_TRUE(app_runtime_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_gui_locale_host_hits.empty()) << JoinLines(app_runtime_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_main_header_hits.empty()) << JoinLines(app_runtime_main_header_hits);
	EXPECT_TRUE(app_runtime_facilities_wx_hits.empty()) << JoinLines(app_runtime_facilities_wx_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_locale_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_facilities_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_facilities_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_facilities_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_facilities_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_facilities_main_header_hits.empty()) << JoinLines(app_runtime_facilities_main_header_hits);
	EXPECT_TRUE(app_runtime_init_wx_hits.empty()) << JoinLines(app_runtime_init_wx_hits);
	EXPECT_TRUE(app_runtime_init_gui_runtime_host_hits.empty()) << JoinLines(app_runtime_init_gui_runtime_host_hits);
	EXPECT_TRUE(app_runtime_init_gui_locale_host_hits.empty()) << JoinLines(app_runtime_init_gui_locale_host_hits);
	EXPECT_TRUE(app_runtime_init_gui_bootstrap_ui_host_hits.empty()) << JoinLines(app_runtime_init_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(app_runtime_init_bootstrap_ui_hits.empty()) << JoinLines(app_runtime_init_bootstrap_ui_hits);
	EXPECT_TRUE(app_runtime_init_main_header_hits.empty()) << JoinLines(app_runtime_init_main_header_hits);
	EXPECT_TRUE(app_runtime_locale_wx_hits.empty()) << JoinLines(app_runtime_locale_wx_hits);

	EXPECT_TRUE(process_entry_wx_hits.empty()) << JoinLines(process_entry_wx_hits);
	EXPECT_TRUE(process_entry_main_header_hits.empty()) << JoinLines(process_entry_main_header_hits);
	EXPECT_TRUE(process_entry_gui_runtime_host_hits.empty()) << JoinLines(process_entry_gui_runtime_host_hits);

	EXPECT_TRUE(bootstrap_wx_hits.empty()) << JoinLines(bootstrap_wx_hits);
	EXPECT_TRUE(bootstrap_main_header_hits.empty()) << JoinLines(bootstrap_main_header_hits);
	EXPECT_TRUE(bootstrap_gui_runtime_host_hits.empty()) << JoinLines(bootstrap_gui_runtime_host_hits);
	EXPECT_TRUE(bootstrap_gui_bootstrap_ui_host_hits.empty()) << JoinLines(bootstrap_gui_bootstrap_ui_host_hits);
	EXPECT_TRUE(bootstrap_bootstrap_ui_hits.empty()) << JoinLines(bootstrap_bootstrap_ui_hits);
}

TEST(host_boundary_policy, gui_runtime_and_bootstrap_host_headers_stay_localized_to_gui_entry) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";

	std::set<std::string> const expected_runtime_host_includers = {
		"src/gui_wx_runtime_host.cpp",
		"src/main.cpp",
	};
	std::set<std::string> const expected_runtime_process_host_includers = {
		"src/app_runtime.h",
		"src/gui_wx_runtime_host.h",
	};
	std::set<std::string> const expected_runtime_optional_facility_host_includers = {
		"src/app_runtime.h",
		"src/gui_wx_runtime_host.h",
	};
	std::set<std::string> const expected_locale_host_includers = {
		"src/gui_wx_locale_host.cpp",
		"src/main.cpp",
	};
	std::set<std::string> const expected_bootstrap_host_includers = {
		"src/gui_wx_bootstrap_ui_host.cpp",
		"src/main.cpp",
	};
	std::set<std::string> const expected_bootstrap_ui_includers = {
		"src/gui_wx_bootstrap_ui_host.cpp",
	};

	auto runtime_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_runtime_host.h");
	auto runtime_process_host_includers = FindFilesContainingLiteralInTree(root, src_root, "runtime_process_host.h");
	auto runtime_optional_facility_host_includers = FindFilesContainingLiteralInTree(root, src_root, "runtime_optional_facility_host.h");
	auto locale_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_locale_host.h");
	auto bootstrap_host_includers = FindFilesContainingLiteralInTree(root, src_root, "gui_wx_bootstrap_ui_host.h");
	auto bootstrap_ui_includers = FindFilesContainingLiteralInTree(root, src_root, "wx_app_bootstrap_ui_services.h");

	EXPECT_EQ(expected_runtime_host_includers, runtime_host_includers);
	EXPECT_EQ(expected_runtime_process_host_includers, runtime_process_host_includers);
	EXPECT_EQ(expected_runtime_optional_facility_host_includers, runtime_optional_facility_host_includers);
	EXPECT_EQ(expected_locale_host_includers, locale_host_includers);
	EXPECT_EQ(expected_bootstrap_host_includers, bootstrap_host_includers);
	EXPECT_EQ(expected_bootstrap_ui_includers, bootstrap_ui_includers);
}

TEST(host_boundary_policy, explicit_wx_surface_inventory_stays_current) {
	auto const root = ProjectRoot();
	auto const src_root = root / "src";
	ASSERT_TRUE(std::filesystem::exists(src_root));

	std::set<std::string> const expected_explicit_wx_surfaces = {
		"src/gui_wx_bootstrap_ui_host.cpp",
		"src/gui_wx_bootstrap_ui_host.h",
		"src/wx_app_bootstrap_ui_services.h",
		"src/wx_automation_file_dialog_service.h",
		"src/wx_file_dialog_services.h",
		"src/wx_frame_main_dialog_ui_host.h",
		"src/wx_frame_main_request_host.h",
		"src/wx_frame_main_runtime_host.h",
		"src/wx_preferences_ui_host.h",
		"src/wx_style_editor_ui_host.h",
		"src/gui_wx_locale_host.cpp",
		"src/gui_wx_locale_host.h",
		"src/gui_wx_runtime_host.cpp",
		"src/gui_wx_runtime_host.h",
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
