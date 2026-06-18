// Copyright (c) 2006, Rodrigo Braz Monteiro
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

/// @file subtitle_format_microdvd.cpp
/// @brief Reading/writing MicroDVD subtitle format (.SUB)
/// @ingroup subtitle_io
///

#include "subtitle_format_microdvd.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "options.h"
#include "subtitle_format_microdvd_parser.h"
#include "text_file_reader.h"
#include "text_file_writer.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>
#include <libaegisub/vfr.h>

#include <boost/regex.hpp>

MicroDVDSubtitleFormat::MicroDVDSubtitleFormat()
: SubtitleFormat("MicroDVD")
{
}

std::vector<std::string> MicroDVDSubtitleFormat::GetReadWildcards() const {
	return {"sub"};
}

std::vector<std::string> MicroDVDSubtitleFormat::GetWriteWildcards() const {
	return GetReadWildcards();
}

static const boost::regex line_regex(R"(^[\{\[]([0-9]+)[\}\]][\{\[]([0-9]+)[\}\]](.*)$)");

bool MicroDVDSubtitleFormat::CanReadFile(agi::fs::path const& filename, std::string const& encoding) const {
	// Return false immediately if extension is wrong
	if (!agi::fs::HasExtension(filename, "sub")) return false;

	// Since there is an infinity of .sub formats, load first line and check if it's valid
	TextFileReader file(filename, encoding);
	if (file.HasMoreLines())
		return regex_match(file.ReadLineFromFile(), line_regex);

	return false;
}

void MicroDVDSubtitleFormat::ReadFile(AssFile *target, agi::fs::path const& filename, agi::vfr::Framerate const& vfps, std::string const& encoding, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink, std::shared_ptr<agi::BackgroundRunnerFactory>) const {
	TextFileReader file(filename, encoding);

	LoadDefaultAssFileWithAppOptions(*target, false, GetSubtitleFormatDefaultStyleCatalog("MicroDVD"));

	agi::vfr::Framerate fps;

	bool isFirst = true;
	while (file.HasMoreLines()) {
		boost::smatch match;
		std::string line = file.ReadLineFromFile();
		if (!regex_match(line, match, line_regex)) continue;

		std::string text = match[3].str();

		// If it's the first, check if it contains fps information
		if (isFirst) {
			isFirst = false;

			double cfr;
			if (agi::util::try_parse(text, &cfr)) {
				fps = cfr;
				continue;
			}

			// If it wasn't an fps line, ask the user for it
			fps = AskForFPS(true, false, vfps, choice_sink);
			if (!fps.IsLoaded()) return;
		}

		int f1 = 0;
		int f2 = 0;
		if (!TryParseMicroDVDFrames(
			match[1].str(),
			match[2].str(),
			f1, f2)) {
			throw SubtitleFormatParseError("Malformed MicroDVD frame number: " + line);
		}

		agi::util::strings::replace_all_inplace(text, "|", "\\N");

		auto diag = new AssDialogue;
		diag->Start = fps.TimeAtFrame(f1, agi::vfr::START);
		diag->End = fps.TimeAtFrame(f2, agi::vfr::END);
		diag->Text = text;
		target->Events.push_back(*diag);
	}
}

void MicroDVDSubtitleFormat::WriteFile(const AssFile *src, agi::fs::path const& filename, agi::vfr::Framerate const& vfps, std::string const& encoding, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) const {
	agi::vfr::Framerate fps = AskForFPS(true, false, vfps, choice_sink);
	if (!fps.IsLoaded()) return;

	AssFile copy(*src);
	copy.Sort();
	StripComments(copy);
	RecombineOverlaps(copy);
	MergeIdentical(copy);
	StripTags(copy);
	ConvertNewlines(copy, "|");

	TextFileWriter file(filename, ResolveWriteEncoding(encoding));

	// Write FPS line
	if (!fps.IsVFR())
		file.WriteLineToFile(agi::format("{1}{1}%.6f", fps.FPS()));

	// Write lines
	for (auto const& current : copy.Events) {
		int start = fps.FrameAtTime(current.Start, agi::vfr::START);
		int end = fps.FrameAtTime(current.End, agi::vfr::END);

		file.WriteLineToFile(agi::format("{%i}{%i}%s", start, end, agi::util::strings::replace_all_copy(current.Text.get(), "\\N", "|")));
	}
	file.Close();
}
