// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "trace_inspect_service.h"

#include <libaegisub/fs.h>

#include <fstream>
#include <string>

namespace aegisub::trace_inspect_service {
namespace {

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

std::map<std::string, std::string> ReadKeyValueFile(agi::fs::path const& path) {
	std::map<std::string, std::string> values;
	std::ifstream in(path, std::ios::in);
	std::string line;
	while (std::getline(in, line)) {
		auto split = line.find('=');
		if (split == std::string::npos)
			continue;
		values.emplace(line.substr(0, split), line.substr(split + 1));
	}
	return values;
}

agi::fs::path ResolveSessionDirectory(agi::fs::path path) {
	if (agi::fs::DirectoryExists(path))
		return path;

	auto filename = agi::fs::PathToString(path.filename());
	if (filename == "summary.txt" || filename == "manifest.txt" || filename == "trace.ndjson")
		return path.parent_path();
	return {};
}

}

TraceInspectResult Inspect(TraceInspectRequest const& request) {
	TraceInspectResult result;
	auto session_dir = ResolveSessionDirectory(request.input_path);
	if (session_dir.empty()) {
		result.error = "could not resolve trace session directory from: " + ToGenericString(request.input_path);
		return result;
	}

	auto manifest_path = session_dir / "manifest.txt";
	auto summary_path = session_dir / "summary.txt";
	if (!agi::fs::FileExists(manifest_path)) {
		result.error = "trace inspect missing manifest.txt in: " + ToGenericString(session_dir);
		return result;
	}
	if (!agi::fs::FileExists(summary_path)) {
		result.error = "trace inspect missing summary.txt in: " + ToGenericString(session_dir);
		return result;
	}

	result.session = TraceSessionSummary{
		session_dir,
		ReadKeyValueFile(manifest_path),
		ReadKeyValueFile(summary_path),
	};
	return result;
}

}
