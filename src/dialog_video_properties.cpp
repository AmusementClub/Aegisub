// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "ui_services.h"
#include "video_property_update.h"

#include <algorithm>
#include <wx/intl.h>

namespace {
agi::SingleChoiceInteractionRequest build_resolution_mismatch_request(VideoPropertyUpdateInput const& input, bool aspect_ratio_changed) {
	agi::SingleChoiceInteractionRequest request;
	request.title = from_wx(_("Resolution mismatch"));
	request.message = agi::format(_("The resolution of the loaded video and the resolution specified for the subtitles don't match.\n\nVideo resolution:\t%d x %d\nScript resolution:\t%d x %d\n\nChange subtitles resolution to match video?"),
		input.video_width, input.video_height, input.script_width, input.script_height);
	request.help_page = "Resolution mismatch";
	request.choices.push_back(from_wx(_("Set to video resolution")));
	request.choices.push_back(from_wx(aspect_ratio_changed
		? _("Resample script (stretch to new aspect ratio)")
		: _("Resample script")));
	if (aspect_ratio_changed) {
		request.choices.push_back(from_wx(_("Resample script (add borders)")));
		request.choices.push_back(from_wx(_("Resample script (remove borders)")));
	}

	auto const last_choice = OPT_GET("Video/Last Script Resolution Mismatch Choice")->GetInt();
	request.default_choice = std::clamp(static_cast<int>(last_choice) - 1, 0, static_cast<int>(request.choices.size() - 1));
	return request;
}

VideoPropertyUpdateInput make_input(AssFile *file, const AsyncVideoProvider *new_provider) {
	int sx, sy;
	return {
		new_provider->ShouldSetVideoProperties(),
		file->GetScriptInfo("YCbCr Matrix"),
		new_provider->GetColorSpace(),
		file->GetResolutionType(sx, sy),
		sx,
		sy,
		new_provider->GetWidth(),
		new_provider->GetHeight(),
		static_cast<VideoResolutionMismatchMode>(OPT_GET("Video/Script Resolution Mismatch")->GetInt())
	};
}
}

void UpdateVideoProperties(agi::Context *context, AssFile *file, const AsyncVideoProvider *new_provider) {
	auto input = make_input(file, new_provider);
	auto plan = PlanVideoPropertyUpdate(input);
	if (plan.prompt_for_resolution_mismatch) {
		auto selection = context->RequestSingleChoice(build_resolution_mismatch_request(input, plan.aspect_ratio_changed));
		if (!selection) {
			plan.prompt_for_resolution_mismatch = false;
		}
		else if (auto choice = ParseVideoResolutionMismatchChoice(*selection, plan.aspect_ratio_changed)) {
			OPT_SET("Video/Last Script Resolution Mismatch Choice")->SetInt(*selection + 1);
			plan = ResolveVideoResolutionMismatchChoice(plan, *choice);
		}
		else {
			plan.prompt_for_resolution_mismatch = false;
		}
	}

	ApplyVideoPropertyUpdatePlan(file, input, plan);
	if (plan.ShouldCommit())
		file->Commit(from_wx(_("change script resolution")), AssFile::COMMIT_SCRIPTINFO);
}
