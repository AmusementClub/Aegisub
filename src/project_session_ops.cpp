#include "project_session_ops.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <exception>

namespace {

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
