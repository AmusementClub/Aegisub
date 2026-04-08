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

#include "project.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "audio_provider_factory.h"
#include "charset_detect.h"
#include "compat.h"
#include "dialogs.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/video_provider.h"
#include "mkv_wrap.h"
#include "options.h"
#include "perf_trace.h"
#include "project_session_ops.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "transient_font_set.h"
#include "ui_services.h"
#include "include/aegisub/subtitles_provider.h"
#include "utils.h"
#include "video_memory_stats.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_session_ops.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/access.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/keyframe.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <chrono>
#include <filesystem>

namespace {
bool transient_font_environment_matches(std::shared_ptr<const TransientFontSet> const& left, std::shared_ptr<const TransientFontSet> const& right) {
	if (left == right)
		return true;

	auto const is_empty = [](std::shared_ptr<const TransientFontSet> const& fonts) {
		return !fonts || fonts->empty();
	};
	if (is_empty(left) && is_empty(right))
		return true;
	if (!left || !right)
		return false;
	return left->generation != 0 && left->generation == right->generation;
}

bool try_check_readable_media_path(agi::fs::path const& path, std::string& error) {
	error.clear();
	if (agi::IsNonFilesystemMediaPath(path))
		return true;

	try {
		agi::acs::CheckFileRead(path);
		return true;
	}
	catch (agi::fs::FileSystemError const& err) {
		error = err.GetMessage();
		return false;
	}
}

agi::ProjectUiStateSnapshot BuildSubtitleUiStateSnapshot(ProjectProperties const& properties) {
	agi::ProjectUiStateSnapshot snapshot;
	snapshot.subtitle_scroll_position = properties.scroll_position;
	return snapshot;
}

agi::ProjectUiStateSnapshot BuildVideoUiStateSnapshot(ProjectProperties const& properties) {
	agi::ProjectUiStateSnapshot snapshot;
	snapshot.video_zoom = properties.video_zoom;
	return snapshot;
}

void RestoreProjectUiState(agi::Context *context, agi::ProjectUiStateSnapshot const& state) {
	if (auto sink = context->GetProjectUiStateSink())
		sink->RestoreProjectUiState(state);
}

void ApplyPostOpenVideoPlan(agi::Context *context, aegisub::video_session_ops::PostOpenPlan const& plan) {
	auto video_controller = context->GetCore().videoController.get();
	if (plan.display_aspect_ratio_override)
		video_controller->SetAspectRatio(*plan.display_aspect_ratio_override);
	else
		video_controller->SetAspectRatio(AspectRatio::Default);
	video_controller->JumpToFrame(plan.initial_frame);
}
}

Project::Project(agi::Context *c) : context(c) {
	option_connections = agi::signal::make_vector({
		OPT_SUB("Audio/Cache/Type", &Project::ReloadAudio, this),
		OPT_SUB("Audio/Provider", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Audio/FFmpegSource/Decode Error Handling", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Audio/FFmpegSource/Downmix", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Allow Ancient", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Allow Ancient", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Memory Max", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Memory Max", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Runtime Path", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Runtime Path", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Video/FFmpegSource/Decoding Threads", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Video/FFmpegSource/Unsafe Seeking", &Project::ReloadVideo, this),
		OPT_SUB("Subtitle/Provider", &Project::ReloadSubtitlesProvider, this),
		OPT_SUB("Video/Provider", &Project::ReloadVideo, this),
	});
}

Project::~Project() { }

void Project::UpdateRelativePaths() {
	auto core = context->GetCore();
	core.ass->Properties.audio_file     = agi::fs::PathToGenericString(core.path->MakeRelative(audio_file, "?script"));
	core.ass->Properties.video_file     = agi::fs::PathToGenericString(core.path->MakeRelative(video_file, "?script"));
	core.ass->Properties.timecodes_file = agi::fs::PathToGenericString(core.path->MakeRelative(timecodes_file, "?script"));
	core.ass->Properties.keyframes_file = agi::fs::PathToGenericString(core.path->MakeRelative(keyframes_file, "?script"));
}

void Project::ReloadAudio() {
	if (audio_provider)
		LoadAudio(audio_file);
}

void Project::RefreshSubtitlesProvider(bool recreate_provider) {
	if (!video_provider)
		return;

	auto core = context->GetCore();
	try {
		if (recreate_provider) {
			video_provider->ReplaceSubtitlesProvider(SubtitlesProviderFactory::GetProvider({
				GetProgressRunner(),
				core.ass->GetTransientFonts()
			}));
		}
		video_provider->SetSubtitlesTimecodes(timecodes);
		video_provider->LoadSubtitles(core.ass.get());
		core.videoController->JumpToFrame(core.videoController->GetFrameN());
	}
	catch (agi::UserCancelException const&) {
	}
	catch (std::string const& err) {
		ShowError(err);
	}
	catch (agi::Exception const& err) {
		ShowError(err.GetMessage());
	}
	catch (...) {
		ShowError(std::string("Failed to reload subtitles provider."));
	}
}

