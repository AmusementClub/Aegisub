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

#include "ass_info_service.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "subtitle_format.h"
#include "ui_services.h"

#include <libaegisub/fs.h>
#include <libaegisub/exception.h>
#include <libaegisub/vfr.h>

#include <memory>

namespace aegisub::ass_info_service {

AssInfoInspectResult Inspect(AssInfoInspectRequest const& request) {
	AssInfoInspectResult result;
	AssFile file;

	auto const* reader = SubtitleFormat::GetReader(request.subtitle_path, request.encoding);
	if (!reader) {
		result.error = "could not resolve subtitle reader for: " + agi::fs::PathToString(request.subtitle_path);
		return result;
	}

	try {
		reader->ReadFile(
			&file,
			request.subtitle_path,
			agi::vfr::Framerate{},
			request.encoding,
			std::make_shared<agi::NullSingleChoiceInteractionSink>());
	}
	catch (std::exception const& error) {
		result.error = error.what();
		return result;
	}
	catch (agi::Exception const& error) {
		result.error = error.GetMessage();
		return result;
	}

	AssInfoSnapshot snapshot;
	snapshot.subtitle_path = request.subtitle_path;
	snapshot.format_name = reader->GetName();
	snapshot.title = file.GetScriptInfo("Title");
	snapshot.script_type = file.GetScriptInfo("ScriptType");
	snapshot.wrap_style = file.GetScriptInfo("WrapStyle");
	snapshot.scaled_border_and_shadow = file.GetScriptInfo("ScaledBorderAndShadow");
	snapshot.play_res_x = file.GetScriptInfoAsInt("PlayResX");
	snapshot.play_res_y = file.GetScriptInfoAsInt("PlayResY");
	snapshot.layout_res_x = file.GetScriptInfoAsInt("LayoutResX");
	snapshot.layout_res_y = file.GetScriptInfoAsInt("LayoutResY");
	snapshot.info_count = file.Info.size();
	snapshot.style_count = file.Styles.size();
	snapshot.event_count = file.Events.size();
	snapshot.attachment_count = file.Attachments.size();
	snapshot.extradata_count = file.Extradata.size();
	snapshot.project_audio_file = file.Properties.audio_file;
	snapshot.project_video_file = file.Properties.video_file;
	snapshot.project_timecodes_file = file.Properties.timecodes_file;
	snapshot.project_keyframes_file = file.Properties.keyframes_file;

	for (auto const& event : file.Events) {
		if (event.Comment)
			++snapshot.comment_count;
		else
			++snapshot.dialogue_count;
	}

	result.snapshot = std::move(snapshot);
	return result;
}

}
