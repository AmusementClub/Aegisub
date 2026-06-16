#include "ass_file.h"
#include "export_framerate_transform.h"
#include "options.h"
#include "presentation/subtitle_grid_query_service.h"
#include "subtitle_format.h"

#include <libaegisub/option.h>
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

std::filesystem::path MakeTempTxtPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_smoke_" + std::to_string(stamp) + ".txt");
}

std::filesystem::path MakeTempStlPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_smoke_" + std::to_string(stamp) + ".stl");
}

constexpr char kCoreSmokeOptionDefaults[] = R"({
	"Subtitle" : {
		"Default Resolution" : {
			"Auto" : true,
			"Width" : 640,
			"Height" : 480
		}
	},
	"Subtitle Format" : {
		"EBU STL" : {
			"Display Standard" : 0,
			"Inclusive End Times" : true,
			"Line Wrapping Mode" : 1,
			"Max Line Length" : 42,
			"TV Standard" : 0,
			"Text Encoding" : 0,
			"Timecode Offset" : {
				"H" : 0,
				"M" : 0,
				"S" : 0,
				"F" : 0
			},
			"Translate Alignments" : true
		},
		"TXT" : {
			"Default Style Catalog" : ""
		}
	},
	"Tool" : {
		"Import" : {
			"Text" : {
				"Actor Separator" : ":",
				"Comment Starter" : "#",
				"Include Blank" : false
			}
		}
	},
	"Timing" : {
		"Default Duration" : 2000
	}
})";

class ScopedCoreSmokeOptions {
	agi::Options options;
	agi::Options *previous = nullptr;

public:
	ScopedCoreSmokeOptions()
	: options("", kCoreSmokeOptionDefaults, agi::Options::FLUSH_SKIP)
	, previous(config::opt) {
		config::opt = &options;
	}

	~ScopedCoreSmokeOptions() {
		config::opt = previous;
	}
};

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

void WriteSmokeTxt(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create smoke TXT file");

	file <<
		"Alice: Hello from TXT\n"
		"# Internal note\n"
		"\n"
		"Bob: Second line\n";
}

int RunSmoke() {
	ScopedCoreSmokeOptions options;

	ScopedFile ass_path(MakeTempAssPath());
	WriteSmokeAss(ass_path.get());

	AssFile file;
	auto const* reader = SubtitleFormat::GetReader(ass_path.get(), "utf-8");
	if (!reader)
		throw std::runtime_error("ASS reader was not registered");
	reader->ReadFile(&file, ass_path.get(), agi::vfr::Framerate(), "utf-8", {});

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

	ScopedFile txt_path(MakeTempTxtPath());
	WriteSmokeTxt(txt_path.get());

	AssFile txt_file;
	auto const* txt_reader = SubtitleFormat::GetReader(txt_path.get(), "utf-8");
	if (!txt_reader)
		throw std::runtime_error("TXT reader was not registered");
	txt_reader->ReadFile(&txt_file, txt_path.get(), agi::vfr::Framerate(), "utf-8", {});

	auto txt_window = aegisub::presentation::QueryVisibleSubtitleRows(txt_file, request, 1);
	if (txt_window.total_rows != 3 || txt_window.rows.size() != 3)
		throw std::runtime_error("unexpected TXT projected subtitle row count");
	if (txt_window.rows[0].actor != "Alice" || txt_window.rows[0].text != "Hello from TXT")
		throw std::runtime_error("TXT reader did not parse actor/text fields");
	if (!txt_window.rows[1].comment || txt_window.rows[1].text != "Internal note")
		throw std::runtime_error("TXT reader did not parse comment prefix");

	ScopedFile stl_path(MakeTempStlPath());
	auto const* stl_writer = SubtitleFormat::GetWriter(stl_path.get());
	if (!stl_writer)
		throw std::runtime_error("EBU STL writer was not registered");
	stl_writer->WriteFile(&file, stl_path.get(), agi::vfr::Framerate(), "");
	if (std::filesystem::file_size(stl_path.get()) < 1024)
		throw std::runtime_error("EBU STL writer produced an unexpectedly small file");

	agi::vfr::Framerate source(24.0);
	agi::vfr::Framerate destination(25.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 1000, 2500);
	if (transform.new_end_ms <= transform.new_start_ms)
		throw std::runtime_error("framerate transform produced an invalid interval");

	std::cout
		<< "aegisub_core_smoke: rows=" << window.total_rows
		<< " txt_rows=" << txt_window.total_rows
		<< " stl_bytes=" << std::filesystem::file_size(stl_path.get())
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
