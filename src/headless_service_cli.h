#pragma once

#include "ass_info_service.h"
#include "media_inspect_service.h"
#include "playback_probe_service.h"
#include "trace_inspect_service.h"

#include <variant>

namespace aegisub::headless_service_cli {

struct ProbePlaybackCommand {
	playback_probe_service::PlaybackProbeRequest request;
};

struct InspectMediaCommand {
	media_inspect_service::MediaInspectRequest request;
};

struct InspectAssInfoCommand {
	ass_info_service::AssInfoInspectRequest request;
};

struct InspectTraceCommand {
	trace_inspect_service::TraceInspectRequest request;
};

using Command = std::variant<
	ProbePlaybackCommand,
	InspectMediaCommand,
	InspectAssInfoCommand,
	InspectTraceCommand>;

}
