#include "project_session_ops.h"

#include "format.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/format_path.h>

#include <exception>

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
	if (!create_provider)
		return {};

	try {
		return create_provider();
	}
	catch (agi::UserCancelException const&) {
		return {};
	}
	catch (agi::fs::FileNotFound const& err) {
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(kErrorLoadingFileTitle, agi::format(_("The audio file was not found: %s"), err.GetMessage()));
	}
	catch (agi::AudioDataNotFound const& err) {
		remove_mru_if_requested("Audio", path, remove_mru);
		if (quiet) {
			if (on_quiet_no_audio)
				on_quiet_no_audio(err.GetMessage());
		}
		else {
			notification_sink.ShowError(
				kErrorLoadingFileTitle,
				agi::format(_("None of the available audio providers recognised the selected file as containing audio data.\n\nThe following providers were tried:\n%s"), err.GetMessage()));
		}
	}
	catch (agi::AudioProviderError const& err) {
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(
			kErrorLoadingFileTitle,
			agi::format(_("None of the available audio providers have a codec available to handle the selected file.\n\nThe following providers were tried:\n%s"), err.GetMessage()));
	}
	catch (agi::Exception const& err) {
		remove_mru_if_requested("Audio", path, remove_mru);
		notification_sink.ShowError(kErrorLoadingFileTitle, err.GetMessage());
	}

	return {};
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