void Project::ReloadSubtitlesProvider() {
	RefreshSubtitlesProvider(true);
}

void Project::RefreshVideoFrameForTimecodesChange() {
	if (!video_provider)
		return;

	video_provider->SetSubtitlesTimecodes(timecodes);
	auto core = context->GetCore();
	core.videoController->JumpToFrame(core.videoController->GetFrameN());
}

void Project::ReloadVideo() {
	if (video_provider) {
		DoLoadVideo(video_file);
		auto core = context->GetCore();
		core.videoController->JumpToFrame(core.videoController->GetFrameN());
	}
}

agi::BackgroundRunner *Project::GetProgressRunner(std::string const& title, std::string const& message) {
	if (!progress_runner)
		progress_runner = context->CreateBackgroundRunner(title, message);
	return progress_runner.get();
}

void Project::ShowError(wxString const& message, std::string const& title) {
	context->ShowError(from_wx(message), title);
}

void Project::ShowError(std::string const& message, std::string const& title) {
	context->ShowError(message, title);
}

void Project::ShowWarning(std::string const& message, std::string const& title) {
	context->ShowWarning(message, title);
}

void Project::SetPath(agi::fs::path& var, const char *token, const char *mru, agi::fs::path const& value) {
	auto core = context->GetCore();
	var = value;
	if (*token)
		core.path->SetToken(token, value);
	if (*mru)
		config::mru->Add(mru, value);
	UpdateRelativePaths();
}

bool Project::DoLoadSubtitles(agi::fs::path const& path, std::string encoding, ProjectProperties &properties) {
	auto core = context->GetCore();
	auto const previous_transient_fonts = core.ass->GetTransientFonts();
	auto remove_mru = [](char const* category, agi::fs::path const& candidate) {
		config::mru->Remove(category, candidate);
	};

	auto resolved_encoding = aegisub::project_session_ops::ResolveSubtitleEncoding(
		path,
		std::move(encoding),
		[&] { return CharSetDetect::GetEncoding(path, context->GetSingleChoiceInteractionSink()); },
		*context->GetNotificationSink(),
		remove_mru);
	if (!resolved_encoding)
		return false;
	encoding = *resolved_encoding;

	if (encoding != "binary") {
		// Try loading as timecodes and keyframes first since we can't
		// distinguish them based on filename alone, and just ignore failures
		// rather than trying to differentiate between malformed timecodes
		// files and things that aren't timecodes files at all
		try { DoLoadTimecodes(path); return false; } catch (...) { }
		try { DoLoadKeyframes(path); return false; } catch (...) { }
	}

	if (!aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		path,
		[&] { properties = core.subsController->Load(path, encoding); },
		*context->GetNotificationSink(),
		remove_mru)) {
		return false;
	}

	Selection sel;
	AssDialogue *active_line = nullptr;
	if (!core.ass->Events.empty()) {
		int row = mid<int>(0, properties.active_row, core.ass->Events.size() - 1);
		active_line = &*std::next(core.ass->Events.begin(), row);
		sel.insert(active_line);
	}
	core.selectionController->SetSelectionAndActive(std::move(sel), active_line);
	RestoreProjectUiState(context, BuildSubtitleUiStateSnapshot(properties));

	if (video_provider)
		RefreshSubtitlesProvider(!transient_font_environment_matches(previous_transient_fonts, core.ass->GetTransientFonts()));

	return true;
}

void Project::LoadSubtitles(agi::fs::path path, std::string encoding, bool load_linked) {
	ProjectProperties properties;
	if (DoLoadSubtitles(path, encoding, properties) && load_linked)
		LoadUnloadFiles(properties);
}

void Project::CloseSubtitles() {
	auto core = context->GetCore();
	auto const previous_transient_fonts = core.ass->GetTransientFonts();

	core.subsController->Close();
	core.path->SetToken("?script", "");
	LoadUnloadFiles(core.ass->Properties);
	auto line = &*core.ass->Events.begin();
	core.selectionController->SetSelectionAndActive({line}, line);
	if (video_provider)
		RefreshSubtitlesProvider(!transient_font_environment_matches(previous_transient_fonts, core.ass->GetTransientFonts()));
}

