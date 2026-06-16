// Copyright (c) 2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file subtitle_format_ttxt.cpp
/// @brief Reading/writing MPEG-4 Timed Text subtitles in TTXT XML format
/// @ingroup subtitle_io
///

#include "subtitle_format_ttxt.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <pugixml.hpp>

DEFINE_EXCEPTION(TTXTParseError, SubtitleFormatParseError);

TTXTSubtitleFormat::TTXTSubtitleFormat()
: SubtitleFormat("MPEG-4 Streaming Text")
{
}

std::vector<std::string> TTXTSubtitleFormat::GetReadWildcards() const {
	return {"ttxt"};
}

std::vector<std::string> TTXTSubtitleFormat::GetWriteWildcards() const {
	return GetReadWildcards();
}

void TTXTSubtitleFormat::ReadFile(AssFile *target, agi::fs::path const& filename, agi::vfr::Framerate const& fps, std::string const& encoding, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink, std::shared_ptr<agi::BackgroundRunnerFactory>) const {
	(void)choice_sink;
	LoadDefaultAssFileWithAppOptions(*target, false, OPT_GET("Subtitle Format/TTXT/Default Style Catalog")->GetString());

	// Load XML document
	pugi::xml_document doc;
	auto input = agi::io::Open(filename, true);
	pugi::xml_parse_result result = doc.load(*input);
	if (!result) throw TTXTParseError("Failed loading TTXT XML file.");

	// Check root node name
	pugi::xml_node root = doc.child("TextStream");
	if (!root) throw TTXTParseError("Invalid TTXT file.");

	// Check version
	std::string verStr = root.attribute("version").as_string("");
	int version = -1;
	if (verStr == "1.0")
		version = 0;
	else if (verStr == "1.1")
		version = 1;
	else
		throw TTXTParseError("Unknown TTXT version: " + verStr);

	// Get children
	AssDialogue *diag = nullptr;
	int lines = 0;
	for (pugi::xml_node child : root.children()) {
		// Line
		if (std::string(child.name()) == "TextSample") {
			if ((diag = ProcessLine(child, diag, version))) {
				lines++;
				target->Events.push_back(*diag);
			}
		}
		// Header
		else if (std::string(child.name()) == "TextStreamHeader") {
			ProcessHeader(child);
		}
	}

	// No lines?
	if (lines == 0)
		target->Events.push_back(*new AssDialogue);
}

AssDialogue *TTXTSubtitleFormat::ProcessLine(pugi::xml_node node, AssDialogue *prev, int version) const {
	// Get time
	std::string sampleTime = node.attribute("sampleTime").as_string("00:00:00.000");
	agi::Time time(sampleTime);

	// Set end time of last line
	if (prev)
		prev->End = time;

	// Get text
	std::string text;
	if (version == 0)
		text = node.attribute("text").as_string("");
	else
		text = node.child_value();

	// Create line
	if (text.empty()) return nullptr;

	// Create dialogue
	auto diag = new AssDialogue;
	diag->Start = time;
	diag->End = 36000000-10;

	// Process text for 1.0
	if (version == 0) {
		std::string finalText;
		finalText.reserve(text.size());
		bool in = false;
		bool first = true;
		for (char chr : text) {
			if (chr == '\'') {
				if (!in && !first) finalText += "\\N";
				first = false;
				in = !in;
			}
			else if (in) finalText += chr;
		}
		diag->Text = finalText;
	}

	// Process text for 1.1
	else {
		// Replace \r\n and \n with \N
		std::string processed;
		processed.reserve(text.size());
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\r') {
				// Skip \r
			}
			else if (text[i] == '\n') {
				processed += "\\N";
			}
			else {
				processed += text[i];
			}
		}
		diag->Text = processed;
	}

	return diag;
}

void TTXTSubtitleFormat::ProcessHeader(pugi::xml_node node) const {
	// TODO
}

