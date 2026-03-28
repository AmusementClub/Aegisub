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

#include "project_open_service.h"

#include "include/aegisub/context.h"
#include "project.h"

#include <libaegisub/exception.h>

#include <exception>

namespace aegisub::project_open_service {

ProjectOpenResult Open(agi::ContextCoreSession const& core, PlaybackOpenOptions const& options) {
	ProjectOpenResult result;
	if (!core.project) {
		result.error_code = 2;
		result.error = "project context is unavailable";
		return result;
	}

	if (!options.video_path.empty()) {
		try {
			core.project->LoadVideo(options.video_path);
		}
		catch (agi::Exception const& error) {
			result.error_code = 6;
			result.error = error.GetMessage();
			return result;
		}
		catch (std::exception const& error) {
			result.error_code = 6;
			result.error = error.what();
			return result;
		}
		if (!core.project->VideoProvider()) {
			result.error_code = 6;
			result.error = "failed to load video";
			return result;
		}
	}

	if (!options.skip_audio) {
		auto audio_path = options.audio_path.value_or(options.video_path);
		if (!audio_path.empty()) {
			try {
				core.project->LoadAudio(audio_path);
			}
			catch (agi::Exception const& error) {
				result.error_code = 7;
				result.error = error.GetMessage();
				return result;
			}
			catch (std::exception const& error) {
				result.error_code = 7;
				result.error = error.what();
				return result;
			}
			if (!core.project->AudioProvider()) {
				result.error_code = 7;
				result.error = "failed to load audio";
				return result;
			}
		}
	}

	result.media = playback_query_service::QueryProjectMedia(core);
	result.opened = result.media.has_video || result.media.has_audio;
	if (!result.opened) {
		result.error_code = 8;
		result.error = "no project media opened";
	}
	return result;
}

}
