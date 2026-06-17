#pragma once

#include "media_open_contract.h"

#include <libaegisub/fs_fwd.h>
#include <libaegisub/vfr.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AsyncVideoProvider;
namespace agi { class NotificationSink; }

namespace aegisub::video_session_ops {

struct OpenedVideoSummary {
	agi::vfr::Framerate timecodes;
	std::vector<int> keyframes;
	std::string warning;
	std::optional<double> display_aspect_ratio_override;
	bool has_audio = false;
	bool has_subtitles = false;
};

using HasSubtitlesProbe = std::function<bool(agi::fs::path const&)>;
using MruRemoveAction = std::function<void(char const*, agi::fs::path const&)>;

OpenedVideoSummary BuildOpenedVideoSummary(AsyncVideoProvider const& provider,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe = {});

bool HandleUnreadableVideoOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru = {});

using CreateVideoProviderAction = std::function<std::unique_ptr<AsyncVideoProvider>()>;
using ProviderSelectionReportSupplier = std::function<aegisub::provider_selection_diagnostics::SelectionReport()>;

struct VideoProviderOpenResult {
	std::unique_ptr<AsyncVideoProvider> provider;
	media_open::MediaOpenResult result;
};

VideoProviderOpenResult OpenVideoProvider(media_open::MediaOpenRequest const& request,
                                          CreateVideoProviderAction const& create_provider,
                                          ProviderSelectionReportSupplier const& provider_report_supplier = {});

std::unique_ptr<AsyncVideoProvider> CreateVideoProviderWithErrorHandling(agi::fs::path const& path,
                                                                         CreateVideoProviderAction const& create_provider,
                                                                         agi::NotificationSink& notification_sink,
                                                                         MruRemoveAction const& remove_mru = {});

struct PostOpenPlan {
	std::optional<double> display_aspect_ratio_override;
	bool auto_load_linked_audio = false;
	int initial_frame = 0;
};

PostOpenPlan PlanPostOpen(OpenedVideoSummary const& summary,
                          bool open_audio_enabled,
                          agi::fs::path const& current_audio_file,
                          agi::fs::path const& current_video_file);

}
