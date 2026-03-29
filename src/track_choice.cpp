#include "track_choice.h"

#include "compat.h"

#include <wx/intl.h>

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
		return from_wx(_("Choose audio track"));
	case DialogKind::Subtitle:
		return from_wx(_("Multiple subtitle tracks found"));
	case DialogKind::Video:
		return from_wx(_("Choose video track"));
	}

	return {};
}

std::string BuildMessage(DialogKind kind) {
	switch (kind) {
	case DialogKind::Audio:
		return from_wx(_("Multiple audio tracks detected, please choose the one you wish to load:"));
	case DialogKind::Subtitle:
		return from_wx(_("Choose which track to read:"));
	case DialogKind::Video:
		return from_wx(_("Multiple video tracks detected, please choose the one you wish to load:"));
	}

	return {};
}
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
