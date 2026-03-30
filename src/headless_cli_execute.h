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

#pragma once

#include "headless_cli_command_model.h"

#include <functional>
#include <string>

namespace headless_cli {

TraceInspectResult RunInspectTrace(TraceInspectRequest const& request);
MediaInspectResult RunInspectMedia(MediaInspectRequest const& request);
AssInfoInspectResult RunInspectAssInfo(AssInfoInspectRequest const& request);
std::string BuildMediaInspectJson(MediaInspectResult const& result);
std::string BuildAssInfoJson(AssInfoInspectResult const& result);
void RunSessionPlaybackAsync(PlaybackSessionRequest request, std::function<void(PlaybackSessionResult)> on_done);
void RunSessionProjectAsync(ProjectSessionRequest request, std::function<void(ProjectSessionResult)> on_done);
void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done);
BatchTraceSummarizeResult RunBatchTraceSummarize(BatchTraceSummarizeRequest const& request);
BatchAssInfoResult RunBatchAssInfo(BatchAssInfoRequest const& request);

}