void TTXTSubtitleFormat::WriteFile(const AssFile *src, agi::fs::path const& filename, agi::vfr::Framerate const& fps, std::string const& encoding, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) const {
	(void)choice_sink;
	// Convert to TTXT
	AssFile copy(*src);
	ConvertToTTXT(copy);

	// Create XML structure
	pugi::xml_document doc;
	pugi::xml_node root = doc.append_child("TextStream");
	root.append_attribute("version").set_value("1.1");

	// Create header
	WriteHeader(root);

	// Create lines
	const AssDialogue *prev = nullptr;
	for (auto const& current : copy.Events) {
		WriteLine(root, prev, &current);
		prev = &current;
	}

	// Save XML
	agi::io::Save output(filename, true);
	doc.save(output.Get());
	output.Close();
}

void TTXTSubtitleFormat::WriteHeader(pugi::xml_node root) const {
	// Write stream header
	pugi::xml_node node = root.append_child("TextStreamHeader");
	node.append_attribute("width").set_value("400");
	node.append_attribute("height").set_value("60");
	node.append_attribute("layer").set_value("0");
	node.append_attribute("translation_x").set_value("0");
	node.append_attribute("translation_y").set_value("0");

	// Write sample description
	pugi::xml_node desc = node.append_child("TextSampleDescription");
	desc.append_attribute("horizontalJustification").set_value("center");
	desc.append_attribute("verticalJustification").set_value("bottom");
	desc.append_attribute("backColor").set_value("0 0 0 0");
	desc.append_attribute("verticalText").set_value("no");
	desc.append_attribute("fillTextRegion").set_value("no");
	desc.append_attribute("continuousKaraoke").set_value("no");
	desc.append_attribute("scroll").set_value("None");

	// Write font table
	pugi::xml_node fontTable = desc.append_child("FontTable");
	pugi::xml_node fontEntry = fontTable.append_child("FontTableEntry");
	fontEntry.append_attribute("fontName").set_value("Sans");
	fontEntry.append_attribute("fontID").set_value("1");

	// Write text box
	pugi::xml_node textBox = desc.append_child("TextBox");
	textBox.append_attribute("top").set_value("0");
	textBox.append_attribute("left").set_value("0");
	textBox.append_attribute("bottom").set_value("60");
	textBox.append_attribute("right").set_value("400");

	// Write style
	pugi::xml_node style = desc.append_child("Style");
	style.append_attribute("styles").set_value("Normal");
	style.append_attribute("fontID").set_value("1");
	style.append_attribute("fontSize").set_value("18");
	style.append_attribute("color").set_value("ff ff ff ff");
}

void TTXTSubtitleFormat::WriteLine(pugi::xml_node root, const AssDialogue *prev, const AssDialogue *line) const {
	// If it doesn't start at the end of previous, add blank
	if (prev && prev->End != line->Start) {
		pugi::xml_node node = root.append_child("TextSample");
		node.append_attribute("sampleTime").set_value(("0" + prev->End.GetAssFormatted(true)).c_str());
		node.append_attribute("xml:space").set_value("preserve");
		node.text().set("");
	}

	// Generate and insert node
	pugi::xml_node node = root.append_child("TextSample");
	node.append_attribute("sampleTime").set_value(("0" + line->Start.GetAssFormatted(true)).c_str());
	node.append_attribute("xml:space").set_value("preserve");
	node.text().set(line->Text.get().c_str());
}

void TTXTSubtitleFormat::ConvertToTTXT(AssFile &file) const {
	file.Sort();
	StripComments(file);
	RecombineOverlaps(file);
	MergeIdentical(file);
	StripTags(file);
	ConvertNewlines(file, "\r\n");

	// Find last line
	agi::Time lastTime;
	if (!file.Events.empty())
		lastTime = file.Events.back().End;

	// Insert blank line at the end
	auto diag = new AssDialogue;
	diag->Start = lastTime;
	diag->End = lastTime+OPT_GET("Timing/Default Duration")->GetInt();
	file.Events.push_back(*diag);
}
