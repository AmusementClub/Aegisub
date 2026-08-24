// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "secondary_subtitle_session.h"

#include "async_video_provider.h"
#include "async_video_provider_host.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "charset_detect.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/subtitles_provider.h"
#include "mkv_wrap.h"
#include "options.h"
#include "project.h"
#include "pgs_sup_packet_stream.h"
#include "secondary_subtitle_decoder.h"
#include "secondary_subtitle_reload_policy.h"
#include "vobsub_packet_stream.h"
#include "subtitle_fps_choice.h"
#include "subs_controller.h"
#include "subtitle_format.h"
#include "track_choice.h"
#include "ui_services.h"
#include "video_controller.h"
#include "video_frame_wx.h"
#include "video_provider_dummy.h"
#include "video_subtitle_update_policy.h"
#include "watched_file.h"

#include <libaegisub/color.h>
#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/msgdlg.h>

namespace {
constexpr char const *kSecondarySubtitleWarningTitle = "Secondary subtitles";

constexpr char const *kSubtitleFpsChoiceRequestId = "subtitle_fps_choice.selection";

bool HasDecoderForBitmapTrack(MkvTrackInfo const& track) {
	if (!IsDecodableMkvBitmapSubtitleTrack(track))
		return false;
	auto const codec_id = GetSecondarySubtitleCodecId(track.bitmap_subtitle_codec);
	return !codec_id.empty() && secondary_subtitle_decoder::IsAvailable(std::string(codec_id));
}

bool HasDecoderForAvailableBitmapTrack(Project const& project) {
	return (project.CanLoadBitmapSubtitlesFromVideo(MkvBitmapSubtitleCodec::HdmvPgs)
			&& secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecHdmvPgs))
		|| (project.CanLoadBitmapSubtitlesFromVideo(MkvBitmapSubtitleCodec::VobSub)
			&& secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecDvdSubtitle));
}

void FillMissingBitmapCanvasFromVideo(
	SecondarySubtitlePacketStream& stream,
	AsyncVideoProvider *video_provider) {
	if (!video_provider
		|| (stream.fallback_canvas_width > 0 && stream.fallback_canvas_height > 0))
		return;
	stream.fallback_canvas_width = video_provider->GetWidth();
	stream.fallback_canvas_height = video_provider->GetHeight();
}

bool SameFixedFramerate(agi::vfr::Framerate const& fps, SecondarySubtitleFpsSelection const& selection) {
	if (selection.follow_video || !fps.IsLoaded() || fps.IsVFR())
		return false;
	auto const [numerator, denominator] = fps.FPSFraction();
	return numerator == selection.numerator
		&& denominator == selection.denominator
		&& fps.NeedsDropFrames() == selection.drop;
}

class SecondarySubtitleChoiceSink final : public agi::SingleChoiceInteractionSink {
	std::shared_ptr<agi::SingleChoiceInteractionSink> delegate;
	agi::vfr::Framerate const& video_fps;
	std::optional<SecondarySubtitleFpsSelection> existing_selection;
	std::optional<SecondarySubtitleFpsSelection> recorded_selection;

	std::optional<int> FindExistingSelection(SubtitleFpsChoiceModel const& model) const {
		if (!existing_selection)
			return std::nullopt;
		if (existing_selection->follow_video)
			return model.includes_video_choice ? std::optional<int>(0) : std::nullopt;

		for (int i = 0; i < static_cast<int>(model.choices.size()); ++i) {
			auto resolved = ResolveSubtitleFpsChoiceSelection(model, i, video_fps);
			if (SameFixedFramerate(resolved, *existing_selection))
				return i;
		}
		return std::nullopt;
	}

	void RecordSelection(SubtitleFpsChoiceModel const& model, int selection) {
		if (model.includes_video_choice && selection == 0) {
			recorded_selection = SecondarySubtitleFpsSelection{true, 0, 1, false};
			return;
		}

		auto resolved = ResolveSubtitleFpsChoiceSelection(model, selection, video_fps);
		auto const [numerator, denominator] = resolved.FPSFraction();
		recorded_selection = SecondarySubtitleFpsSelection{
			false,
			numerator,
			denominator,
			resolved.NeedsDropFrames()
		};
	}

public:
	SecondarySubtitleChoiceSink(
		std::shared_ptr<agi::SingleChoiceInteractionSink> delegate,
		agi::vfr::Framerate const& video_fps,
		std::optional<SecondarySubtitleFpsSelection> existing_selection)
	: delegate(std::move(delegate))
	, video_fps(video_fps)
	, existing_selection(std::move(existing_selection)) { }

	std::optional<int> RequestSingleChoice(agi::SingleChoiceInteractionRequest const& request) override {
		if (request.request_id != kSubtitleFpsChoiceRequestId) {
			return delegate
				? delegate->RequestSingleChoice(request)
				: std::nullopt;
		}

		SubtitleFpsChoiceModel model;
		model.choices = request.choices;
		// The MicroDVD reader calls AskForFPS(true, false, ...), so the video
		// choice is present for both CFR and VFR timecodes.
		model.includes_video_choice = video_fps.IsLoaded();
		if (auto selection = FindExistingSelection(model)) {
			recorded_selection = existing_selection;
			return *selection;
		}

		auto selection = delegate
			? delegate->RequestSingleChoice(request)
			: std::nullopt;
		if (selection && *selection >= 0 && *selection < static_cast<int>(model.choices.size()))
			RecordSelection(model, *selection);
		return selection;
	}

	std::optional<SecondarySubtitleFpsSelection> const& GetRecordedSelection() const {
		return recorded_selection;
	}
};
}

