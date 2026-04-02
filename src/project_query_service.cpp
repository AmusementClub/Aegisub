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
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "project_query_service.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "subs_controller.h"

namespace aegisub::project_query_service {
namespace {

template<typename CoreSession>
ProjectSessionSnapshot QueryProjectSessionImpl(CoreSession const& core) {
	ProjectSessionSnapshot snapshot;
	snapshot.media = playback_query_service::QueryProjectMedia(core);

	if (core.subsController) {
		snapshot.subtitle_file_loaded = core.subsController->HasFile();
		if (snapshot.subtitle_file_loaded)
			snapshot.subtitle_path = core.subsController->Filename();
		snapshot.subtitle_modified = core.subsController->IsModified();
	}

	if (core.project) {
		snapshot.timecodes_path = core.project->TimecodesName();
		snapshot.keyframes_path = core.project->KeyframesName();
		snapshot.timecodes_file_loaded = !snapshot.timecodes_path.empty();
		snapshot.timecodes_loaded = core.project->Timecodes().IsLoaded();
		snapshot.keyframes_file_loaded = !snapshot.keyframes_path.empty();
		snapshot.keyframe_count = core.project->Keyframes().size();
		snapshot.keyframes_loaded = snapshot.keyframe_count != 0;
	}

	if (core.ass) {
		snapshot.title = core.ass->GetScriptInfo("Title");
		snapshot.style_count = core.ass->Styles.size();
		snapshot.event_count = core.ass->Events.size();
		for (auto const& event : core.ass->Events) {
			if (event.Comment)
				++snapshot.comment_count;
			else
				++snapshot.dialogue_count;
		}
	}

	return snapshot;
}

}

ProjectSessionSnapshot QueryProjectSession(agi::ContextCoreSession const& core) {
	return QueryProjectSessionImpl(core);
}

ProjectSessionSnapshot QueryProjectSession(agi::ConstContextCoreSession const& core) {
	return QueryProjectSessionImpl(core);
}

}
