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
#include "include/aegisub/context.h"
#include "options.h"
#include "translation_service.h"
#include "video_property_update.h"
#include "video_property_update_requests.h"

void UpdateVideoProperties(agi::Context *context, AssFile *file, const AsyncVideoProvider *new_provider) {
	auto input = BuildVideoPropertyUpdateInput(
		file,
		new_provider,
		static_cast<VideoResolutionMismatchMode>(OPT_GET("Video/Script Resolution Mismatch")->GetInt()));
	auto plan = PlanVideoPropertyUpdate(input);
	if (plan.prompt_for_resolution_mismatch) {
		auto const last_choice = OPT_GET("Video/Last Script Resolution Mismatch Choice")->GetInt();
		auto selection = context->RequestSingleChoice(BuildVideoResolutionMismatchRequest(input, plan.aspect_ratio_changed, static_cast<int>(last_choice)));
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

	// If LayoutRes is still missing after handling any resolution mismatch,
	// prompt the user to set it. LayoutRes ensures \blur, \frx/\fry, and
	// borders (when SBAS=no) scale correctly when the script is used with
	// different video resolutions. See libass discussion #734 (astiob).
	if (plan.prompt_for_layout_res
		&& (file->GetScriptInfoAsInt("LayoutResX") <= 0 || file->GetScriptInfoAsInt("LayoutResY") <= 0)) {
		auto selection = context->RequestSingleChoice(BuildLayoutResRequest(input));
		if (selection && *selection == 0) {
			plan.set_layout_res = true;
			ApplyVideoPropertyUpdatePlan(file, input, plan);
		}
	}

	if (plan.ShouldCommit())
		file->Commit(_("change script resolution"), AssFile::COMMIT_SCRIPTINFO);
}