SecondarySubtitleSession::SecondarySubtitleSession(agi::Context *context)
: context(context)
, external_subtitle_watch(agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend()))
, external_subtitle_companion_watch(agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend()))
, external_subtitle_alternate_companion_watch(agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend()))
, external_style_catalog_watch(agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend())) {
	auto core = context->GetCore();
	auto ui = context->GetUI();
	ui_activation.AddConnections(
		core.project->AddVideoProviderListener(&SecondarySubtitleSession::OnVideoProviderChanged, this),
		core.project->AddTimecodesListener(&SecondarySubtitleSession::OnTimecodesChanged, this),
		core.ass->AddCommitDetailsListener(&SecondarySubtitleSession::OnAssCommit, this),
		core.subsController->AddFileOpenListener([this](agi::fs::path const& filename, bool is_reload) { OnMainSubtitlesFileChanged(filename, is_reload); }),
		core.subsController->AddUpdatePropertiesListener(&SecondarySubtitleSession::OnUpdateProperties, this),
		core.videoController->AddFramePresentedListener(&SecondarySubtitleSession::OnPrimaryFramePresented, this),
		OPT_SUB("Colour/Secondary Subtitle Strip/Dummy Background", &SecondarySubtitleSession::OnDummyBackgroundColorChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Dummy/Pattern", &SecondarySubtitleSession::OnDummyBackgroundPatternChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Provider", &SecondarySubtitleSession::OnConfiguredProviderChanged, this),
		OPT_SUB("Subtitle Format/SRT/Default Style Catalog", &SecondarySubtitleSession::OnSrtStyleCatalogChanged, this),
		OPT_SUB("Subtitle/Provider", &SecondarySubtitleSession::OnGlobalProviderChanged, this));
	external_subtitle_watch->SetChangedCallback([this](agi::fs::path const& path) {
		OnExternalSubtitleFileChanged(path);
	});
	external_subtitle_watch->SetErrorCallback([this](std::string const& message) {
		OnExternalSubtitleWatchError(message);
	});
	external_subtitle_companion_watch->SetChangedCallback([this](agi::fs::path const& path) {
		OnExternalSubtitleFileChanged(path);
	});
	external_subtitle_companion_watch->SetErrorCallback([this](std::string const& message) {
		OnExternalSubtitleWatchError(message);
	});
	external_subtitle_alternate_companion_watch->SetChangedCallback([this](agi::fs::path const& path) {
		OnExternalSubtitleFileChanged(path);
	});
	external_subtitle_alternate_companion_watch->SetErrorCallback([this](std::string const& message) {
		OnExternalSubtitleWatchError(message);
	});
	external_style_catalog_watch->SetChangedCallback([this](agi::fs::path const& path) {
		OnExternalStyleCatalogFileChanged(path);
	});
	external_style_catalog_watch->SetErrorCallback([this](std::string const& message) {
		OnExternalSubtitleWatchError(message);
	});
	RestoreSourceFromProjectProperties();
}

SecondarySubtitleSession::~SecondarySubtitleSession() {
	external_style_catalog_watch.reset();
	external_subtitle_alternate_companion_watch.reset();
	external_subtitle_companion_watch.reset();
	external_subtitle_watch.reset();
	ui_activation.Deactivate();
	ReleaseProvider();
}

void SecondarySubtitleSession::NotifyBitmapUpdated() {
	++bitmap_generation;
	bitmap_updated();
}

void SecondarySubtitleSession::ClearBitmap() {
	current_bitmap = wxBitmap();
	has_bitmap = false;
	current_frame = -1;
	NotifyBitmapUpdated();
}

void SecondarySubtitleSession::ClearExternalSubtitles() {
	external_subtitles.reset();
	bitmap_subtitles.reset();
	loaded_external_subtitle_path.clear();
	external_subtitles_follow_video_resolution = false;
	external_subtitle_reload_pending = false;
	external_subtitle_fps_selection.reset();
	external_subtitles_follow_video_timecodes = false;
	external_subtitles_are_srt = false;
	external_vobsub_track_index.reset();
}

void SecondarySubtitleSession::ReleaseProvider() {
	provider.reset();
	background_runner.reset();
	provider_lifetime.reset();
}

