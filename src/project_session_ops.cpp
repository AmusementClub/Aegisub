#include "project_session_ops.h"

#include "format.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/format_path.h>

#include <exception>
#include <utility>

namespace {

constexpr char kErrorLoadingFileTitle[] = "Error loading file";

void remove_mru_if_requested(char const* category,
                             agi::fs::path const& path,
                             aegisub::project_session_ops::MruRemoveAction const& remove_mru) {
	if (remove_mru)
		remove_mru(category, path);
}

void show_missing_subtitle_path_error(agi::fs::path const& path, agi::NotificationSink& notification_sink) {
	notification_sink.ShowError(kErrorLoadingFileTitle, agi::format("%s not found.", path));
}

template<typename SaveAction>
bool save_with_error_handling(agi::fs::path const& path,
                              char const* category,
                              std::string const& error_title,
                              SaveAction const& save_action,
                              agi::NotificationSink& notification_sink,
                              aegisub::project_session_ops::MruAddAction const& add_mru) {
	if (path.empty())
		return false;

	try {
		save_action();
		if (add_mru)
			add_mru(category, path);
		return true;
	}
	catch (agi::Exception const& err) {
		notification_sink.ShowError(error_title, err.GetMessage());
	}
	catch (std::exception const& err) {
		notification_sink.ShowError(error_title, err.what());
	}
	catch (...) {
		notification_sink.ShowError(error_title, "Unknown error");
	}

	return false;
}

}

namespace aegisub::project_session_ops {

std::optional<std::string> ResolveSubtitleEncoding(agi::fs::path const& path,
                                                   std::string const& encoding,
                                                   DetectSubtitleEncodingAction const& detect_encoding,
                                                   agi::NotificationSink& notification_sink,
                                                   MruRemoveAction const& remove_mru) {
	if (!encoding.empty())
		return encoding;
	if (!detect_encoding)
		return encoding;

	try {
		return detect_encoding();
	}
	catch (agi::UserCancelException const&) {
		return std::nullopt;
	}
	catch (agi::fs::FileNotFound const&) {
		remove_mru_if_requested("Subtitle", path, remove_mru);
		show_missing_subtitle_path_error(path, notification_sink);
		return std::nullopt;
	}
	catch (agi::fs::FileSystemError const& err) {
		remove_mru_if_requested("Subtitle", path, remove_mru);
		notification_sink.ShowError(kErrorLoadingFileTitle, err.GetMessage());
		return std::nullopt;
	}
}

bool LoadSubtitlesWithErrorHandling(agi::fs::path const& path,
                                    LoadSubtitleFileAction const& load_subtitles,
                                    agi::NotificationSink& notification_sink,
                                    MruRemoveAction const& remove_mru) {
	if (!load_subtitles)
		return false;

	try {
		load_subtitles();
		return true;
	}
	catch (agi::UserCancelException const&) {
		return false;
	}
	catch (agi::fs::FileNotFound const&) {
		remove_mru_if_requested("Subtitle", path, remove_mru);
		show_missing_subtitle_path_error(path, notification_sink);
	}
	catch (agi::Exception const& err) {
		notification_sink.ShowError(kErrorLoadingFileTitle, err.GetMessage());
	}
	catch (std::exception const& err) {
		notification_sink.ShowError(kErrorLoadingFileTitle, err.what());
	}
	catch (...) {
		notification_sink.ShowError(kErrorLoadingFileTitle, "Unknown error");
	}

	return false;
}

bool HandleUnreadableAudioOpenPath(agi::fs::path const& path,
                                   std::string const& access_error,
                                   agi::NotificationSink& notification_sink,
                                   MruRemoveAction const& remove_mru) {
	remove_mru_if_requested("Audio", path, remove_mru);
	notification_sink.ShowError(kErrorLoadingFileTitle, agi::format(_("The audio file was not found: %s"), access_error));
	return false;
}

std::unique_ptr<agi::AudioProvider> CreateAudioProviderWithErrorHandling(agi::fs::path const& path,
                                                                         bool quiet,
                                                                         CreateAudioProviderAction const& create_provider,
                                                                         agi::NotificationSink& notification_sink,
                                                                         QuietAudioDataMissingAction const& on_quiet_no_audio,
                                                                         MruRemoveAction const& remove_mru) {
	media_open::MediaOpenRequest request;
	request.kind = media_open::MediaKind::Audio;
	request.path = path;
	auto opened = OpenAudioProvider(request, create_provider);
	if (opened.provider)
		return std::move(opened.provider);

	switch (opened.result.status) {
	case media_open::OpenStatus::Cancelled:
	case media_open::OpenStatus::NotStarted:
		return {};

	case media_open::OpenStatus::FileNotFound:
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(kErrorLoadingFileTitle, agi::format(_("The audio file was not found: %s"), opened.result.error));
		break;

	case media_open::OpenStatus::NoMedia:
		remove_mru_if_requested("Audio", path, remove_mru);
		if (quiet) {
			if (on_quiet_no_audio)
				on_quiet_no_audio(opened.result.error);
		}
		else {
			notification_sink.ShowError(
				kErrorLoadingFileTitle,
				agi::format(_("None of the available audio providers recognised the selected file as containing audio data.\n\nThe following providers were tried:\n%s"), opened.result.error));
		}
		break;

	case media_open::OpenStatus::NotSupported:
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(
			kErrorLoadingFileTitle,
			agi::format(_("None of the available audio providers have a codec available to handle the selected file.\n\nThe following providers were tried:\n%s"), opened.result.error));
		break;

	case media_open::OpenStatus::FileSystemError:
	case media_open::OpenStatus::Error:
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(kErrorLoadingFileTitle, opened.result.error);
		break;

	case media_open::OpenStatus::Opened:
		break;
	}

	return {};
}

AudioProviderOpenResult OpenAudioProvider(media_open::MediaOpenRequest const& request,
                                          CreateAudioProviderAction const& create_provider,
                                          AudioProviderSelectionReportSupplier const& provider_report_supplier) {
	auto current_report = [&] {
		return provider_report_supplier ? provider_report_supplier() : provider_selection_diagnostics::SelectionReport{};
	};

	AudioProviderOpenResult opened;
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
				"audio provider factory returned null",
				current_report());
			return opened;
		}

		auto report = current_report();
		auto memory_stats = opened.provider->GetMemoryStats();
		auto selected_provider = report.selected_provider.empty() ? memory_stats.provider_name : report.selected_provider;
		opened.result = media_open::Opened(
			request.kind,
			selected_provider,
			std::move(report));
		opened.result.decoder_name = std::move(memory_stats.provider_name);
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
	catch (agi::AudioDataNotFound const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::NoMedia,
			err.GetMessage(),
			current_report());
	}
	catch (agi::AudioProviderError const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::NotSupported,
			err.GetMessage(),
			current_report());
	}
	catch (agi::Exception const& err) {
		opened.result = media_open::Failed(
			request.kind,
			media_open::OpenStatus::Error,
			err.GetMessage(),
			current_report());
	}

	return opened;
}

