#include "track_choice.h"

#include <iomanip>
#include <sstream>

namespace aegisub::track_choice {
namespace {
std::string BuildRequestId(DialogKind kind) {
	switch (kind) {
	case DialogKind::Audio:
		return "track_choice.audio";
	case DialogKind::Subtitle:
		return "track_choice.subtitle";
	case DialogKind::Video:
		return "track_choice.video";
	}

	return {};
}

std::string BuildTitle(DialogKind kind) {
	switch (kind) {
	case DialogKind::Audio:
		return "Choose audio track";
	case DialogKind::Subtitle:
		return "Multiple subtitle tracks found";
	case DialogKind::Video:
		return "Choose video track";
	}

	return {};
}

std::string BuildMessage(DialogKind kind) {
	switch (kind) {
	case DialogKind::Audio:
		return "Multiple audio tracks detected, please choose the one you wish to load:";
	case DialogKind::Subtitle:
		return "Choose which track to read:";
	case DialogKind::Video:
		return "Multiple video tracks detected, please choose the one you wish to load:";
	}

	return {};
}
}

std::string FormatTrackLabel(TrackLabel const& label) {
	std::ostringstream text;
	text << "Track " << std::setw(2) << std::setfill('0') << label.index << ": "
		<< (label.codec.empty() ? "unknown" : label.codec);

	for (auto const& detail : label.details) {
		if (detail.empty())
			continue;
		text << ", " << detail;
	}

	if (!label.title.empty())
		text << ": " << label.title;

	return text.str();
}

agi::SingleChoiceInteractionRequest BuildRequest(DialogKind kind, std::vector<std::string> const& choices) {
	agi::SingleChoiceInteractionRequest request;
	request.title = BuildTitle(kind);
	request.message = BuildMessage(kind);
	request.choices = choices;
	request.request_id = BuildRequestId(kind);
	return request;
}

std::optional<int> ResolveSelection(size_t choice_count, std::optional<int> selection) {
	if (!selection)
		return std::nullopt;
	if (*selection < 0 || static_cast<size_t>(*selection) >= choice_count)
		return std::nullopt;
	return *selection;
}

}