void Project::LoadUnloadFiles(ProjectProperties properties) {
	auto load_linked = OPT_GET("App/Auto/Load Linked Files")->GetInt();
	if (!load_linked) return;

	auto core = context->GetCore();
	auto audio     = core.path->MakeAbsolute(properties.audio_file, "?script");
	auto video     = core.path->MakeAbsolute(properties.video_file, "?script");
	auto timecodes = core.path->MakeAbsolute(properties.timecodes_file, "?script");
	auto keyframes = core.path->MakeAbsolute(properties.keyframes_file, "?script");

	if (video == video_file && audio == audio_file && keyframes == keyframes_file && timecodes == timecodes_file)
		return;

	if (load_linked == 2) {
		std::string message = from_wx(_("Do you want to load/unload the associated files?"));
		message += "\n";

		auto append_file = [&](agi::fs::path const& p, wxString const& unload, wxString const& load) {
			message += "\n";
			if (p.empty())
				message += from_wx(unload);
			else
				message += agi::format(load, p);
		};

		if (audio != audio_file)
			append_file(audio, _("Unload audio"), _("Load audio file: %s"));
		if (video != video_file)
			append_file(video, _("Unload video"), _("Load video file: %s"));
		if (timecodes != timecodes_file)
			append_file(timecodes, _("Unload timecodes"), _("Load timecodes file: %s"));
		if (keyframes != keyframes_file)
			append_file(keyframes, _("Unload keyframes"), _("Load keyframes file: %s"));

		if (context->RequestInteraction({
			from_wx(_("(Un)Load files?")),
			message,
			agi::InteractionButtons::YesNo,
			agi::InteractionIcon::Question
		}) != agi::InteractionResult::Yes)
			return;
	}

	bool loaded_video = false;
	aegisub::video_session_ops::OpenedVideoSummary opened_video_summary;
	bool skip_duplicate_audio_error = false;
	if (video != video_file) {
		if (video.empty())
			CloseVideo();
		else {
			loaded_video = DoLoadVideo(video, &opened_video_summary);
			if (loaded_video) {
				auto vc = core.videoController.get();
				vc->JumpToFrame(properties.video_position);

				auto ar_mode = static_cast<AspectRatio>(properties.ar_mode);
				if (ar_mode == AspectRatio::Custom)
					vc->SetAspectRatio(properties.ar_value);
				else
					vc->SetAspectRatio(ar_mode);
				RestoreProjectUiState(context, BuildVideoUiStateSnapshot(properties));
			}
			else if (audio == video) {
				std::string ignored_error;
				skip_duplicate_audio_error = !try_check_readable_media_path(video, ignored_error);
			}
		}
	}

	if (!timecodes.empty()) LoadTimecodes(timecodes);
	if (!keyframes.empty()) LoadKeyframes(keyframes);

	if (audio != audio_file) {
		if (audio.empty())
			CloseAudio();
		else if (!skip_duplicate_audio_error)
			DoLoadAudio(audio, false);
	}
	else if (loaded_video && aegisub::video_session_ops::PlanPostOpen(
		opened_video_summary,
		OPT_GET("Video/Open Audio")->GetBool(),
		audio_file,
		video_file).auto_load_linked_audio)
		DoLoadAudio(video, true);
}

void Project::DoLoadAudio(agi::fs::path const& path, bool quiet) {
	auto remove_mru = [](char const* category, agi::fs::path const& candidate) {
		config::mru->Remove(category, candidate);
	};

	std::string access_error;
	if (!try_check_readable_media_path(path, access_error)) {
		aegisub::project_session_ops::HandleUnreadableAudioOpenPath(
			path,
			access_error,
			*context->GetNotificationSink(),
			remove_mru);
		return;
	}

	audio_provider = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		path,
		quiet,
		[&]() {
			auto core = context->GetCore();
			return GetAudioProvider(path, *core.path, GetProgressRunner(), *context->GetNotificationSink(), context->GetSingleChoiceInteractionSink());
		},
		*context->GetNotificationSink(),
		[&](std::string const& error) {
			LOG_D("video/open/audio") << "File " << video_file << " has no audio data: " << error;
		},
		remove_mru);
	if (!audio_provider)
		return;

	SetPath(audio_file, "?audio", "Audio", path);
	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		if (video_provider)
			snapshot.async = video_provider->CollectMemoryStats();
		if (auto video_display = context->GetUI().videoDisplay)
			snapshot.display = video_display->CollectMemoryStats();
		snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("audio_open", snapshot, true);
	}
	AnnounceAudioProviderModified(audio_provider.get());
}

void Project::LoadAudio(agi::fs::path path) {
	DoLoadAudio(path, false);
}