void SecondarySubtitleSession::OnDummyBackgroundColorChanged(agi::OptionValue const&) {
	if (!active) {
		ReleaseProvider();
		return;
	}

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnDummyBackgroundPatternChanged(agi::OptionValue const&) {
	if (!active) {
		ReleaseProvider();
		return;
	}

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnConfiguredProviderChanged(agi::OptionValue const&) {
	if (bitmap_subtitles)
		return;
	if (!active) {
		ReleaseProvider();
		return;
	}

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnGlobalProviderChanged(agi::OptionValue const&) {
	if (!IsFollowingGlobalSubtitlesProvider() || bitmap_subtitles)
		return;
	if (!active) {
		ReleaseProvider();
		return;
	}

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnSrtStyleCatalogChanged(agi::OptionValue const&) {
	UpdateExternalStyleCatalogWatch();
	if (source_mode == SecondarySubtitleSourceMode::ExternalFile && external_subtitles_are_srt)
		ReloadExternalSubtitlesAfterChange();
}

void SecondarySubtitleSession::OnMainSubtitlesFileChanged(agi::fs::path const&, bool is_reload) {
	// A reload of the primary subtitle file must not disturb the secondary
	// subtitle source. ExternalFile / VideoEmbedded keep their in-memory data
	// and source mode; CurrentScript refreshes itself via OnAssCommit, which
	// runs from the COMMIT_NEW fired by SubsController::Load. Only a freshly
	// opened (different) file re-establishes the source from its properties.
	if (is_reload) {
		if (active)
			RequestFrame(context->GetCore().videoController->GetFrameN());
		return;
	}

	RestoreSourceFromProjectProperties();

	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	else
		ReleaseProvider();
}

void SecondarySubtitleSession::OnUpdateProperties() {
	SyncExternalSubtitleProjectProperty();
}

void SecondarySubtitleSession::RestoreSourceFromProjectProperties() {
	auto core = context->GetCore();
	source_mode = SecondarySubtitleSourceMode::CurrentScript;
	current_source_index = static_cast<size_t>(-1);
	external_subtitle_path.clear();
	ClearExternalSubtitles();

	auto const& stored_path = core.ass->Properties.secondary_subtitles_file;
	if (stored_path.empty()) {
		UpdateExternalSubtitleWatch();
		return;
	}

	source_mode = SecondarySubtitleSourceMode::ExternalFile;
	auto const absolute_path = core.path->MakeAbsolute(stored_path, "?script");
	external_subtitle_path = agi::fs::PathToString(absolute_path);
	RegisterExternalSource(absolute_path);
	UpdateExternalSubtitleWatch();
}

void SecondarySubtitleSession::SyncExternalSubtitleProjectProperty() {
	auto core = context->GetCore();
	if (source_mode != SecondarySubtitleSourceMode::ExternalFile || external_subtitle_path.empty()) {
		core.ass->Properties.secondary_subtitles_file.clear();
		return;
	}

	auto absolute_path = agi::fs::PathFromString(external_subtitle_path);
	core.ass->Properties.secondary_subtitles_file =
		agi::fs::PathToGenericString(core.path->MakeRelative(absolute_path, "?script"));
}

void SecondarySubtitleSession::RequestFrame(int frame_number) {
	if (!active || !presentation_demand.HasDemand() || !provider || frame_number < 0)
		return;

	current_frame = frame_number;
	auto core = context->GetCore();
	provider->RequestFrame(frame_number, core.project->Timecodes().TimeAtFrame(frame_number));
}

void SecondarySubtitleSession::UpdateExternalSubtitleWatch() {
	UpdateExternalStyleCatalogWatch();
	if (!external_subtitle_watch)
		return;

	if (source_mode != SecondarySubtitleSourceMode::ExternalFile || external_subtitle_path.empty()) {
		external_subtitle_watch->ClearTargetPath();
		if (external_subtitle_companion_watch)
			external_subtitle_companion_watch->ClearTargetPath();
		if (external_subtitle_alternate_companion_watch)
			external_subtitle_alternate_companion_watch->ClearTargetPath();
		return;
	}

	auto const path = agi::fs::PathFromString(external_subtitle_path);
	external_subtitle_watch->SetTargetPath(path);
	if (!external_subtitle_companion_watch)
		return;
	if (!IsVobSubIndexPath(path)) {
		external_subtitle_companion_watch->ClearTargetPath();
		if (external_subtitle_alternate_companion_watch)
			external_subtitle_alternate_companion_watch->ClearTargetPath();
		return;
	}

	// Watch candidates even when they do not exist yet so creating or replacing
	// either conventional spelling reloads the pair on case-sensitive systems.
	auto lower_case = GetVobSubCompanionPath(path);
	external_subtitle_companion_watch->SetTargetPath(lower_case);
#ifdef _WIN32
	if (external_subtitle_alternate_companion_watch)
		external_subtitle_alternate_companion_watch->ClearTargetPath();
#else
	if (external_subtitle_alternate_companion_watch) {
		auto upper_case = GetVobSubCompanionPath(path, true);
		external_subtitle_alternate_companion_watch->SetTargetPath(upper_case);
	}
#endif
}

void SecondarySubtitleSession::UpdateExternalStyleCatalogWatch() {
	if (!external_style_catalog_watch)
		return;

	auto const path = ResolveSecondarySubtitleStyleCatalogWatchPath(
		source_mode == SecondarySubtitleSourceMode::ExternalFile,
		external_subtitles_are_srt,
		GetSubtitleFormatDefaultStyleCatalog("SRT"));
	if (path.empty()) {
		external_style_catalog_watch->ClearTargetPath();
		return;
	}

	// The catalog directory is only created when a catalog is first saved from
	// the style manager; until then there is nothing to watch and attempting it
	// just reports an error.
	if (!agi::fs::DirectoryExists(path.parent_path())) {
		external_style_catalog_watch->ClearTargetPath();
		return;
	}

	external_style_catalog_watch->SetTargetPath(path);
}

AssFile *SecondarySubtitleSession::ResolveSubtitlesForProvider(AsyncVideoProvider *main_provider) {
	auto core = context->GetCore();
	if (source_mode == SecondarySubtitleSourceMode::CurrentScript)
		return core.ass.get();
	if (source_mode == SecondarySubtitleSourceMode::VideoEmbedded) {
		// Already loaded into memory by LoadVideoEmbeddedSubtitles; never
		// re-reads from disk and does not consult external_subtitle_path.
		UpdateExternalSubtitleResolution(main_provider);
		return external_subtitles.get();
	}
	if (bitmap_subtitles)
		return nullptr;

	if (LoadConfiguredExternalSubtitles(false)) {
		UpdateExternalSubtitleResolution(main_provider);
		return external_subtitles.get();
	}

	return nullptr;
}

void SecondarySubtitleSession::SyncConfiguredSubtitlesSource(AsyncVideoProvider *main_provider) {
	if (!provider)
		return;

	auto core = context->GetCore();
	provider->SetSubtitlesTimecodes(core.project->Timecodes());
	if (auto *subtitles = ResolveSubtitlesForProvider(main_provider ? main_provider : core.project->VideoProvider()))
		provider->LoadSubtitles(subtitles);
	else {
		AssFile empty_subtitles;
		provider->LoadSubtitles(&empty_subtitles);
		// Bitmap decoders consume packet streams rather than AssFile data.
		// Only clear the bitmap for ASS-based providers that load
		// their data through this call.
		if (!bitmap_subtitles)
			ClearBitmap();
	}
}

bool SecondarySubtitleSession::LoadConfiguredExternalSubtitles(bool show_errors, bool force_reload) {
	auto path_string = external_subtitle_path;
	if (path_string.empty())
		return false;
	if (!force_reload && (external_subtitles || bitmap_subtitles) && loaded_external_subtitle_path == path_string)
		return true;

	return LoadExternalSubtitlesFromPath(path_string, show_errors);
}

void SecondarySubtitleSession::RefreshProviderAfterExternalReload() {
	auto const action = PlanSecondarySubtitleExternalReload(
		active,
		provider != nullptr,
		external_subtitles && external_subtitles_are_srt && !bitmap_subtitles);
	switch (action) {
		case SecondarySubtitleExternalReloadAction::ReleaseProvider:
			ReleaseProvider();
			return;

		case SecondarySubtitleExternalReloadAction::RebuildProvider:
			RebuildProvider(context->GetCore().project->VideoProvider());
			return;

		case SecondarySubtitleExternalReloadAction::ReloadInPlace:
			break;
	}

	auto core = context->GetCore();
	auto *main_provider = core.project->VideoProvider();
	UpdateExternalSubtitleResolution(main_provider);
	provider->SetSubtitlesTimecodes(core.project->Timecodes());
	provider->LoadSubtitles(external_subtitles.get());
	RequestFrame(core.videoController->GetFrameN());
}

void SecondarySubtitleSession::ReloadExternalSubtitlesAfterChange() {
	if (source_mode != SecondarySubtitleSourceMode::ExternalFile || external_subtitle_path.empty())
		return;

	external_subtitle_reload_pending = true;
	if (!active)
		return;
	if (!LoadConfiguredExternalSubtitles(false, true))
		return;

	RefreshProviderAfterExternalReload();
}

bool SecondarySubtitleSession::LoadExternalSubtitlesFromPath(std::string const& path_string, bool show_errors) {
	auto const path = agi::fs::PathFromString(path_string);
	auto report_error = [&](std::string const& message) {
		if (show_errors)
			context->ShowError(message, kSecondarySubtitleWarningTitle);
		else
			wxLogWarning(wxS("Secondary subtitle reload failed: %s"), to_wx(message));
	};
	auto install_bitmap_stream = [&](SecondarySubtitlePacketStream stream,
		std::optional<int> vobsub_track_index) {
		FillMissingBitmapCanvasFromVideo(
			stream, context->GetCore().project->VideoProvider());
		external_subtitles.reset();
		bitmap_subtitles = std::make_shared<SecondarySubtitlePacketStream>(std::move(stream));
		loaded_external_subtitle_path = path_string;
		external_subtitles_follow_video_resolution = false;
		external_subtitle_reload_pending = false;
		external_subtitle_fps_selection.reset();
		external_subtitles_follow_video_timecodes = false;
		external_subtitles_are_srt = false;
		external_vobsub_track_index = vobsub_track_index;
		UpdateExternalStyleCatalogWatch();
		return true;
	};
	try {
		if (IsPgsSupSubtitlePath(path)) {
			if (!secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecHdmvPgs))
				throw agi::EnvironmentError("No PGS decoder plugin is available in the runtimes directory.");
			return install_bitmap_stream(ReadPgsSupPacketStream(path), std::nullopt);
		}
		if (IsVobSubIndexPath(path)) {
			if (!secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecDvdSubtitle))
				throw agi::EnvironmentError("No VobSub decoder plugin is available in the runtimes directory.");

			auto const index_info = ReadVobSubIndex(path);
			int selected_index = index_info.default_track_index;
			bool const reloading_selected_track = loaded_external_subtitle_path == path_string
				&& external_vobsub_track_index
				&& std::any_of(index_info.tracks.begin(), index_info.tracks.end(), [&](VobSubTrackInfo const& track) {
					return track.index == *external_vobsub_track_index;
				});
			if (reloading_selected_track) {
				selected_index = *external_vobsub_track_index;
			}
			else if (index_info.tracks.size() > 1) {
				std::vector<std::string> choices;
				choices.reserve(index_info.tracks.size());
				int default_choice = 0;
				for (auto const& track : index_info.tracks) {
					auto label = std::to_string(track.index) + " (" + track.language + ")";
					if (!track.name.empty())
						label += ": " + track.name;
					if (track.is_default) {
						label += " [default]";
						default_choice = static_cast<int>(choices.size());
					}
					choices.emplace_back(std::move(label));
				}
				agi::SingleChoiceInteractionRequest request;
				request.title = "Multiple VobSub tracks found";
				request.message = "Choose which VobSub track to load into the secondary subtitle strip:";
				request.choices = std::move(choices);
				request.default_choice = default_choice;
				request.request_id = "vobsub.track_choice.secondary";
				auto choice = context->GetSingleChoiceInteractionSink()->RequestSingleChoice(request);
				auto resolved = aegisub::track_choice::ResolveSelection(index_info.tracks.size(), choice);
				if (!resolved)
					throw agi::UserCancelException("canceled");
				selected_index = index_info.tracks[*resolved].index;
			}

			return install_bitmap_stream(ReadVobSubPacketStream(path, selected_index), selected_index);
		}

		auto charset = CharSetDetect::GetEncoding(path, context->GetSingleChoiceInteractionSink());
		auto const *reader = SubtitleFormat::GetReader(path, charset);
		if (!reader)
			throw UnknownSubtitleFormatError("Subtitle format for extension not found");
		bool const is_srt = reader->GetName() == "SubRip";

		auto core = context->GetCore();
		auto const reuse_fps_selection = loaded_external_subtitle_path == path_string
			? external_subtitle_fps_selection
			: std::nullopt;
		auto choice_sink = std::make_shared<SecondarySubtitleChoiceSink>(
			context->GetSingleChoiceInteractionSink(),
			core.project->Timecodes(),
			reuse_fps_selection);

		AssFile temp;
		reader->ReadFile(
			&temp,
			path,
			core.project->Timecodes(),
			charset,
			choice_sink,
			core.backgroundRunnerFactory);

		auto const follow_video_resolution = temp.GetResolutionType(ScriptResolutionType::PlayRes) == ScriptResolutionType::None;
		if (follow_video_resolution) {
			if (auto *main_provider = context->GetCore().project->VideoProvider())
				temp.SetResolution(ScriptResolutionType::None, main_provider->GetWidth(), main_provider->GetHeight());
		}

		external_subtitles = agi::make_unique<AssFile>();
		external_subtitles->swap(temp);
		bitmap_subtitles.reset();
		loaded_external_subtitle_path = path_string;
		external_subtitles_follow_video_resolution = follow_video_resolution;
		external_subtitle_reload_pending = false;
		external_subtitle_fps_selection = choice_sink->GetRecordedSelection();
		external_subtitles_follow_video_timecodes =
			external_subtitle_fps_selection && external_subtitle_fps_selection->follow_video;
		external_subtitles_are_srt = is_srt;
		external_vobsub_track_index.reset();
		UpdateExternalStyleCatalogWatch();
		return true;
	}
	catch (agi::UserCancelException const&) {
		return false;
	}
	catch (agi::Exception const& err) {
		report_error(err.GetMessage());
	}
	catch (std::exception const& err) {
		report_error(err.what());
	}
	catch (...) {
		report_error("Unknown error while loading secondary subtitles.");
	}
	return false;
}

void SecondarySubtitleSession::UpdateExternalSubtitleResolution(AsyncVideoProvider *main_provider) {
	if (!external_subtitles || !external_subtitles_follow_video_resolution || !main_provider)
		return;

	external_subtitles->SetResolution(ScriptResolutionType::None, main_provider->GetWidth(), main_provider->GetHeight());
}

bool SecondarySubtitleSession::LoadVideoEmbeddedSubtitles(bool show_errors, std::string *selected_track_label) {
	auto core = context->GetCore();
	auto const& video_path = core.project->VideoName();
	if (video_path.empty())
		return false;

	// Only Matroska containers expose embedded subtitle tracks in this codebase.
	bool const is_matroska = agi::fs::HasExtension(video_path, "mkv")
		|| agi::fs::HasExtension(video_path, "mka")
		|| agi::fs::HasExtension(video_path, "mks");
	if (!is_matroska)
		return false;

	try {
		AssFile temp;
		std::optional<MkvTrackScanResult> scanned_tracks;
		try {
			scanned_tracks = MatroskaWrapper::ScanTracks(video_path);
		}
		catch (MatroskaException const&) {
			// The legacy Matroska backend cannot scan or load a numbered track.
			// Preserve its existing text-only path.
			MatroskaWrapper::GetSubtitles(
				video_path, &temp, context->GetSingleChoiceInteractionSink(),
				core.backgroundRunnerFactory, true, selected_track_label);
		}

		if (scanned_tracks) {
			std::vector<MkvTrackInfo const*> candidates;
			for (auto const& track : scanned_tracks->tracks) {
				if (IsImportableMkvSubtitleTrack(track)
					|| HasDecoderForBitmapTrack(track)) {
					candidates.push_back(&track);
				}
			}
			if (candidates.empty())
				throw MatroskaException("File has no supported secondary subtitle tracks.");

			auto const *selected = candidates.front();
			if (candidates.size() > 1) {
				std::vector<std::string> choices;
				choices.reserve(candidates.size());
				for (auto const *track : candidates)
					choices.emplace_back(DescribeMkvTrack(*track));
				auto choice = context->GetSingleChoiceInteractionSink()->RequestSingleChoice(
					aegisub::track_choice::BuildRequest(aegisub::track_choice::DialogKind::Subtitle, choices, true));
				auto resolved = aegisub::track_choice::ResolveSelection(candidates.size(), choice);
				if (!resolved)
					throw agi::UserCancelException("canceled");
				selected = candidates[*resolved];
			}

			if (selected_track_label)
				*selected_track_label = DescribeMkvTrack(*selected);
			if (IsDecodableMkvBitmapSubtitleTrack(*selected)) {
				auto packet_stream = MatroskaWrapper::GetBitmapSubtitlePacketsForTrack(
					video_path, selected->track_number, core.backgroundRunnerFactory);
				FillMissingBitmapCanvasFromVideo(packet_stream, core.project->VideoProvider());
				external_subtitles.reset();
				bitmap_subtitles = std::make_shared<SecondarySubtitlePacketStream>(std::move(packet_stream));
				loaded_external_subtitle_path.clear();
				external_subtitles_follow_video_resolution = false;
				external_subtitle_reload_pending = false;
				external_subtitle_fps_selection.reset();
				external_subtitles_follow_video_timecodes = false;
				external_subtitles_are_srt = false;
				external_vobsub_track_index.reset();
				external_subtitle_path.clear();
				return true;
			}

			MatroskaWrapper::GetTextSubtitlesForTrack(
				video_path, selected->track_number, &temp, core.backgroundRunnerFactory);
		}

		auto const follow_video_resolution = temp.GetResolutionType(ScriptResolutionType::PlayRes) == ScriptResolutionType::None;
		if (follow_video_resolution) {
			if (auto *main_provider = core.project->VideoProvider())
				temp.SetResolution(ScriptResolutionType::None, main_provider->GetWidth(), main_provider->GetHeight());
		}

		external_subtitles = agi::make_unique<AssFile>();
		external_subtitles->swap(temp);
		bitmap_subtitles.reset();
		loaded_external_subtitle_path.clear();
		external_subtitles_follow_video_resolution = follow_video_resolution;
		external_subtitle_reload_pending = false;
		external_subtitle_fps_selection.reset();
		external_subtitles_follow_video_timecodes = false;
		external_subtitles_are_srt = false;
		external_vobsub_track_index.reset();
		external_subtitle_path.clear();
		return true;
	}
	catch (agi::UserCancelException const&) {
		return false;
	}
	catch (agi::Exception const& err) {
		if (show_errors)
			context->ShowError(err.GetMessage(), kSecondarySubtitleWarningTitle);
	}
	catch (std::exception const& err) {
		if (show_errors)
			context->ShowError(err.what(), kSecondarySubtitleWarningTitle);
	}
	catch (...) {
		if (show_errors)
			context->ShowError("Unknown error while loading embedded subtitles.", kSecondarySubtitleWarningTitle);
	}
	return false;
}

void SecondarySubtitleSession::OnVideoHasSubtitlesAvailable() {
	if (!active || video_embedded_auto_prompted)
		return;
	if (source_mode == SecondarySubtitleSourceMode::VideoEmbedded)
		return;
	if (!OPT_GET("Video/Secondary Subtitles/Auto Load From Video")->GetBool())
		return;

	auto core = context->GetCore();
	if (!CanOpenVideoEmbedded())
		return;

	// Claim the prompt now so a second provider-changed notification arriving
	// before the deferred dialog runs cannot queue a duplicate.
	video_embedded_auto_prompted = true;

	// Stamp this queued prompt so a later provider change (which bumps the
	// generation again) supersedes it: only the newest queued lambda runs.
	auto const generation = ++video_embedded_prompt_generation;

	// Defer the modal out of the current notification stack: this is invoked
	// from a video-provider-modified callback, and running a modal dialog (plus
	// the track-choice dialog and background I/O behind OpenVideoEmbeddedSubtitles)
	// synchronously inside that callback would re-enter the load path. CallAfter
	// pushes it to the next event-loop turn instead.
	std::weak_ptr<int> guard = alive;
	wxTheApp->CallAfter([this, guard, generation] {
		// The session may have been destroyed while the call was queued.
		if (guard.expired())
			return;
		// A newer provider change queued a fresher prompt; drop this stale one so
		// two rapid video opens do not stack up two dialogs.
		if (generation != video_embedded_prompt_generation)
			return;
		// Re-validate: state may have changed between queueing and firing (video
		// closed, source switched, strip deactivated, auto-load disabled).
		if (!active || source_mode == SecondarySubtitleSourceMode::VideoEmbedded)
			return;
		if (!OPT_GET("Video/Secondary Subtitles/Auto Load From Video")->GetBool())
			return;
		if (!CanOpenVideoEmbedded())
			return;

		auto answer = wxMessageBox(
			_("The current video contains embedded subtitles. Load them into the secondary subtitle strip?"),
			_("Secondary subtitles"),
			wxYES_NO | wxICON_QUESTION);
		if (answer == wxYES)
			OpenVideoEmbeddedSubtitles();
	});
}

void SecondarySubtitleSession::RebuildProvider(AsyncVideoProvider *main_provider) {
	last_rebuilt_main_provider = nullptr;
	ReleaseProvider();
	ClearBitmap();

	if (!main_provider)
		return;

	auto core = context->GetCore();
	try {
		background_runner = context->CreateBackgroundRunner();
		provider_lifetime = agi::ui::MakeLifetime();
		auto event_sink = CreateAsyncVideoProviderMainThreadSink(
			provider_lifetime,
			{
				[this](VideoRenderPacket packet, double time) {
					OnFrameReady(std::move(packet), time);
				},
				[this](std::string const& message) {
					OnVideoError(message);
				},
				[this](std::string const& message) {
					OnSubtitlesError(message);
				}
			});
		auto *subtitles = ResolveSubtitlesForProvider(main_provider);
		SubtitleRenderEnvironment render_environment;
		render_environment.background_runner = background_runner.get();
		render_environment.transient_fonts = subtitles ? subtitles->GetTransientFonts() : core.ass->GetTransientFonts();
		render_environment.preferred_provider = OPT_GET("Video/Secondary Subtitles/Provider")->GetString();
		auto subtitles_provider = bitmap_subtitles
			? secondary_subtitle_decoder::Create(bitmap_subtitles)
			: SubtitlesProviderFactory::GetProvider(render_environment);
		auto dummy_video_provider = agi::make_unique<DummyVideoProvider>(
			main_provider->GetFPS().FPS(),
			main_provider->GetFrameCount(),
			main_provider->GetWidth(),
			main_provider->GetHeight(),
			OPT_GET("Colour/Secondary Subtitle Strip/Dummy Background")->GetColor(),
			OPT_GET("Video/Secondary Subtitles/Dummy/Pattern")->GetBool());
		provider = agi::make_unique<AsyncVideoProvider>(
			std::move(dummy_video_provider),
			std::move(subtitles_provider),
			std::move(event_sink));
		last_rebuilt_main_provider = main_provider;
		SyncConfiguredSubtitlesSource(main_provider);
		if (active)
			RequestFrame(core.videoController->GetFrameN());
	}
	catch (std::string const& err) {
		context->ShowWarning(err, kSecondarySubtitleWarningTitle);
	}
	catch (agi::Exception const& err) {
		context->ShowWarning(err.GetMessage(), kSecondarySubtitleWarningTitle);
	}
	catch (...) {
		context->ShowWarning("Unknown error while initializing secondary subtitles.", kSecondarySubtitleWarningTitle);
	}
}

void SecondarySubtitleSession::OnVideoProviderChanged(AsyncVideoProvider *main_provider) {
	auto core = context->GetCore();
	// Drop session sources bound to a video that is no longer open so the
	// "Loaded" menu never offers tracks that no longer exist.
	// - On video switch: VideoName() already holds the new path, so sources
	//   bound to a different video are dropped while the new video's are kept.
	// - On video close (main_provider == nullptr): VideoName() still holds the
	//   stale path at notify time, so pass an empty match to drop everything.
	std::string const keep_video = main_provider
		? agi::fs::PathToString(core.project->VideoName())
		: std::string{};
	RemoveVideoEmbeddedSources(keep_video);

	// VideoEmbedded is bound to a specific video. If the active embedded source
	// was dropped (video switched to a different file, or closed),
	// RemoveVideoEmbeddedSources leaves current_source_index == SIZE_MAX; fall
	// back to CurrentScript so the strip keeps rendering something sensible. If
	// it survived (same-path reprovider, e.g. reindex/reopen), keep the source
	// active and let RebuildProvider below re-load its held subtitle data.
	if (source_mode == SecondarySubtitleSourceMode::VideoEmbedded
		&& current_source_index == static_cast<size_t>(-1)) {
		source_mode = SecondarySubtitleSourceMode::CurrentScript;
		external_subtitle_path.clear();
		ClearExternalSubtitles();
		SyncExternalSubtitleProjectProperty();
		UpdateExternalSubtitleWatch();
	}

	video_embedded_auto_prompted = false;
	if (!active) {
		last_rebuilt_main_provider = nullptr;
		ReleaseProvider();
		ClearBitmap();
		return;
	}

	// With the current shared-session owner, VideoBox can activate and rebuild
	// before this listener runs. The provider pointer is unique per open video,
	// so consume that marker instead of rebuilding the same secondary provider twice.
	if (last_rebuilt_main_provider == main_provider) {
		last_rebuilt_main_provider = nullptr;
		return;
	}

	RebuildProvider(main_provider);

	// After the new video is wired up, offer to load its embedded subtitles.
	// OnVideoHasSubtitlesAvailable guards against re-prompting and against the
	// disabled option. If the user accepts, OpenVideoEmbeddedSubtitles rebuilds
	// the provider again with the freshly-extracted tracks.
	OnVideoHasSubtitlesAvailable();
}

void SecondarySubtitleSession::OnTimecodesChanged(agi::vfr::Framerate const&) {
	if (!provider) {
		if (source_mode == SecondarySubtitleSourceMode::ExternalFile
			&& !bitmap_subtitles
			&& external_subtitles_follow_video_timecodes)
			external_subtitle_reload_pending = true;
		return;
	}

	auto core = context->GetCore();
	if (source_mode == SecondarySubtitleSourceMode::ExternalFile
		&& !bitmap_subtitles
		&& external_subtitles_follow_video_timecodes) {
		external_subtitle_reload_pending = true;
		if (active && LoadConfiguredExternalSubtitles(false, true)) {
			RefreshProviderAfterExternalReload();
			return;
		}

		provider->SetSubtitlesTimecodes(core.project->Timecodes());
		if (active)
			RequestFrame(core.videoController->GetFrameN());
		return;
	}

	// CurrentScript and VideoEmbedded keep their existing subtitles and just
	// re-sync the timecodes handed to the provider.
	provider->SetSubtitlesTimecodes(core.project->Timecodes());
	if (active)
		RequestFrame(core.videoController->GetFrameN());
}

void SecondarySubtitleSession::OnAssCommit(AssFileCommitDetails commit) {
	if (!provider || source_mode != SecondarySubtitleSourceMode::CurrentScript)
		return;

	auto core = context->GetCore();
	if (video_subtitle_update_policy::SelectUpdateMode(commit.type, commit.changed_lines)
		== video_subtitle_update_policy::UpdateMode::IncrementalLines)
		provider->UpdateSubtitles(core.ass.get(), commit.changed_lines);
	else
		provider->LoadSubtitles(core.ass.get());

	// Without a new primary frame presented, the secondary strip would not be
	// re-rendered. Request the current frame so edits are visible immediately.
	if (active)
		RequestFrame(core.videoController->GetFrameN());
}

void SecondarySubtitleSession::OnPrimaryFramePresented(int frame_number) {
	if (!active)
		return;
	RequestFrame(frame_number);
}

void SecondarySubtitleSession::OnFrameReady(VideoRenderPacket packet, double) {
	if (!active || !presentation_demand.HasDemand() || !provider
		|| !provider->IsCurrent(packet, current_frame))
		return;

	auto frame = BakePacketForCpuReadback(packet);
	if (!frame || frame->data.empty()) {
		ClearBitmap();
		return;
	}

	current_bitmap = static_cast<wxBitmap>(GetImage(*frame));
	has_bitmap = current_bitmap.IsOk();
	NotifyBitmapUpdated();
}

void SecondarySubtitleSession::OnVideoError(std::string const& message) {
	wxLogWarning(wxS("Secondary subtitle strip video error: %s"), to_wx(message));
}

void SecondarySubtitleSession::OnSubtitlesError(std::string const& message) {
	wxLogWarning(wxS("Secondary subtitle strip subtitles error: %s"), to_wx(message));
}

bool SecondarySubtitleSession::OpenExternalSubtitles() {
	auto wildcards = SubtitleFormat::GetWildcards(0);
	if (secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecHdmvPgs))
		wildcards = "PGS bitmap subtitles (*.sup,*.pgs)|*.sup;*.pgs|" + wildcards;
	if (secondary_subtitle_decoder::IsAvailable(kSecondarySubtitleCodecDvdSubtitle))
		wildcards = "VobSub bitmap subtitles (*.idx; paired .sub)|*.idx|" + wildcards;
	// Prepend "All files" so the dialog defaults to showing everything
	// instead of filtering to the first entry.
	wildcards = "All files (*.*)|*.*|" + wildcards;

	auto path = context->RequestOpenFile({
		from_wx(_("Open secondary subtitles")),
		"Path/Last/Subtitles",
		"",
		"",
		wildcards
	});
	if (path.empty())
		return false;

	auto path_string = agi::fs::PathToString(path);
	if (!LoadExternalSubtitlesFromPath(path_string, true))
		return false;

	source_mode = SecondarySubtitleSourceMode::ExternalFile;
	external_subtitle_path = path_string;
	SyncExternalSubtitleProjectProperty();
	UpdateExternalSubtitleWatch();
	RegisterExternalSource(path);
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	else
		ReleaseProvider();
	return true;
}

bool SecondarySubtitleSession::OpenExternalSubtitlesFromPath(agi::fs::path const& path, bool show_errors) {
	if (path.empty())
		return false;

	auto const path_string = agi::fs::PathToString(path);
	if (!LoadExternalSubtitlesFromPath(path_string, show_errors))
		return false;

	source_mode = SecondarySubtitleSourceMode::ExternalFile;
	external_subtitle_path = path_string;
	SyncExternalSubtitleProjectProperty();
	UpdateExternalSubtitleWatch();
	RegisterExternalSource(path);
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	else
		ReleaseProvider();
	return true;
}

bool SecondarySubtitleSession::CanOpenVideoEmbedded() const {
	auto const& project = *context->GetCore().project;
	return project.CanLoadSubtitlesFromVideo()
		|| HasDecoderForAvailableBitmapTrack(project);
}

bool SecondarySubtitleSession::OpenVideoEmbeddedSubtitles() {
	auto core = context->GetCore();
	if (!core.project->VideoProvider()) {
		context->ShowError("Open a video first.", kSecondarySubtitleWarningTitle);
		return false;
	}
	if (!CanOpenVideoEmbedded()) {
		context->ShowError("The current video has no embedded subtitle tracks.", kSecondarySubtitleWarningTitle);
		return false;
	}

	std::string selected_track_label;
	if (!LoadVideoEmbeddedSubtitles(true, &selected_track_label))
		return false;

	source_mode = SecondarySubtitleSourceMode::VideoEmbedded;
	external_subtitle_path.clear();
	SyncExternalSubtitleProjectProperty();
	UpdateExternalSubtitleWatch();
	if (external_subtitles) {
		auto const video_path = core.project->VideoName();
		if (!video_path.empty())
			RegisterVideoEmbeddedSource(video_path, selected_track_label, *external_subtitles, external_subtitles_follow_video_resolution);
	}
	else if (bitmap_subtitles) {
		auto const video_path = core.project->VideoName();
		if (!video_path.empty())
			RegisterVideoEmbeddedBitmapSource(video_path, selected_track_label, bitmap_subtitles);
	}
	if (active)
		RebuildProvider(core.project->VideoProvider());
	else
		ReleaseProvider();
	return true;
}

bool SecondarySubtitleSession::ReloadSubtitles() {
	if (source_mode == SecondarySubtitleSourceMode::CurrentScript) {
		SyncConfiguredSubtitlesSource();
		if (provider)
			RequestFrame(context->GetCore().videoController->GetFrameN());
		return true;
	}

	if (source_mode == SecondarySubtitleSourceMode::VideoEmbedded) {
		std::string selected_track_label;
		bool const reloaded = LoadVideoEmbeddedSubtitles(true, &selected_track_label);
		// Refresh the snapshot so switching away and back no longer resurrects
		// the pre-reload track; also reflects a re-picked track's label/policy.
		if (reloaded && (external_subtitles || bitmap_subtitles)
			&& current_source_index != static_cast<size_t>(-1)
			&& current_source_index < loaded_sources.size()
			&& loaded_sources[current_source_index].kind == LoadedSecondarySource::Kind::VideoEmbedded) {
			auto &src = loaded_sources[current_source_index];
			src.held_subtitle = external_subtitles ? agi::make_unique<AssFile>(*external_subtitles) : nullptr;
			src.held_bitmap_subtitle = bitmap_subtitles;
			src.follow_video_resolution = external_subtitles_follow_video_resolution;
			if (!selected_track_label.empty())
				src.label = from_wx(_("embedded")) + " " + selected_track_label;
		}
		if (active)
			RebuildProvider(context->GetCore().project->VideoProvider());
		else
			ReleaseProvider();
		return reloaded;
	}

	bool const reloaded = LoadConfiguredExternalSubtitles(true, true);
	if (reloaded)
		RefreshProviderAfterExternalReload();
	return reloaded;
}

bool SecondarySubtitleSession::IsFollowingGlobalSubtitlesProvider() const {
	return OPT_GET("Video/Secondary Subtitles/Provider")->GetString().empty();
}

std::string SecondarySubtitleSession::GetConfiguredSubtitlesProvider() const {
	return OPT_GET("Video/Secondary Subtitles/Provider")->GetString();
}

std::string SecondarySubtitleSession::GetEffectiveSubtitlesProvider() const {
	auto configured_provider = OPT_GET("Video/Secondary Subtitles/Provider")->GetString();
	if (!configured_provider.empty())
		return configured_provider;

	return OPT_GET("Subtitle/Provider")->GetString();
}

void SecondarySubtitleSession::SetActive(bool value) {
	if (active == value)
		return;

	active = value;
	if (!active) {
		last_rebuilt_main_provider = nullptr;
		UpdateExternalSubtitleWatch();
		if (provider) {
			provider->CancelPendingFrameRequests();
			if (!bitmap_subtitles)
				ReleaseProvider();
		}
		ClearBitmap();
		return;
	}

	auto core = context->GetCore();
	if (source_mode == SecondarySubtitleSourceMode::ExternalFile && external_subtitle_reload_pending) {
		if (LoadConfiguredExternalSubtitles(false, true))
			ReleaseProvider();
	}
	UpdateExternalSubtitleWatch();
	if (!provider && core.project->VideoProvider())
		RebuildProvider(core.project->VideoProvider());
	else if (provider)
		SyncConfiguredSubtitlesSource();

	if (provider)
		RequestFrame(core.videoController->GetFrameN());

	// If the strip is being enabled after a video was opened while hidden,
	// offer to load its embedded subtitles.
	OnVideoHasSubtitlesAvailable();
}

void SecondarySubtitleSession::SetPresentationDemand(void const *presenter, bool demanded) {
	auto const change = presentation_demand.Set(presenter, demanded);
	if (change == SecondarySubtitlePresentationDemandChange::None)
		return;

	if (change == SecondarySubtitlePresentationDemandChange::BecameIdle) {
		// Avoid doing work for a frame which can no longer be displayed. The
		// provider remains alive so re-showing the strip does not rebuild it.
		if (provider)
			provider->CancelPendingFrameRequests();
		return;
	}

	// A presenter becoming visible may be the first demand after activation;
	// request exactly the current primary frame to repopulate the bitmap.
	if (active && provider)
		RequestFrame(context->GetCore().videoController->GetFrameN());
}

void SecondarySubtitleSession::UseGlobalSubtitlesProvider() {
	OPT_SET("Video/Secondary Subtitles/Provider")->SetString("");
}

void SecondarySubtitleSession::UseIndependentSubtitlesProvider(std::string const& provider_name) {
	OPT_SET("Video/Secondary Subtitles/Provider")->SetString(provider_name);
}

void SecondarySubtitleSession::UseCurrentScriptSource() {
	source_mode = SecondarySubtitleSourceMode::CurrentScript;
	external_subtitle_path.clear();
	ClearExternalSubtitles();
	SyncExternalSubtitleProjectProperty();
	UpdateExternalSubtitleWatch();
	current_source_index = static_cast<size_t>(-1);
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	else
		ReleaseProvider();
}

void SecondarySubtitleSession::RegisterExternalSource(agi::fs::path const& path) {
	auto const path_string = agi::fs::PathToString(path);
	// De-duplicate: drop any existing entry for the same path so it moves to
	// the end (most recent). Also drop any VideoEmbedded entry bound to a
	// different video that no longer matches the open file.
	loaded_sources.erase(
		std::remove_if(loaded_sources.begin(), loaded_sources.end(),
			[&](LoadedSecondarySource const& s) {
				return s.kind == LoadedSecondarySource::Kind::ExternalFile
					&& s.file_path == path_string;
			}),
		loaded_sources.end());

	LoadedSecondarySource src;
	src.kind = LoadedSecondarySource::Kind::ExternalFile;
	src.label = agi::fs::PathToString(path.filename());
	src.file_path = path_string;
	loaded_sources.push_back(std::move(src));
	current_source_index = loaded_sources.size() - 1;
}

void SecondarySubtitleSession::RegisterVideoEmbeddedSource(agi::fs::path const& video_path, std::string const& track_label, AssFile const& subtitles, bool follow_video_resolution) {
	// Hold a private copy so switching back to this source does not re-extract
	// the track or re-prompt for a track choice. The menu label prefixes the
	// track description (codec/language/name) with "embedded" to distinguish
	// it from external-file sources.
	LoadedSecondarySource src;
	src.kind = LoadedSecondarySource::Kind::VideoEmbedded;
	src.label = from_wx(_("embedded")) + " " + track_label;
	src.file_path.clear();
	src.held_subtitle = agi::make_unique<AssFile>(subtitles);
	src.video_origin = agi::fs::PathToString(video_path);
	// Record whether this track lacks an intrinsic PlayRes (SRT etc.) so it can
	// be re-scaled to the video resolution when re-activated, while ASS/SSA
	// tracks keep their own PlayRes untouched.
	src.follow_video_resolution = follow_video_resolution;
	loaded_sources.push_back(std::move(src));
	current_source_index = loaded_sources.size() - 1;
}

void SecondarySubtitleSession::RegisterVideoEmbeddedBitmapSource(agi::fs::path const& video_path, std::string const& track_label, std::shared_ptr<const SecondarySubtitlePacketStream> subtitles) {
	LoadedSecondarySource src;
	src.kind = LoadedSecondarySource::Kind::VideoEmbedded;
	src.label = from_wx(_("embedded")) + " " + track_label;
	src.held_bitmap_subtitle = std::move(subtitles);
	src.video_origin = agi::fs::PathToString(video_path);
	loaded_sources.push_back(std::move(src));
	current_source_index = loaded_sources.size() - 1;
}

void SecondarySubtitleSession::RemoveVideoEmbeddedSources(std::string const& except_video) {
	auto const had_active = current_source_index != static_cast<size_t>(-1)
		&& current_source_index < loaded_sources.size();

	// std::remove_if preserves the relative order of surviving elements, so the
	// active source's new index is simply the number of survivors that precede
	// its old position. Compute that while the old indices are still valid, then
	// relocate after the erase. This works for VideoEmbedded sources too (an
	// entry bound to the still-open video survives), not just ExternalFile.
	auto const should_drop = [&](LoadedSecondarySource const& s) {
		return s.kind == LoadedSecondarySource::Kind::VideoEmbedded
			&& s.video_origin != except_video;
	};

	bool active_survives = false;
	size_t relocated_index = 0;
	if (had_active) {
		active_survives = !should_drop(loaded_sources[current_source_index]);
		for (size_t i = 0; i < current_source_index; ++i) {
			if (!should_drop(loaded_sources[i]))
				++relocated_index;
		}
	}

	loaded_sources.erase(
		std::remove_if(loaded_sources.begin(), loaded_sources.end(), should_drop),
		loaded_sources.end());

	current_source_index = active_survives ? relocated_index : static_cast<size_t>(-1);
}

void SecondarySubtitleSession::ActivateLoadedSource(size_t index) {
	if (index >= loaded_sources.size())
		return;

	auto const& src = loaded_sources[index];
	if (src.kind == LoadedSecondarySource::Kind::ExternalFile) {
		// Re-read from disk. OpenExternalSubtitlesFromPath re-registers the
		// source (moving it to the end and updating current_source_index).
		OpenExternalSubtitlesFromPath(agi::fs::PathFromString(src.file_path));
		return;
	}

	// VideoEmbedded: switch from the held copy without re-extracting.
	if (!src.held_subtitle && !src.held_bitmap_subtitle)
		return;

	external_subtitles = src.held_subtitle ? agi::make_unique<AssFile>(*src.held_subtitle) : nullptr;
	bitmap_subtitles = src.held_bitmap_subtitle;
	source_mode = SecondarySubtitleSourceMode::VideoEmbedded;
	external_subtitle_path.clear();
	// Restore this source's own resolution policy rather than inheriting a stale
	// value from the previously-active source. SRT-style tracks (no intrinsic
	// PlayRes) keep following the video resolution; ASS/SSA tracks keep their
	// own PlayRes and are never re-scaled.
	external_subtitles_follow_video_resolution = src.follow_video_resolution;
	loaded_external_subtitle_path.clear();
	external_subtitles_are_srt = false;
	external_vobsub_track_index.reset();
	current_source_index = index;
	SyncExternalSubtitleProjectProperty(); // VideoEmbedded clears the property
	UpdateExternalSubtitleWatch();
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	else
		ReleaseProvider();
}

void SecondarySubtitleSession::OnExternalSubtitleFileChanged(agi::fs::path const&) {
	ReloadExternalSubtitlesAfterChange();
}

void SecondarySubtitleSession::OnExternalStyleCatalogFileChanged(agi::fs::path const&) {
	if (source_mode != SecondarySubtitleSourceMode::ExternalFile || !external_subtitles_are_srt)
		return;
	ReloadExternalSubtitlesAfterChange();
}

void SecondarySubtitleSession::OnExternalSubtitleWatchError(std::string const& message) {
	LOG_W("secondary_subtitle/session") << "Secondary subtitle watcher error: " << message;
}
