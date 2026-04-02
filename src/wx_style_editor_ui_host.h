#pragma once

#include "wx_message_box_ui_services.h"

namespace agi { struct Context; }

namespace agi {

// This header is the explicit wx host seam for style editor flows which may
// run without a project context and therefore need window-backed fallback
// notification and interaction sinks.

inline std::shared_ptr<NotificationSink> ResolveStyleEditorNotificationSink(Context *context, wxWindow *parent) {
	if (context)
		return context->GetNotificationSink();
	return MakeWindowNotificationSink(parent);
}

inline std::shared_ptr<InteractionSink> ResolveStyleEditorInteractionSink(Context *context, wxWindow *parent) {
	if (context)
		return context->GetInteractionSink();
	return MakeWindowInteractionSink(parent);
}

}
