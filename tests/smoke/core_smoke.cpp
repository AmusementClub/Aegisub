#include "ass_file.h"
#include "ass_io_core.h"
#include "export_framerate_transform.h"
#include "presentation/subtitle_grid_query_service.h"

#include <libaegisub/vfr.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class ScopedFile {
	std::filesystem::path path;

public:
	explicit ScopedFile(std::filesystem::path path) : path(std::move(path)) { }
	~ScopedFile() {
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

	std::filesystem::path const& get() const { return path; }
};

std::filesystem::path MakeTempAssPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_smoke_" + std::to_string(stamp) + ".ass");
}

void WriteSmokeAss(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create smoke ASS file");

	file <<
		"[Script Info]\n"
		"ScriptType: v4.00+\n"
		"\n"
		"[V4+ Styles]\n"
		"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
		"Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
		"Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
		"Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
		"0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
		"\n"
		"[Events]\n"
		"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:01.00,0:00:02.50,Default,Actor,0000,0000,0000,,Hello core\n"
		"Comment: 1,0:00:03.00,0:00:04.00,Default,,0000,0000,0000,fx,Hidden note\n";
}

int RunSmoke() {
	ScopedFile ass_path(MakeTempAssPath());
	WriteSmokeAss(ass_path.get());

	auto file = ReadAssFileForCore(ass_path.get(), "utf-8");

	aegisub::presentation::VisibleSubtitleRowsRequest request;
	request.first_row = 0;
	request.row_count = 10;

	auto window = aegisub::presentation::QueryVisibleSubtitleRows(file, request, 1);
	if (window.total_rows != 2 || window.rows.size() != 2)
		throw std::runtime_error("unexpected projected subtitle row count");
	if (window.rows[0].text != "Hello core")
		throw std::runtime_error("unexpected projected subtitle text");
	if (!window.rows[1].comment)
		throw std::runtime_error("comment dialogue did not project as comment row");

	agi::vfr::Framerate source(24.0);
	agi::vfr::Framerate destination(25.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 1000, 2500);
	if (transform.new_end_ms <= transform.new_start_ms)
		throw std::runtime_error("framerate transform produced an invalid interval");

	std::cout
		<< "aegisub_core_smoke: rows=" << window.total_rows
		<< " first_text=\"" << window.rows[0].text << "\""
		<< " transformed=[" << transform.new_start_ms << ", " << transform.new_end_ms << "]\n";
	return 0;
}

}

int main() {
	try {
		return RunSmoke();
	}
	catch (std::exception const& err) {
		std::cerr << "aegisub_core_smoke failed: " << err.what() << '\n';
		return 1;
	}
	catch (...) {
		std::cerr << "aegisub_core_smoke failed: unknown exception\n";
		return 1;
	}
}
