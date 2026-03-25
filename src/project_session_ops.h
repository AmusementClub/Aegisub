#pragma once

#include "ui_services.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <string>

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
