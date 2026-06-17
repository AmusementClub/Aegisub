#include "video_session_ops.h"

#include "async_video_provider.h"
#include "include/aegisub/video_provider.h"
#include "ui_services.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <utility>

namespace aegisub::video_session_ops {

OpenedVideoSummary BuildOpenedVideoSummary(AsyncVideoProvider const& provider,
                                           agi::fs::path const& path,
                                           HasSubtitlesProbe const& has_subtitles_probe) {
	OpenedVideoSummary summary;
	summary.timecodes = provider.GetFPS();
	summary.keyframes = provider.GetKeyFrames();
	summary.warning = provider.GetWarning();
	summary.has_audio = provider.HasAudio();

	auto dar = provider.GetDAR();
	if (dar > 0.0)
		summary.display_aspect_ratio_override = dar;

	if (has_subtitles_probe && agi::fs::HasExtension(path, "mkv"))
		summary.has_subtitles = has_subtitles_probe(path);

	return summary;
}

bool HandleUnreadableVideoOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru) {
	if (remove_mru)
		remove_mru("Video", path);
	notification_sink.ShowError("Error loading file", access_error);
	return false;
}

std::unique_ptr<AsyncVideoProvider> CreateVideoProviderWithErrorHandling(agi::fs::path const& path,
                                                                         CreateVideoProviderAction const& create_provider,
                                                                         agi::NotificationSink& notification_sink,
                                                                         MruRemoveAction const& remove_mru) {
	media_open::MediaOpenRequest request;
	request.kind = media_open::MediaKind::Video;
	request.path = path;
	auto opened = OpenVideoProvider(request, create_provider);
	if (opened.provider)
		return std::move(opened.provider);

	if (opened.result.status == media_open::OpenStatus::Cancelled)
		return {};

	if (opened.result.status == media_open::OpenStatus::FileNotFound || opened.result.status == media_open::OpenStatus::FileSystemError) {
		if (remove_mru)
			remove_mru("Video", path);
	}
	if (!opened.result.error.empty())
		notification_sink.ShowError("Error loading file", opened.result.error);

	return {};
}

VideoProviderOpenResult OpenVideoProvider(media_open::MediaOpenRequest const& request,
                                          CreateVideoProviderAction const& create_provider,
                                          ProviderSelectionReportSupplier const& provider_report_supplier) {
	auto current_report = [&] {
		return provider_report_supplier ? provider_report_supplier() : provider_selection_diagnostics::SelectionReport{};
	};

	VideoProviderOpenResult opened;
	opened.result.kind = request.kind;
	opened.result.provider_report = current_report();

	if (!create_provider)
		return opened;

	try {
		opened.provider = create_provider();
		if (!opened.provider) {
			opened.result = media_open::Failed(
				request.kind,
				media_open::OpenStatus::Error,
				"video provider factory returned null",
				current_report());
			return opened;
		}

		auto report = current_report();
		auto selected_provider = report.selected_provider.empty() ? opened.provider->GetDecoderName() : report.selected_provider;
		opened.result = media_open::Opened(
			request.kind,
			selected_provider,
			std::move(report));
		opened.result.decoder_name = opened.provider->GetDecoderName();
		opened.result.warning = opened.provider->GetWarning();
		return opened;
	}
	catch (agi::UserCancelException const&) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::Cancelled,
			{},
			current_report());
	}
	catch (agi::fs::FileNotFound const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::FileNotFound,
			err.GetMessage(),
			current_report());
	}
	catch (agi::fs::FileSystemError const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::FileSystemError,
			err.GetMessage(),
			current_report());
	}
	catch (VideoNotSupported const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::NotSupported,
			err.GetMessage(),
			current_report());
	}
	catch (VideoProviderError const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::Error,
			err.GetMessage(),
			current_report());
	}

	return opened;
}

PostOpenPlan PlanPostOpen(OpenedVideoSummary const& summary,
                          bool open_audio_enabled,
                          agi::fs::path const& current_audio_file,
                          agi::fs::path const& current_video_file) {
	PostOpenPlan plan;
	plan.display_aspect_ratio_override = summary.display_aspect_ratio_override;
	plan.auto_load_linked_audio =
		open_audio_enabled &&
		summary.has_audio &&
		current_audio_file != current_video_file;
	return plan;
}

}