SubtitleSessionTarget ResolveSubtitleSessionTarget(bool open_in_new_session, bool close_cancelled) {
	if (open_in_new_session)
		return SubtitleSessionTarget::NewSession;

	return close_cancelled ? SubtitleSessionTarget::Cancel : SubtitleSessionTarget::CurrentSession;
}

bool ExecuteSubtitleSessionAction(SubtitleSessionTarget target,
                                  SessionAction const& current_session_action,
                                  SessionAction const& new_session_action) {
	switch (target) {
	case SubtitleSessionTarget::Cancel:
		return false;
	case SubtitleSessionTarget::CurrentSession:
		if (!current_session_action)
			return false;
		current_session_action();
		return true;
	case SubtitleSessionTarget::NewSession:
		if (!new_session_action)
			return false;
		new_session_action();
		return true;
	}

	return false;
}

bool ExecuteSubtitleLoad(SubtitleSessionTarget target,
                         agi::fs::path const& path,
                         SubtitleLoadAction const& current_session_action,
                         SubtitleLoadAction const& new_session_action,
                         std::string const& encoding,
                         bool load_linked) {
	if (path.empty())
		return false;

	switch (target) {
	case SubtitleSessionTarget::Cancel:
		return false;
	case SubtitleSessionTarget::CurrentSession:
		if (!current_session_action)
			return false;
		current_session_action(path, encoding, load_linked);
		return true;
	case SubtitleSessionTarget::NewSession:
		if (!new_session_action)
			return false;
		new_session_action(path, encoding, load_linked);
		return true;
	}

	return false;
}

bool SaveTimecodesToPath(agi::fs::path const& path,
                         int frame_count,
                         TimecodesSaveAction const& save_action,
                         agi::NotificationSink& notification_sink,
                         MruAddAction const& add_mru) {
	if (!save_action)
		return false;

	return save_with_error_handling(
		path,
		"Timecodes",
		"Error saving timecodes",
		[&] { save_action(path, frame_count); },
		notification_sink,
		add_mru);
}

bool SaveKeyframesToPath(agi::fs::path const& path,
                         KeyframesSaveAction const& save_action,
                         agi::NotificationSink& notification_sink,
                         MruAddAction const& add_mru) {
	if (!save_action)
		return false;

	return save_with_error_handling(
		path,
		"Keyframes",
		"Error saving keyframes",
		[&] { save_action(path); },
		notification_sink,
		add_mru);
}

}
