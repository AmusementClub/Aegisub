#pragma once

#include "media_open_contract.h"
#include "video_session_core_ops.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>

class AsyncVideoProvider;
namespace agi { class NotificationSink; }

namespace aegisub::video_session_ops {

OpenedVideoSummary BuildOpenedVideoSummary(AsyncVideoProvider const& provider,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe = {});

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

}
