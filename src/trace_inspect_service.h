#pragma once

#include <libaegisub/fs_fwd.h>

#include <map>
#include <optional>
#include <string>

namespace aegisub::trace_inspect_service {

struct TraceInspectRequest {
	agi::fs::path input_path;
};

struct TraceSessionSummary {
	agi::fs::path session_dir;
	std::map<std::string, std::string> manifest;
	std::map<std::string, std::string> summary;
};

struct TraceInspectResult {
	std::optional<TraceSessionSummary> session;
	std::string error;
};

TraceInspectResult Inspect(TraceInspectRequest const& request);

}
