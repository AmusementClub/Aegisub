#pragma once

#include "ui_services.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agi { class AudioProvider; }

namespace aegisub::project_session_ops {

enum class SubtitleSessionTarget {
	Cancel,
	CurrentSession,
	NewSession
};

SubtitleSessionTarget ResolveSubtitleSessionTarget(bool open_in_new_session, bool close_cancelled);

using SessionAction = std::function<void()>;
bool ExecuteSubtitleSessionAction(SubtitleSessionTarget target,
                                  SessionAction const& current_session_action,
                                  SessionAction const& new_session_action);

using SubtitleLoadAction = std::function<void(agi::fs::path const&, std::string const&, bool)>;
bool ExecuteSubtitleLoad(SubtitleSessionTarget target,
                         agi::fs::path const& path,
                         SubtitleLoadAction const& current_session_action,
                         SubtitleLoadAction const& new_session_action,
                         std::string const& encoding = "",
                         bool load_linked = true);

using MruAddAction = std::function<void(char const*, agi::fs::path const&)>;
using MruRemoveAction = std::function<void(char const*, agi::fs::path const&)>;

using DetectSubtitleEncodingAction = std::function<std::string()>;
std::optional<std::string> ResolveSubtitleEncoding(agi::fs::path const& path,
                                                   std::string const& encoding,
                                                   DetectSubtitleEncodingAction const& detect_encoding,
                                                   agi::NotificationSink& notification_sink,
                                                   MruRemoveAction const& remove_mru = {});

using LoadSubtitleFileAction = std::function<void()>;
bool LoadSubtitlesWithErrorHandling(agi::fs::path const& path,
                                    LoadSubtitleFileAction const& load_subtitles,
                                    agi::NotificationSink& notification_sink,
                                    MruRemoveAction const& remove_mru = {});

bool HandleUnreadableAudioOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru = {});

using CreateAudioProviderAction = std::function<std::unique_ptr<agi::AudioProvider>()>;
using QuietAudioDataMissingAction = std::function<void(std::string const&)>;
std::unique_ptr<agi::AudioProvider> CreateAudioProviderWithErrorHandling(agi::fs::path const& path,
                                                                         bool quiet,
                                                                         CreateAudioProviderAction const& create_provider,
                                                                         agi::NotificationSink& notification_sink,
                                                                         QuietAudioDataMissingAction const& on_quiet_no_audio = {},
                                                                         MruRemoveAction const& remove_mru = {});

using TimecodesSaveAction = std::function<void(agi::fs::path const&, int)>;
bool SaveTimecodesToPath(agi::fs::path const& path,
                         int frame_count,
                         TimecodesSaveAction const& save_action,
                         agi::NotificationSink& notification_sink,
                         MruAddAction const& add_mru = {});

using KeyframesSaveAction = std::function<void(agi::fs::path const&)>;
bool SaveKeyframesToPath(agi::fs::path const& path,
                         KeyframesSaveAction const& save_action,
                         agi::NotificationSink& notification_sink,
                         MruAddAction const& add_mru = {});

}
