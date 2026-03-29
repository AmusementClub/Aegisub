#pragma once

#include "wx_message_box_ui_services.h"
#include "wx_single_choice_dialog.h"

namespace agi {

// This header is the explicit wx host seam for frame_main dialog-backed UI
// flows such as startup logging, notifications, generic interaction prompts,
// and single-choice requests.

inline void ShowFrameMainStartupLogDialog(wxString const& message) {
	wxMessageBox(message, wxS("Aegisub startup log"));
}

inline InteractionResult SafeFrameMainInteractionResult(InteractionButtons buttons) {
	switch (buttons) {
	case InteractionButtons::Ok:
		return InteractionResult::Ok;
	case InteractionButtons::OkCancel:
		return InteractionResult::Cancel;
	case InteractionButtons::YesNo:
		return InteractionResult::No;
	case InteractionButtons::YesNoCancel:
		return InteractionResult::Cancel;
	}
	return InteractionResult::Cancel;
}

class WxFrameMainNotificationSink final : public NotificationSink {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

	void Show(std::string const& title, std::string const& message, int flags) {
		ui::MainInvokeIfAlive(lifetime, [parent = parent, title, message, flags] {
			wxMessageBox(to_wx(message), to_wx(title), flags | wxCENTER, parent);
		});
	}

public:
	WxFrameMainNotificationSink(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	void ShowInfo(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_INFORMATION);
	}

	void ShowError(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_ERROR);
	}

	void ShowWarning(std::string const& title, std::string const& message) override {
		Show(title, message, wxOK | wxICON_WARNING);
	}
};

class WxFrameMainInteractionSink final : public InteractionSink {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainInteractionSink(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	InteractionResult Request(InteractionRequest const& request) override {
		return ui::MainInvoke([parent = parent, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return SafeFrameMainInteractionResult(request.buttons);

			return FromWxMessageBoxResult(wxMessageBox(
				to_wx(request.message),
				to_wx(request.title),
				ToWxMessageBoxFlags(request.buttons, request.icon),
				parent));
		});
	}
};

class WxFrameMainSingleChoiceInteractionSink final : public SingleChoiceInteractionSink {
	wxWindow *parent = nullptr;
	ui::WeakLifetime lifetime;

public:
	WxFrameMainSingleChoiceInteractionSink(wxWindow *parent, ui::WeakLifetime lifetime)
	: parent(parent)
	, lifetime(std::move(lifetime)) {
	}

	std::optional<int> RequestSingleChoice(SingleChoiceInteractionRequest const& request) override {
		return ui::MainInvoke([parent = parent, lifetime = lifetime, request] {
			if (!lifetime.lock())
				return std::optional<int>();
			return ShowSingleChoiceDialog(parent, request);
		});
	}
};

inline std::shared_ptr<NotificationSink> MakeFrameMainNotificationSink(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainNotificationSink>(parent, std::move(lifetime));
}

inline std::shared_ptr<InteractionSink> MakeFrameMainInteractionSink(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainInteractionSink>(parent, std::move(lifetime));
}

inline std::shared_ptr<SingleChoiceInteractionSink> MakeFrameMainSingleChoiceInteractionSink(wxWindow *parent, ui::WeakLifetime lifetime) {
	return std::make_shared<WxFrameMainSingleChoiceInteractionSink>(parent, std::move(lifetime));
}

}
