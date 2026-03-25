// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//	 this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//	 this list of conditions and the following disclaimer in the documentation
//	 and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//	 may be used to endorse or promote products derived from this software
//	 without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../async_video_provider.h"
#include "../audio_controller.h"
#include "../audio_timing.h"
#include "../compat.h"
#include "../dialogs.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../subtitle_timing_ops.h"
#include "../video_controller.h"

#include <libaegisub/make_unique.h>

#include <algorithm>

namespace {
using cmd::Command;

struct validate_video_loaded : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !!c->GetCore().project->VideoProvider();
	}
};

struct validate_adjoinable : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		return aegisub::subtitle_timing_ops::IsAdjoinableSelection(
			core.selectionController->GetSortedSelection(),
			core.ass->Events.size());
	}
};

void adjoin_lines(agi::Context *c, bool set_start) {
	auto core = c->GetCore();
	std::vector<AssDialogue *> ordered_events;
	ordered_events.reserve(core.ass->Events.size());
	for (auto& diag : core.ass->Events)
		ordered_events.push_back(&diag);
	if (!aegisub::subtitle_timing_ops::AdjoinSelection(ordered_events, core.selectionController->GetSelectedSet(), set_start))
		return;
	core.ass->Commit(from_wx(_("adjoin")), AssFile::COMMIT_DIAG_TIME);
}

struct time_continuous_end final : public validate_adjoinable {
	CMD_NAME("time/continuous/end")
	STR_MENU("Change &End")
	STR_DISP("Change End")
	STR_HELP("Change end times of lines to the next line's start time")

	void operator()(agi::Context *c) override {
		adjoin_lines(c, false);
	}
};

struct time_continuous_start final : public validate_adjoinable {
	CMD_NAME("time/continuous/start")
	STR_MENU("Change &Start")
	STR_DISP("Change Start")
	STR_HELP("Change start times of lines to the previous line's end time")

	void operator()(agi::Context *c) override {
		adjoin_lines(c, true);
	}
};

struct time_frame_current final : public validate_video_loaded {
	CMD_NAME("time/frame/current")
	CMD_ICON(shift_to_frame)
	STR_MENU("Shift to &Current Frame")
	STR_DISP("Shift to Current Frame")
	STR_HELP("Shift selection so that the active line starts at current frame")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		const auto active_line = core.selectionController->GetActiveLine();

		int target_start = std::max(0, core.videoController->TimeAtFrame(core.videoController->GetFrameN(), agi::vfr::START));
		if (!aegisub::subtitle_timing_ops::ShiftSelectionToStartTime(core.selectionController->GetSelectedSet(), active_line, target_start))
			return;

		core.ass->Commit(from_wx(_("shift to frame")), AssFile::COMMIT_DIAG_TIME);
	}
};

struct time_shift final : public Command {
	CMD_NAME("time/shift")
	CMD_ICON(shift_times_toolbutton)
	STR_MENU("S&hift Times...")
	STR_DISP("Shift Times")
	STR_HELP("Shift subtitles by time or frames")

	void operator()(agi::Context *c) override {
		ShowShiftTimesDialog(c);
	}
};

static void snap_subs_video(agi::Context *c, bool set_start) {
	auto core = c->GetCore();
	int start = core.videoController->TimeAtFrame(core.videoController->GetFrameN(), agi::vfr::START);
	int end = core.videoController->TimeAtFrame(core.videoController->GetFrameN(), agi::vfr::END);
	if (!aegisub::subtitle_timing_ops::SnapSelectionToVideoRange(core.selectionController->GetSelectedSet(), start, end, set_start))
		return;

	core.ass->Commit(from_wx(_("timing")), AssFile::COMMIT_DIAG_TIME);
}

struct time_snap_end_video final : public validate_video_loaded {
	CMD_NAME("time/snap/end_video")
	CMD_ICON(subend_to_video)
	STR_MENU("Snap &End to Video")
	STR_DISP("Snap End to Video")
	STR_HELP("Set end of selected subtitles to current video frame")

	void operator()(agi::Context *c) override {
		snap_subs_video(c, false);
	}
};

struct time_snap_scene final : public validate_video_loaded {
	CMD_NAME("time/snap/scene")
	CMD_ICON(snap_subs_to_scene)
	STR_MENU("Snap to S&cene")
	STR_DISP("Snap to Scene")
	STR_HELP("Set start and end of subtitles to the keyframes around current video frame")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		VideoController *con = core.videoController.get();
		auto range = aegisub::subtitle_timing_ops::ComputeSceneSnapFrameRange(
			core.project->Keyframes(),
			con->GetFrameN(),
			core.project->VideoProvider()->GetFrameCount());
		if (!range) return;

		int start_ms = con->TimeAtFrame(range->start_frame, agi::vfr::START);
		int end_ms = con->TimeAtFrame(range->one_past_end_frame - 1, agi::vfr::END);
		if (!aegisub::subtitle_timing_ops::ApplyTimeRangeToSelection(core.selectionController->GetSelectedSet(), start_ms, end_ms))
			return;

		core.ass->Commit(from_wx(_("snap to scene")), AssFile::COMMIT_DIAG_TIME);
	}
};

