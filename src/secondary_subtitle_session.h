// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "ui_dispatch.h"

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wx/bitmap.h>

class AssDialogue;
class AssFile;
class AsyncVideoProvider;
class WatchedFile;
struct VideoRenderPacket;

/// A secondary-subtitle source recorded for fast switching within the
/// current session. Never persisted to disk.
struct LoadedSecondarySource {
	enum class Kind { ExternalFile, VideoEmbedded };
	Kind kind;
	std::string label;     ///< Menu display name (filename / "video.mkv: embedded")
	std::string file_path; ///< Absolute path; empty for VideoEmbedded
	/// Held subtitle data. Only populated for VideoEmbedded (external sources
	/// are re-read from disk on activation); always non-null when present.
	std::unique_ptr<AssFile> held_subtitle;
	std::string video_origin; ///< Video path the source was extracted from; empty for ExternalFile
};
namespace agi {
	struct Context;
	class BackgroundRunner;
	class OptionValue;
	namespace vfr { class Framerate; }
}

class SecondarySubtitleSession final {
	enum class SecondarySubtitleSourceMode : int {
		CurrentScript = 0,
		ExternalFile = 1,
		// Subtitles embedded in the currently-open video container (e.g. MKV).
		// Session-level only: never persisted, never file-watched.
		VideoEmbedded = 2
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
	// Guards the "video has embedded subtitles" auto-prompt so it asks at most
	// once per video. Reset whenever the video provider changes.
	bool video_embedded_auto_prompted = false;

	/// Session-level list of loaded secondary-subtitle sources for the
	/// "Loaded" quick-switch submenu. Not persisted. SIZE_MAX index means the
	/// current source is CurrentScript (nothing in the list is active).
	std::vector<LoadedSecondarySource> loaded_sources;
	size_t current_source_index = static_cast<size_t>(-1);

	void RegisterExternalSource(agi::fs::path const& path);
	void RegisterVideoEmbeddedSource(agi::fs::path const& video_path, std::string const& track_label, AssFile const& subtitles);
	void RemoveVideoEmbeddedSources(std::string const& except_video);

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
	void OnMainSubtitlesFileChanged(agi::fs::path const& filename, bool is_reload);
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
	bool LoadVideoEmbeddedSubtitles(bool show_errors, std::string *selected_track_label = nullptr);
	void OnVideoHasSubtitlesAvailable();

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
	bool OpenVideoEmbeddedSubtitles();
	bool CanOpenVideoEmbedded() const;
	bool ReloadSubtitles();
	bool IsFollowingGlobalSubtitlesProvider() const;
	std::string GetConfiguredSubtitlesProvider() const;
	std::string GetEffectiveSubtitlesProvider() const;
	void SetActive(bool value);
	void UseGlobalSubtitlesProvider();
	void UseIndependentSubtitlesProvider(std::string const& provider_name);
	void UseCurrentScriptSource();

	/// Session-level "Loaded" quick-switch sources (see LoadedSecondarySource).
	std::vector<LoadedSecondarySource> const& GetLoadedSources() const { return loaded_sources; }
	/// Index of the currently active source in GetLoadedSources(), or
	/// SIZE_MAX when the active source is CurrentScript.
	size_t GetCurrentLoadedSourceIndex() const { return current_source_index; }
	/// Switch to a source in the "Loaded" list. External files are re-read from
	/// disk; VideoEmbedded sources reuse their held subtitle data.
	void ActivateLoadedSource(size_t index);

	bool HasBitmap() const { return has_bitmap && current_bitmap.IsOk(); }
	wxBitmap const& GetBitmap() const { return current_bitmap; }
	void SetBitmapUpdatedCallback(std::function<void()> callback) { bitmap_updated = std::move(callback); }
};
