#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/options.h"
#include "../../src/subtitle_format_ttxt.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/option.h>
#include <libaegisub/vfr.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr char kTtxtOptionDefaults[] = R"({
	"Subtitle" : {
		"Default Resolution" : {
			"Auto" : true,
			"Width" : 640,
			"Height" : 480
		}
	},
	"Subtitle Format" : {
		"TTXT" : {
			"Default Style Catalog" : ""
		}
	},
	"Timing" : {
		"Default Duration" : 2000
	}
})";

class ScopedTtxtOptions {
	agi::Options options;
	agi::Options *previous = nullptr;

public:
	ScopedTtxtOptions()
	: options("", kTtxtOptionDefaults, agi::Options::FLUSH_SKIP)
	, previous(config::opt) {
		config::opt = &options;
	}

	~ScopedTtxtOptions() {
		config::opt = previous;
	}
};

agi::fs::path TestPath(std::filesystem::path const& name) {
	auto path = std::filesystem::path("data") / name;
	std::filesystem::remove(path);
	return path;
}

void WriteText(agi::fs::path const& path, std::string const& text) {
	agi::io::Save output(path, true);
	output.Get() << text;
	output.Close();
}

std::string ReadText(agi::fs::path const& path) {
	auto input = agi::io::Open(path, true);
	std::ostringstream buffer;
	buffer << input->rdbuf();
	return buffer.str();
}

std::vector<AssDialogue const*> Events(AssFile const& file) {
	std::vector<AssDialogue const*> events;
	for (auto const& event : file.Events)
		events.push_back(&event);
	return events;
}

agi::vfr::Framerate const& DummyFps() {
	static agi::vfr::Framerate fps;
	return fps;
}

} // namespace

TEST(subtitle_format_ttxt, reads_11_text_samples_with_xml_entities_newlines_and_unicode_path) {
	ScopedTtxtOptions options;
	auto const path = TestPath(std::filesystem::path(u8"ttxt_unicode_路径.ttxt"));
	WriteText(path,
		"<?xml version=\"1.0\"?>\n"
		"<TextStream version=\"1.1\">\n"
		"  <TextSample sampleTime=\"00:00:01.000\" xml:space=\"preserve\">Hello &amp; world\r\nsecond line</TextSample>\n"
		"  <TextSample sampleTime=\"00:00:02.500\" xml:space=\"preserve\"></TextSample>\n"
		"</TextStream>\n");

	AssFile file;
	TTXTSubtitleFormat format;
	format.ReadFile(&file, path, DummyFps(), "", {}, {});

	auto events = Events(file);
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ(1000, events[0]->Start);
	EXPECT_EQ(2500, events[0]->End);
	EXPECT_EQ("Hello & world\\Nsecond line", events[0]->Text.get());
}

TEST(subtitle_format_ttxt, reads_10_attribute_text_samples) {
	ScopedTtxtOptions options;
	auto const path = TestPath("ttxt_v10.ttxt");
	WriteText(path,
		"<?xml version=\"1.0\"?>\n"
		"<TextStream version=\"1.0\">\n"
		"  <TextSample sampleTime=\"00:00:03.000\" text=\"'first line''second line'\" />\n"
		"  <TextSample sampleTime=\"00:00:04.000\" text=\"\" />\n"
		"</TextStream>\n");

	AssFile file;
	TTXTSubtitleFormat format;
	format.ReadFile(&file, path, DummyFps(), "", {}, {});

	auto events = Events(file);
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ(3000, events[0]->Start);
	EXPECT_EQ(4000, events[0]->End);
	EXPECT_EQ("first line\\Nsecond line", events[0]->Text.get());
}

TEST(subtitle_format_ttxt, writes_11_xml_with_escaped_text_and_gap_sample) {
	ScopedTtxtOptions options;
	auto const path = TestPath("ttxt_write.ttxt");

	AssFile file;
	file.LoadDefault(false);
	auto first = new AssDialogue;
	first->Start = 1000;
	first->End = 1500;
	first->Text = "A & B";
	file.Events.push_back(*first);

	auto second = new AssDialogue;
	second->Start = 2000;
	second->End = 2500;
	second->Text = "line 1\\Nline 2";
	file.Events.push_back(*second);

	TTXTSubtitleFormat format;
	format.WriteFile(&file, path, DummyFps(), "", {});

	auto xml = ReadText(path);
	EXPECT_NE(std::string::npos, xml.find("<TextStream version=\"1.1\">"));
	EXPECT_NE(std::string::npos, xml.find("sampleTime=\"00:00:01.000\""));
	EXPECT_NE(std::string::npos, xml.find("A &amp; B"));
	EXPECT_NE(std::string::npos, xml.find("sampleTime=\"00:00:01.500\""));
	EXPECT_NE(std::string::npos, xml.find("sampleTime=\"00:00:02.000\""));
	EXPECT_NE(std::string::npos, xml.find("line 1\r\nline 2"));
}
