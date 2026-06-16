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
#include "options.h"
#include "project.h"
#include "subs_controller.h"
#include "subtitle_format.h"
#include "ui_services.h"
#include "video_controller.h"
#include "video_frame.h"
#include "video_provider_dummy.h"
#include "watched_file.h"

#include <libaegisub/color.h>
#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <exception>
#include <wx/bitmap.h>
#include <wx/intl.h>
#include <wx/log.h>

namespace {
constexpr char const *kSecondarySubtitleWarningTitle = "Secondary subtitles";
}

SecondarySubtitleSession::SecondarySubtitleSession(agi::Context *context)
: context(context)
, external_subtitle_watch(agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend())) {
	auto core = context->GetCore();
	auto ui = context->GetUI();
	ui_activation.AddConnections(
		core.project->AddVideoProviderListener(&SecondarySubtitleSession::OnVideoProviderChanged, this),
		core.project->AddTimecodesListener(&SecondarySubtitleSession::OnTimecodesChanged, this),
		core.ass->AddCommitListener(&SecondarySubtitleSession::OnAssCommit, this),
		core.subsController->AddFileOpenListener(&SecondarySubtitleSession::OnMainSubtitlesFileChanged, this),
		core.subsController->AddUpdatePropertiesListener(&SecondarySubtitleSession::OnUpdateProperties, this),
		core.videoController->AddFramePresentedListener(&SecondarySubtitleSession::OnPrimaryFramePresented, this),
		OPT_SUB("Colour/Secondary Subtitle Strip/Dummy Background", &SecondarySubtitleSession::OnDummyBackgroundColorChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Dummy/Pattern", &SecondarySubtitleSession::OnDummyBackgroundPatternChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Provider", &SecondarySubtitleSession::OnConfiguredProviderChanged, this),
		OPT_SUB("Subtitle/Provider", &SecondarySubtitleSession::OnGlobalProviderChanged, this));
	external_subtitle_watch->SetChangedCallback([this](agi::fs::path const& path) {
		OnExternalSubtitleFileChanged(path);
	});
	external_subtitle_watch->SetErrorCallback([this](std::string const& message) {
		OnExternalSubtitleWatchError(message);
	});
	RestoreSourceFromProjectProperties();
}

SecondarySubtitleSession::~SecondarySubtitleSession() {
	external_subtitle_watch.reset();
	ui_activation.Deactivate();
	ReleaseProvider();
}

void SecondarySubtitleSession::NotifyBitmapUpdated() {
	if (bitmap_updated)
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
	loaded_external_subtitle_path.clear();
	external_subtitles_follow_video_resolution = false;
	external_subtitles_use_plugin_provider = false;
}

void SecondarySubtitleSession::ReleaseProvider() {
	provider.reset();
	background_runner.reset();
	provider_lifetime.reset();
}

