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
		"src/app_runtime.cpp",
		"src/audio_controller_power_host.cpp",
		"src/headless_runtime_bootstrap.cpp",
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
	auto const wx_ui_services_h = root / "src" / "wx_ui_services.h";

	auto ui_header_hits = FindWxMarkers(ui_services_h);
	auto ui_cpp_hits = FindWxMarkers(ui_services_cpp);
	auto wx_adapter_hits = FindWxMarkers(wx_ui_services_h);

	EXPECT_TRUE(ui_header_hits.empty()) << JoinLines(ui_header_hits);
	EXPECT_TRUE(ui_cpp_hits.empty()) << JoinLines(ui_cpp_hits);
	EXPECT_FALSE(wx_adapter_hits.empty());
}
