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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <string>
#include <vector>

namespace Automation4 {
	class AutomationHost;

	struct AutomationSelectionSnapshot {
		std::vector<int> selected_rows;
		int active_row = 0;
	};

	struct AutomationMediaSnapshot {
		bool has_video = false;
		bool has_audio = false;
		int video_width = 0;
		int video_height = 0;
		double video_aspect_ratio = 0.0;
		int video_aspect_ratio_type = 0;

		bool has_timecodes = false;
		bool has_keyframes = false;
		std::vector<int> keyframes;

		bool has_audio_selection = false;
		int audio_selection_begin = 0;
		int audio_selection_end = 0;
	};

	struct AutomationProjectSnapshot {
		std::string script_filename;
		std::string subtitle_file;
		std::string automation_scripts;
		std::string export_filters;
		std::string export_encoding;
		std::string style_storage;
		std::string audio_file;
		std::string video_file;
		std::string timecodes_file;
		std::string keyframes_file;
		int play_res_x = 0;
		int play_res_y = 0;
		int info_count = 0;
		int style_count = 0;
		int event_count = 0;
		int dialogue_count = 0;
		int comment_count = 0;
		int attachment_count = 0;
		int extradata_count = 0;
		double video_zoom = 0.0;
		double ar_value = 0.0;
		int scroll_position = 0;
		int ar_mode = 0;
		int active_row = 0;
		int video_position = 0;
	};

	struct AutomationContextSnapshot {
		bool has_project_context = false;
		AutomationSelectionSnapshot selection;
		AutomationMediaSnapshot media;
		AutomationProjectSnapshot project;
	};

	AutomationContextSnapshot CaptureAutomationContextSnapshot(
		AutomationHost const* host,
		std::vector<int> selection,
		int active_row);
}