void SecondarySubtitleSession::OnDummyBackgroundColorChanged(agi::OptionValue const&) {
	if (!active)
		return;

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnDummyBackgroundPatternChanged(agi::OptionValue const&) {
	if (!active)
		return;

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnConfiguredProviderChanged(agi::OptionValue const&) {
	if (!active)
		return;

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnGlobalProviderChanged(agi::OptionValue const&) {
	if (!active || !IsFollowingGlobalSubtitlesProvider())
		return;

	RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnMainSubtitlesFileChanged(agi::fs::path const&) {
	RestoreSourceFromProjectProperties();

	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnUpdateProperties() {
	SyncExternalSubtitleProjectProperty();
}

void SecondarySubtitleSession::RestoreSourceFromProjectProperties() {
	auto core = context->GetCore();
	source_mode = SecondarySubtitleSourceMode::CurrentScript;
	external_subtitle_path.clear();
	ClearExternalSubtitles();

	auto const& stored_path = core.ass->Properties.secondary_subtitles_file;
	if (stored_path.empty()) {
		UpdateExternalSubtitleWatch();
		return;
	}

	source_mode = SecondarySubtitleSourceMode::ExternalFile;
	external_subtitle_path = agi::fs::PathToString(core.path->MakeAbsolute(stored_path, "?script"));
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
	if (!provider || frame_number < 0)
		return;

	current_frame = frame_number;
	auto core = context->GetCore();
	provider->RequestFrame(frame_number, core.project->Timecodes().TimeAtFrame(frame_number));
}

void SecondarySubtitleSession::UpdateExternalSubtitleWatch() {
	if (!external_subtitle_watch)
		return;

	if (!active || source_mode != SecondarySubtitleSourceMode::ExternalFile || external_subtitle_path.empty()) {
		external_subtitle_watch->ClearTargetPath();
		return;
	}

	external_subtitle_watch->SetTargetPath(agi::fs::PathFromString(external_subtitle_path));
}

AssFile *SecondarySubtitleSession::ResolveSubtitlesForProvider(AsyncVideoProvider *main_provider) {
	auto core = context->GetCore();
	if (source_mode == SecondarySubtitleSourceMode::CurrentScript)
		return core.ass.get();
	if (external_subtitles_use_plugin_provider)
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
		// Plugin providers decode from external files; their overlay
		// may still be valid while the file is being re-read.
		// Only clear the bitmap for ASS-based providers that load
		// their data through this call.
		if (!external_subtitles_use_plugin_provider)
			ClearBitmap();
	}
}

bool SecondarySubtitleSession::LoadConfiguredExternalSubtitles(bool show_errors, bool force_reload) {
	auto path_string = external_subtitle_path;
	if (path_string.empty())
		return false;
	if (ShouldUsePluginProviderForExternalFile(path_string)) {
		external_subtitles.reset();
		loaded_external_subtitle_path = path_string;
		external_subtitles_follow_video_resolution = false;
		external_subtitles_use_plugin_provider = true;
		return true;
	}

	if (!force_reload && external_subtitles && loaded_external_subtitle_path == path_string)
		return true;

	return LoadExternalSubtitlesFromPath(path_string, show_errors);
}

bool SecondarySubtitleSession::ShouldUsePluginProviderForExternalFile(std::string const& path_string) const {
	return SubtitlesProviderFactory::HasExternalFileProviderFor(agi::fs::PathFromString(path_string));
}

bool SecondarySubtitleSession::LoadExternalSubtitlesFromPath(std::string const& path_string, bool show_errors) {
	if (ShouldUsePluginProviderForExternalFile(path_string)) {
		external_subtitles.reset();
		loaded_external_subtitle_path = path_string;
		external_subtitles_follow_video_resolution = false;
		external_subtitles_use_plugin_provider = true;
		return true;
	}

	auto const path = agi::fs::PathFromString(path_string);
	try {
		auto charset = CharSetDetect::GetEncoding(path, context->GetSingleChoiceInteractionSink());
		auto const *reader = SubtitleFormat::GetReader(path, charset);
		if (!reader)
			throw UnknownSubtitleFormatError("Subtitle format for extension not found");

		AssFile temp;
		reader->ReadFile(
			&temp,
			path,
			context->GetCore().project->Timecodes(),
			charset,
			context->GetSingleChoiceInteractionSink(),
			context->GetCore().backgroundRunnerFactory);

		auto const follow_video_resolution = temp.GetResolutionType(ScriptResolutionType::PlayRes) == ScriptResolutionType::None;
		if (follow_video_resolution) {
			if (auto *main_provider = context->GetCore().project->VideoProvider())
				temp.SetResolution(ScriptResolutionType::None, main_provider->GetWidth(), main_provider->GetHeight());
		}

		external_subtitles = agi::make_unique<AssFile>();
		external_subtitles->swap(temp);
		loaded_external_subtitle_path = path_string;
		external_subtitles_follow_video_resolution = follow_video_resolution;
		external_subtitles_use_plugin_provider = false;
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
			context->ShowError("Unknown error while loading secondary subtitles.", kSecondarySubtitleWarningTitle);
	}
	return false;
}

void SecondarySubtitleSession::UpdateExternalSubtitleResolution(AsyncVideoProvider *main_provider) {
	if (!external_subtitles || !external_subtitles_follow_video_resolution || !main_provider)
		return;

	external_subtitles->SetResolution(ScriptResolutionType::None, main_provider->GetWidth(), main_provider->GetHeight());
}

void SecondarySubtitleSession::RebuildProvider(AsyncVideoProvider *main_provider) {
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
		if (source_mode == SecondarySubtitleSourceMode::ExternalFile && external_subtitles_use_plugin_provider) {
			render_environment.external_subtitle_file = agi::fs::PathFromString(external_subtitle_path);
			render_environment.require_external_file_provider = true;
		}
		auto subtitles_provider = SubtitlesProviderFactory::GetProvider(render_environment);
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
	if (!active) {
		ReleaseProvider();
		ClearBitmap();
		return;
	}

	RebuildProvider(main_provider);
}

void SecondarySubtitleSession::OnTimecodesChanged(agi::vfr::Framerate const&) {
	if (!provider)
		return;

	auto core = context->GetCore();
	if (source_mode == SecondarySubtitleSourceMode::ExternalFile && active) {
		RebuildProvider(core.project->VideoProvider());
		return;
	}

	provider->SetSubtitlesTimecodes(core.project->Timecodes());
	if (active)
		RequestFrame(core.videoController->GetFrameN());
}

void SecondarySubtitleSession::OnAssCommit(int, AssDialogue const* changed) {
	if (!provider || source_mode != SecondarySubtitleSourceMode::CurrentScript)
		return;

	auto core = context->GetCore();
	if (changed)
		provider->UpdateSubtitles(core.ass.get(), changed);
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
	if (!active)
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
	auto external_plugin_wildcards = SubtitlesProviderFactory::GetExternalFileProviderWildcards();
	auto wildcards = SubtitleFormat::GetWildcards(0);
	if (!external_plugin_wildcards.empty()) {
		auto joined = agi::util::strings::join(external_plugin_wildcards, ";");
		wildcards = "Dynamic Subtitle Plugins (" + agi::util::strings::join(external_plugin_wildcards, ",") + ")|" + joined + "|" + wildcards;
	}
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
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
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
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
	return true;
}

bool SecondarySubtitleSession::ReloadSubtitles() {
	if (source_mode == SecondarySubtitleSourceMode::CurrentScript) {
		SyncConfiguredSubtitlesSource();
		if (provider)
			RequestFrame(context->GetCore().videoController->GetFrameN());
		return true;
	}

	bool const reloaded = LoadConfiguredExternalSubtitles(true, true);
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
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
		UpdateExternalSubtitleWatch();
		ReleaseProvider();
		ClearBitmap();
		return;
	}

	auto core = context->GetCore();
	UpdateExternalSubtitleWatch();
	if (!provider && core.project->VideoProvider())
		RebuildProvider(core.project->VideoProvider());
	else if (provider)
		SyncConfiguredSubtitlesSource();

	if (provider)
		RequestFrame(core.videoController->GetFrameN());
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
	if (active)
		RebuildProvider(context->GetCore().project->VideoProvider());
}

void SecondarySubtitleSession::OnExternalSubtitleFileChanged(agi::fs::path const&) {
	if (!active || source_mode != SecondarySubtitleSourceMode::ExternalFile || external_subtitle_path.empty())
		return;

	if (!LoadConfiguredExternalSubtitles(false, true))
		return;

	if (provider) {
		SyncConfiguredSubtitlesSource(context->GetCore().project->VideoProvider());
		RequestFrame(context->GetCore().videoController->GetFrameN());
	}
	else if (active) {
		RebuildProvider(context->GetCore().project->VideoProvider());
	}
}

void SecondarySubtitleSession::OnExternalSubtitleWatchError(std::string const& message) {
	wxLogWarning(wxS("Secondary subtitle watcher error: %s"), to_wx(message));
}
