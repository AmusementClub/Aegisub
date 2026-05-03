#include "../../src/video_subtitle_scene_cache.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int kAssMaxMs = std::numeric_limits<int>::max() / 10 * 10;

int ClampNonnegativeMs(int ms) {
	return std::max(0, ms);
}

int ClampAssCs(long long ms) {
	return static_cast<int>(std::clamp(ms, 0LL, static_cast<long long>(kAssMaxMs)));
}

int FloorCs(long long ms) {
	ms = std::max(0LL, ms);
	return ClampAssCs(ms / 10 * 10);
}

int CeilCs(long long ms) {
	if (ms <= 0)
		return 0;
	if (ms >= kAssMaxMs)
		return kAssMaxMs;
	return ClampAssCs(((ms + 9) / 10) * 10);
}

int RoundCs(long long ms) {
	ms = std::max(0LL, ms);
	return ClampAssCs((ms + 5) / 10 * 10);
}

agi::vfr::Time ToVfrMode(AssStorageTimeBoundary boundary) {
	return boundary == AssStorageTimeBoundary::Start ? agi::vfr::START : agi::vfr::END;
}

bool CandidatePreservesFrame(
	int candidate_ms,
	int original_ms,
	AssStorageTimeBoundary boundary,
	agi::vfr::Framerate const& fps) {
	auto const mode = ToVfrMode(boundary);
	return fps.FrameAtTime(candidate_ms, mode) == fps.FrameAtTime(original_ms, mode);
}

int PreferFrameSafeCandidate(
	int original_ms,
	AssStorageTimeBoundary boundary,
	agi::vfr::Framerate const& fps) {
	std::array<int, 2> candidates = { FloorCs(original_ms), CeilCs(original_ms) };
	std::array<bool, 2> valid = {
		CandidatePreservesFrame(candidates[0], original_ms, boundary, fps),
		CandidatePreservesFrame(candidates[1], original_ms, boundary, fps)
	};

	if (!valid[0] && !valid[1])
		return std::numeric_limits<int>::min();
	if (valid[0] && !valid[1])
		return candidates[0];
	if (!valid[0] && valid[1])
		return candidates[1];

	return RoundCs(original_ms);
}

int ProjectShortIntervalToSingleAssBucket(int start_ms, int end_ms) {
	long long midpoint_ms = static_cast<long long>(start_ms) + (static_cast<long long>(end_ms) - start_ms) / 2;
	int projected_start = FloorCs(midpoint_ms);
	if (projected_start >= kAssMaxMs)
		projected_start = std::max(0, kAssMaxMs - 10);
	return projected_start;
}

int ProjectAssTimeForStorageSmoke(
	int time_ms,
	AssStorageTimeBoundary boundary,
	agi::vfr::Framerate const* fps) {
	time_ms = ClampNonnegativeMs(time_ms);

	if (fps && fps->IsLoaded()) {
		int const projected = PreferFrameSafeCandidate(time_ms, boundary, *fps);
		if (projected != std::numeric_limits<int>::min())
			return projected;
	}

	return RoundCs(time_ms);
}

std::pair<int, int> ProjectAssDialogueTimesForStorageSmoke(
	agi::Time const& start,
	agi::Time const& end,
	agi::vfr::Framerate const* fps) {
	int const start_ms = start.GetMillisecond();
	int const end_ms = end.GetMillisecond();

	int projected_start = ProjectAssTimeForStorageSmoke(start_ms, AssStorageTimeBoundary::Start, fps);
	int projected_end = ProjectAssTimeForStorageSmoke(end_ms, AssStorageTimeBoundary::End, fps);

	if (end_ms > start_ms && projected_end <= projected_start) {
		projected_start = ProjectShortIntervalToSingleAssBucket(start_ms, end_ms);
		projected_end = std::min(kAssMaxMs, projected_start + 10);
	}

	if (projected_end < projected_start)
		projected_end = projected_start;

	return { projected_start, projected_end };
}

AssDialogueBase MakeLine(
	int start_ms,
	int end_ms,
	std::string text,
	std::string effect = {},
	bool comment = false) {
	AssDialogueBase line;
	line.Start = start_ms;
	line.End = end_ms;
	line.Text = std::move(text);
	line.Effect = std::move(effect);
	line.Comment = comment;
	return line;
}

struct FakeSceneCache {
	bool valid = false;
	int cached_scene = 0;
	int backend_render_count = 0;
	bool playing = false;

	void Invalidate() noexcept {
		valid = false;
		cached_scene = 0;
	}

