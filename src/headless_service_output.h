#pragma once

#include "ass_info_service.h"
#include "media_inspect_service.h"
#include "trace_inspect_service.h"

#include <string>

namespace aegisub::headless_service_output {

std::string BuildTraceInspectJson(trace_inspect_service::TraceSessionSummary const& session);
std::string BuildMediaInspectJson(media_inspect_service::MediaInspectResult const& result);
std::string BuildAssInfoJson(ass_info_service::AssInfoInspectResult const& result);

}