void Project::CloseAudio() {
	AnnounceAudioProviderModified(nullptr);
	audio_provider.reset();
	SetPath(audio_file, "?audio", "", "");
}

bool Project::DoLoadVideo(agi::fs::path const& path, aegisub::video_session_ops::OpenedVideoSummary* summary) {
	std::string access_error;
	if (!try_check_readable_media_path(path, access_error)) {
		return aegisub::video_session_ops::HandleUnreadableVideoOpenPath(
			path,
			access_error,
			*context->GetNotificationSink(),
			[](char const* category, agi::fs::path const& candidate) {
				config::mru->Remove(category, candidate);
			});
	}

	auto const load_started = std::chrono::steady_clock::now();
	video_provider = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		path,
		[&] {
			auto core = context->GetCore();
			auto old_matrix = core.ass->GetScriptInfo("YCbCr Matrix");
			auto event_sink = core.videoController->CreateAsyncVideoProviderEventSink();
			return agi::make_unique<AsyncVideoProvider>(
				path,
				old_matrix,
				std::move(event_sink),
				GetProgressRunner(),
				core.ass->GetTransientFonts(),
				context->GetSingleChoiceInteractionSink());
		},
		*context->GetNotificationSink(),
		[](char const* category, agi::fs::path const& candidate) {
			config::mru->Remove(category, candidate);
		});
	if (!video_provider)
		return false;

	auto opened_video = aegisub::video_session_ops::BuildOpenedVideoSummary(
		*video_provider,
		path,
		[](agi::fs::path const& candidate) {
			return MatroskaWrapper::HasSubtitles(candidate);
		});

	timecodes_file.clear();
	keyframes_file.clear();
	// Video-open listeners read Project::VideoName(), so publish the new path first.
	SetPath(video_file, "?video", "Video", path);

	AnnounceVideoProviderModified(video_provider.get());

	auto core = context->GetCore();
	UpdateVideoProperties(context, core.ass.get(), video_provider.get());
	video_provider->LoadSubtitles(core.ass.get());

	timecodes = opened_video.timecodes;
	keyframes = opened_video.keyframes;
	video_provider->SetSubtitlesTimecodes(timecodes);

	std::string warning = opened_video.warning;
	if (!warning.empty())
		ShowWarning(warning, "Warning");

	video_has_subtitles = opened_video.has_subtitles;
	if (summary)
		*summary = opened_video;

	AnnounceKeyframesModified(keyframes);
	AnnounceTimecodesModified(timecodes);
	auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_started).count();
	perf_trace::TraceVideoOpen(path, video_provider->GetWidth(), video_provider->GetHeight(), video_provider->GetFrameCount(), video_provider->HasAudio(), video_provider->GetDecoderName(), duration_ms);
	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		snapshot.async = video_provider->CollectMemoryStats();
		if (audio_provider)
			snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("video_open", snapshot, true);
	}
	return true;
}

void Project::LoadVideo(agi::fs::path path) {
	if (path.empty()) return;
	aegisub::video_session_ops::OpenedVideoSummary opened_video;
	if (!DoLoadVideo(path, &opened_video)) return;
	auto plan = aegisub::video_session_ops::PlanPostOpen(
		opened_video,
		OPT_GET("Video/Open Audio")->GetBool(),
		audio_file,
		video_file);
	if (plan.auto_load_linked_audio)
		DoLoadAudio(video_file, true);
	ApplyPostOpenVideoPlan(context, plan);
}

void Project::CloseVideo() {
	auto core = context->GetCore();
	AnnounceVideoProviderModified(nullptr);
	video_provider.reset();
	SetPath(video_file, "?video", "", "");
	video_has_subtitles = false;
	core.ass->Properties.ar_mode = 0;
	core.ass->Properties.ar_value = 0.0;
	core.ass->Properties.video_position = 0;
}

void Project::DoLoadTimecodes(agi::fs::path const& path) {
	timecodes = agi::vfr::Framerate(path);
	SetPath(timecodes_file, "", "Timecodes", path);
	RefreshVideoFrameForTimecodesChange();
	AnnounceTimecodesModified(timecodes);
}

void Project::LoadTimecodes(agi::fs::path path) {
	try {
		DoLoadTimecodes(path);
	}
	catch (agi::fs::FileSystemError const& e) {
		ShowError(e.GetMessage());
		config::mru->Remove("Timecodes", path);
	}
	catch (agi::vfr::Error const& e) {
		ShowError(agi::format("Failed to parse timecodes file: %s", e.GetMessage()));
		config::mru->Remove("Timecodes", path);
	}
}