	int Redraw(bool allow_scene_cache) {
		bool const can_use_scene_cache = allow_scene_cache && !playing;
		if (can_use_scene_cache && valid)
			return cached_scene;

		int const rendered_scene = ++backend_render_count;
		if (can_use_scene_cache) {
			valid = true;
			cached_scene = rendered_scene;
		}
		else {
			valid = false;
		}
		return rendered_scene;
	}
};

bool ReportScenarioResult(char const* name, bool passed) {
	std::cout << name << "=" << (passed ? "ok" : "fail") << std::endl;
	return passed;
}

bool RunVisibleTextCommitScenario() {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "old")
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	FakeSceneCache cache;
	int const initial = cache.Redraw(true);

	lines[0].Text = "new";
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TEXT,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);

	cache.Invalidate();
	int const redraw_before_packet_1 = cache.Redraw(!wait);
	int const redraw_before_packet_2 = cache.Redraw(!wait);

	bool const passed =
		wait
		&& initial == 1
		&& redraw_before_packet_1 == 2
		&& redraw_before_packet_2 == 3;
	return ReportScenarioResult("visible_text_commit_waits", passed);
}

bool RunInvisibleCommitScenario() {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "visible"),
		MakeLine(3000, 4000, "hidden")
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[1].Text = "hidden changed";
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TEXT,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);

	FakeSceneCache cache;
	cache.Redraw(true);
	cache.Invalidate();
	int const redraw_before_packet_1 = cache.Redraw(!wait);
	int const redraw_before_packet_2 = cache.Redraw(!wait);

	bool const passed =
		!wait
		&& redraw_before_packet_1 == 2
		&& redraw_before_packet_2 == 2;
	return ReportScenarioResult("invisible_commit_reuses_cache", passed);
}

bool RunStyleReloadScenario() {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "same text")
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_STYLES,
		true,
		false,
		lines,
		fps,
		1500,
		snapshot);
	return ReportScenarioResult("style_reload_waits", wait);
}

bool RunStaticTimingChangeScenario() {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "plain text")
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[0].Start = 1100;
	lines[0].End = 2100;
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TIME,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);
	return ReportScenarioResult("static_timing_change_skips_wait", !wait);
}

bool RunPlaybackBypassesSceneCacheScenario() {
	FakeSceneCache cache;
	int const paused_initial = cache.Redraw(true);
	int const paused_cached = cache.Redraw(true);

	cache.playing = true;
	int const playing_first = cache.Redraw(true);
	int const playing_second = cache.Redraw(true);

	cache.playing = false;
	int const paused_after_playback = cache.Redraw(true);
	int const paused_after_cached = cache.Redraw(true);

	bool const passed =
		paused_initial == 1
		&& paused_cached == 1
		&& playing_first == 2
		&& playing_second == 3
		&& paused_after_playback == 4
		&& paused_after_cached == 4;
	return ReportScenarioResult("playback_bypasses_scene_cache", passed);
}

bool RunAnimatedTimingChangeScenario() {
	auto const fps = agi::vfr::Framerate(100.0);
	std::vector<AssDialogueBase> lines = {
		MakeLine(1000, 2000, "{\\move(0,0,100,100)}animated")
	};
	auto const snapshot = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(lines, fps, 1500);

	lines[0].Start = 1100;
	lines[0].End = 2100;
	bool const wait = video_subtitle_scene_cache::ShouldWaitForFreshPacket(
		AssFile::COMMIT_DIAG_TIME,
		true,
		true,
		lines,
		fps,
		1500,
		snapshot);
	return ReportScenarioResult("animated_timing_change_waits", wait);
}

} // namespace

bool IsAssDialogueVisibleAtTimeForStorage(
	agi::Time const& start,
	agi::Time const& end,
	int time_ms,
	agi::vfr::Framerate const* fps) {
	auto const projected = ProjectAssDialogueTimesForStorageSmoke(start, end, fps);
	return !(projected.first > time_ms || projected.second <= time_ms);
}

int main() try {
	bool passed = true;
	passed = RunVisibleTextCommitScenario() && passed;
	passed = RunInvisibleCommitScenario() && passed;
	passed = RunStyleReloadScenario() && passed;
	passed = RunStaticTimingChangeScenario() && passed;
	passed = RunPlaybackBypassesSceneCacheScenario() && passed;
	passed = RunAnimatedTimingChangeScenario() && passed;
	return passed ? 0 : 1;
}
catch (std::exception const& err) {
	std::cerr << "scene-cache-subtitle-commit-smoke failed: " << err.what() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "scene-cache-subtitle-commit-smoke failed: unknown exception" << std::endl;
	return 2;
}
