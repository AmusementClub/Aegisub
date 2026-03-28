#pragma once

#include "project_open_service.h"
#include "provider_selection_diagnostics.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <optional>
#include <string>

namespace agi {
	struct ContextCoreSession;
	struct ConstContextCoreSession;
}

namespace aegisub::headless_playback_session_host {

struct PlaybackSessionHostOptions {
	std::optional<std::string> video_provider;
	std::optional<std::string> audio_provider;
	std::optional<agi::fs::path> trace_dir;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	std::string trace_dir_pattern = "headless-playback-session-%%%%%%%%";
};

class PlaybackSessionHost final {
public:
	explicit PlaybackSessionHost(PlaybackSessionHostOptions options = {});
	~PlaybackSessionHost();

	agi::ContextCoreSession GetCore();
	agi::ConstContextCoreSession GetCore() const;

	bool Start(int& error_code, std::string& error_message);
	project_open_service::ProjectOpenResult OpenMedia(project_open_service::PlaybackOpenOptions const& options);
	project_open_service::ProjectOpenResult ReopenMedia();
	void CloseMedia();
	void ShutdownTrace();
	void ReleaseResources();

	bool HasLastOpenOptions() const;
	int GetCurrentAudioPositionMs();
	agi::fs::path const& TraceDir() const;

	std::string const& SelectedVideoProvider() const;
	std::string const& SelectedAudioProvider() const;
	std::string const& ActualVideoProvider() const;
	std::string const& ActualVideoDecoder() const;
	std::string const& ActualAudioProviderFactory() const;
	std::string const& ActualAudioProvider() const;

	provider_selection_diagnostics::SelectionReport const& VideoProviderReport() const;
	provider_selection_diagnostics::SelectionReport const& AudioProviderReport() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl;
};

std::string BoolString(bool value);
bool UsedProviderFallback(provider_selection_diagnostics::SelectionReport const& report);
std::string FormatProviderAttempts(provider_selection_diagnostics::SelectionReport const& report);
std::string DescribeProviderFallback(provider_selection_diagnostics::SelectionReport const& report);

}
