#include "../../src/subtitle_format.h"

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <utility>

void LoadDefaultAssFileWithAppOptions(AssFile& file, bool include_dialogue_line, std::string const&) {
	file.LoadDefault(false);
	if (include_dialogue_line)
		file.Events.push_back(*new AssDialogue);
}

std::string GetSubtitleFormatDefaultStyleCatalog(std::string const&) {
	return {};
}

std::string GetTextImportActorSeparator() {
	return ":";
}

std::string GetTextImportCommentStarter() {
	return "#";
}

bool GetTextImportIncludeBlank() {
	return false;
}

ScriptResolutionType GetAppScriptResolutionPreference() {
	return ScriptResolutionType::PlayRes;
}

SubtitleFormat::SubtitleFormat(std::string name)
: name(std::move(name)) {
}

bool SubtitleFormat::CanReadFile(agi::fs::path const&, std::string const&) const {
	return false;
}

bool SubtitleFormat::CanWriteFile(agi::fs::path const&) const {
	return false;
}

bool SubtitleFormat::CanSave(const AssFile *) const {
	return true;
}

void SubtitleFormat::StripTags(AssFile&) {
}

void SubtitleFormat::StripComments(AssFile& file) {
	for (auto it = file.Events.begin(); it != file.Events.end();) {
		if (it->Comment || it->Text.get().empty())
			it = file.Events.erase(it);
		else
			++it;
	}
}

void SubtitleFormat::RecombineOverlaps(AssFile&) {
}

void SubtitleFormat::MergeIdentical(AssFile&) {
}

void SubtitleFormat::ConvertNewlines(AssFile& file, std::string const& newline, bool) {
	for (auto& event : file.Events) {
		auto text = event.Text.get();
		size_t pos = 0;
		while ((pos = text.find("\\N", pos)) != std::string::npos) {
			text.replace(pos, 2, newline);
			pos += newline.size();
		}
		event.Text = text;
	}
}
