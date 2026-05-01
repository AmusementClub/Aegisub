// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "ui_dispatch.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <string>
#include <wx/bitmap.h>

class AssDialogue;
class AssFile;
class AsyncVideoProvider;
class WatchedFile;
struct VideoRenderPacket;
namespace agi {
	struct Context;
	class BackgroundRunner;
	class OptionValue;
	namespace vfr { class Framerate; }
}

class SecondarySubtitleSession final {
	enum class SecondarySubtitleSourceMode : int {
		CurrentScript = 0,
		ExternalFile = 1
	};

	agi::Context *context;
	agi::ui::UiActivationScope ui_activation;
	agi::ui::Lifetime provider_lifetime;

	std::unique_ptr<agi::BackgroundRunner> background_runner;
	std::unique_ptr<AsyncVideoProvider> provider;
	std::unique_ptr<AssFile> external_subtitles;
	std::unique_ptr<WatchedFile> external_subtitle_watch;
	SecondarySubtitleSourceMode source_mode = SecondarySubtitleSourceMode::CurrentScript;
	std::string external_subtitle_path;
	std::string loaded_external_subtitle_path;
	bool external_subtitles_follow_video_resolution = false;
	bool external_subtitles_use_plugin_provider = false;

	wxBitmap current_bitmap;
	bool has_bitmap = false;
	bool active = false;
	int current_frame = -1;
	std::function<void()> bitmap_updated;

	void NotifyBitmapUpdated();
	void ClearBitmap();
	void ClearExternalSubtitles();
	void ReleaseProvider();
	void OnDummyBackgroundColorChanged(agi::OptionValue const& opt);
	void OnDummyBackgroundPatternChanged(agi::OptionValue const& opt);
	void OnConfiguredProviderChanged(agi::OptionValue const& opt);
	void OnGlobalProviderChanged(agi::OptionValue const& opt);
	void OnMainSubtitlesFileChanged(agi::fs::path const& filename);
	void OnUpdateProperties();
	void RestoreSourceFromProjectProperties();
	void SyncExternalSubtitleProjectProperty();
	void UpdateExternalSubtitleWatch();
	AssFile *ResolveSubtitlesForProvider(AsyncVideoProvider *main_provider);
	void RebuildProvider(AsyncVideoProvider *main_provider);
	void RequestFrame(int frame_number);
	void SyncConfiguredSubtitlesSource(AsyncVideoProvider *main_provider = nullptr);
	bool LoadConfiguredExternalSubtitles(bool show_errors, bool force_reload = false);
	bool LoadExternalSubtitlesFromPath(std::string const& path_string, bool show_errors);
	bool ShouldUsePluginProviderForExternalFile(std::string const& path_string) const;
	void UpdateExternalSubtitleResolution(AsyncVideoProvider *main_provider);

	void OnVideoProviderChanged(AsyncVideoProvider *main_provider);
	void OnTimecodesChanged(agi::vfr::Framerate const& timecodes);
	void OnAssCommit(int type, AssDialogue const* changed);
	void OnPrimaryFramePresented(int frame_number);
	void OnFrameReady(VideoRenderPacket packet, double time);
	void OnVideoError(std::string const& message);
	void OnSubtitlesError(std::string const& message);
	void OnExternalSubtitleFileChanged(agi::fs::path const& path);
	void OnExternalSubtitleWatchError(std::string const& message);

public:
	explicit SecondarySubtitleSession(agi::Context *context);
	~SecondarySubtitleSession();

	bool OpenExternalSubtitles();
	bool OpenExternalSubtitlesFromPath(agi::fs::path const& path, bool show_errors = true);
	bool ReloadSubtitles();
	bool IsFollowingGlobalSubtitlesProvider() const;
	std::string GetConfiguredSubtitlesProvider() const;
	std::string GetEffectiveSubtitlesProvider() const;
	void SetActive(bool value);
	void UseGlobalSubtitlesProvider();
	void UseIndependentSubtitlesProvider(std::string const& provider_name);
	void UseCurrentScriptSource();
	bool HasBitmap() const { return has_bitmap && current_bitmap.IsOk(); }
	wxBitmap const& GetBitmap() const { return current_bitmap; }
	void SetBitmapUpdatedCallback(std::function<void()> callback) { bitmap_updated = std::move(callback); }
};
