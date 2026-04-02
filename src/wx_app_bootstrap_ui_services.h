#pragma once

#include "wx_message_box_ui_services.h"
#include "wx_single_choice_dialog.h"

namespace agi {

// This header is the explicit wx adapter seam for app bootstrap UI flows
// which need message-box notifications, generic interaction prompts, and
// startup single-choice dialogs before a project context exists.

inline NotificationSink& AppBootstrapNotificationSink() {
	static WxMessageBoxNotificationSink sink(nullptr);
	return sink;
}

inline InteractionSink& AppBootstrapInteractionSink() {
	static WxMessageBoxInteractionSink sink(nullptr);
	return sink;
}

inline std::shared_ptr<SingleChoiceInteractionSink> MakeAppBootstrapSingleChoiceInteractionSink() {
	return MakeWindowSingleChoiceInteractionSink();
}

}