void Project::CloseTimecodes() {
	timecodes = video_provider ? video_provider->GetFPS() : agi::vfr::Framerate{};
	SetPath(timecodes_file, "", "", "");
	RefreshVideoFrameForTimecodesChange();
	AnnounceTimecodesModified(timecodes);
}

void Project::DoLoadKeyframes(agi::fs::path const& path) {
	keyframes = agi::keyframe::Load(path);
	SetPath(keyframes_file, "", "Keyframes", path);
	AnnounceKeyframesModified(keyframes);
}

void Project::LoadKeyframes(agi::fs::path path) {
	try {
		DoLoadKeyframes(path);
	}
	catch (agi::fs::FileSystemError const& e) {
		ShowError(e.GetMessage());
		config::mru->Remove("Keyframes", path);
	}
	catch (agi::keyframe::Error const& e) {
		ShowError(agi::format("Failed to parse keyframes file: %s", e.GetMessage()));
		config::mru->Remove("Keyframes", path);
	}
}

void Project::CloseKeyframes() {
	keyframes = video_provider ? video_provider->GetKeyFrames() : std::vector<int>{};
	SetPath(keyframes_file, "", "", "");
	AnnounceKeyframesModified(keyframes);
}

void Project::LoadList(std::vector<agi::fs::path> const& files) {
	// Keep these lists sorted

	// Video formats
	const char *videoList[] = {
		".asf",
		".avi",
		".avs",
		".d2v",
		".h264",
		".hevc",
		".m2ts",
		".m4v",
		".mkv",
		".mov",
		".mp4",
		".mpeg",
		".mpg",
		".ogm",
		".rm",
		".rmvb",
		".ts",
		".webm"
		".wmv",
		".y4m",
		".yuv"
	};

	// Subtitle formats
	const char *subsList[] = {
		".ass",
		".srt",
		".ssa",
		".sub",
		".ttxt"
	};

	// Audio formats
	const char *audioList[] = {
		".aac",
		".ac3",
		".ape",
		".dts",
		".eac3",
		".flac",
		".m4a",
		".mka",
		".mp3",
		".ogg",
		".opus",
		".w64",
		".wav",
		".wma"
	};

	auto search = [](const char **begin, const char **end, std::string const& str) {
		return std::binary_search(begin, end, str.c_str(), [](const char *a, const char *b) {
			return strcmp(a, b) < 0;
		});
	};

	agi::fs::path audio, video, subs, timecodes, keyframes;
	for (auto file : files) {
		if (file.is_relative()) file = absolute(file);
		if (!agi::fs::FileExists(file)) continue;

		auto ext = file.extension().string();
		agi::util::strings::to_lower_inplace(ext);

		// Could be subtitles, keyframes or timecodes, so try loading as each
		if (ext == ".txt" || ext == ".log") {
			if (timecodes.empty()) {
				try {
					DoLoadTimecodes(file);
					timecodes = file;
					continue;
				} catch (...) { }
			}

			if (keyframes.empty()) {
				try {
					DoLoadKeyframes(file);
					keyframes = file;
					continue;
				} catch (...) { }
			}

			if (subs.empty() && ext != ".log")
				subs = file;
			continue;
		}

		if (subs.empty() && search(std::begin(subsList), std::end(subsList), ext))
			subs = file;
		if (video.empty() && search(std::begin(videoList), std::end(videoList), ext))
			video = file;
		if (audio.empty() && search(std::begin(audioList), std::end(audioList), ext))
			audio = file;
	}

	ProjectProperties properties;
	if (!subs.empty()) {
		if (!DoLoadSubtitles(subs, "", properties))
			subs.clear();
	}

	if (!audio.empty())
		DoLoadAudio(audio, false);

	aegisub::video_session_ops::OpenedVideoSummary opened_video;
	if (!video.empty() && DoLoadVideo(video, &opened_video)) {
		auto plan = aegisub::video_session_ops::PlanPostOpen(
			opened_video,
			OPT_GET("Video/Open Audio")->GetBool(),
			audio_file,
			video_file);
		ApplyPostOpenVideoPlan(context, plan);

		// We loaded these earlier, but loading video unloaded them
		// Non-Do version of Load in case they've vanished or changed between
		// then and now
		if (!timecodes.empty())
			LoadTimecodes(timecodes);
		if (!keyframes.empty())
			LoadKeyframes(keyframes);

		// Load audio from video
		if (audio.empty() && plan.auto_load_linked_audio)
			DoLoadAudio(video_file, true);
	}

	if (!subs.empty())
		LoadUnloadFiles(properties);
}