struct time_align_subtitle_to_point final : public validate_video_loaded {
	CMD_NAME("time/align")
	CMD_ICON(button_align)
	STR_MENU("Align subtitle to video")
	STR_DISP("Align subtitle to video")
	STR_HELP("Align subtitle to video by key points")
	void operator()(agi::Context* c) override {
		c->GetCore().videoController->Stop();
		ShowAlignToVideoDialog(c);
	}
};

struct time_add_lead_both final : public Command {
	CMD_NAME("time/lead/both")
	STR_MENU("Add lead in and out")
	STR_DISP("Add lead in and out")
	STR_HELP("Add both lead in and out to the selected lines")
	void operator()(agi::Context *c) override {
		if (AudioTimingController *tc = c->GetCore().audioController->GetTimingController()) {
			tc->AddLeadIn();
			tc->AddLeadOut();
		}
	}
};

struct time_add_lead_in final : public Command {
	CMD_NAME("time/lead/in")
	CMD_ICON(button_leadin)
	STR_MENU("Add lead in")
	STR_DISP("Add lead in")
	STR_HELP("Add the lead in time to the selected lines")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->AddLeadIn();
	}
};

struct time_add_lead_out final : public Command {
	CMD_NAME("time/lead/out")
	CMD_ICON(button_leadout)
	STR_MENU("Add lead out")
	STR_DISP("Add lead out")
	STR_HELP("Add the lead out time to the selected lines")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->AddLeadOut();
	}
};

struct time_length_increase final : public Command {
	CMD_NAME("time/length/increase")
	STR_MENU("Increase length")
	STR_DISP("Increase length")
	STR_HELP("Increase the length of the current timing unit")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyLength(1, false);
	}
};

struct time_length_increase_shift final : public Command {
	CMD_NAME("time/length/increase/shift")
	STR_MENU("Increase length and shift")
	STR_DISP("Increase length and shift")
	STR_HELP("Increase the length of the current timing unit and shift the following items")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyLength(1, true);
	}
};

struct time_length_decrease final : public Command {
	CMD_NAME("time/length/decrease")
	STR_MENU("Decrease length")
	STR_DISP("Decrease length")
	STR_HELP("Decrease the length of the current timing unit")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyLength(-1, false);
	}
};

struct time_length_decrease_shift final : public Command {
	CMD_NAME("time/length/decrease/shift")
	STR_MENU("Decrease length and shift")
	STR_DISP("Decrease length and shift")
	STR_HELP("Decrease the length of the current timing unit and shift the following items")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyLength(-1, true);
	}
};

struct time_start_increase final : public Command {
	CMD_NAME("time/start/increase")
	STR_MENU("Shift start time forward")
	STR_DISP("Shift start time forward")
	STR_HELP("Shift the start time of the current timing unit forward")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyStart(1);
	}
};

struct time_start_decrease final : public Command {
	CMD_NAME("time/start/decrease")
	STR_MENU("Shift start time backward")
	STR_DISP("Shift start time backward")
	STR_HELP("Shift the start time of the current timing unit backward")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->ModifyStart(-1);
	}
};

struct time_snap_start_video final : public validate_video_loaded {
	CMD_NAME("time/snap/start_video")
	CMD_ICON(substart_to_video)
	STR_MENU("Snap &Start to Video")
	STR_DISP("Snap Start to Video")
	STR_HELP("Set start of selected subtitles to current video frame")

	void operator()(agi::Context *c) override {
		snap_subs_video(c, true);
	}
};

struct time_next final : public Command {
	CMD_NAME("time/next")
	CMD_ICON(button_next)
	STR_MENU("Next Line")
	STR_DISP("Next Line")
	STR_HELP("Next line or syllable")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->Next(AudioTimingController::TIMING_UNIT);
	}
};

struct time_prev final : public Command {
	CMD_NAME("time/prev")
	CMD_ICON(button_prev)
	STR_MENU("Previous Line")
	STR_DISP("Previous Line")
	STR_HELP("Previous line or syllable")
	void operator()(agi::Context *c) override {
		if (auto *tc = c->GetCore().audioController->GetTimingController())
			tc->Prev();
	}
};
}

namespace cmd {
	void init_time() {
		reg(agi::make_unique<time_add_lead_both>());
		reg(agi::make_unique<time_add_lead_in>());
		reg(agi::make_unique<time_add_lead_out>());
		reg(agi::make_unique<time_continuous_end>());
		reg(agi::make_unique<time_continuous_start>());
		reg(agi::make_unique<time_frame_current>());
		reg(agi::make_unique<time_length_decrease>());
		reg(agi::make_unique<time_length_decrease_shift>());
		reg(agi::make_unique<time_length_increase>());
		reg(agi::make_unique<time_length_increase_shift>());
		reg(agi::make_unique<time_next>());
		reg(agi::make_unique<time_prev>());
		reg(agi::make_unique<time_shift>());
		reg(agi::make_unique<time_snap_end_video>());
		reg(agi::make_unique<time_snap_scene>());
		reg(agi::make_unique<time_snap_start_video>());
		reg(agi::make_unique<time_align_subtitle_to_point>());
		reg(agi::make_unique<time_start_decrease>());
		reg(agi::make_unique<time_start_increase>());
	}
}
