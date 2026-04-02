#pragma once

#include "ui_services.h"

#include <memory>

struct RuntimeBootstrapUiHost {
	agi::NotificationSink *notification_sink = nullptr;
	agi::InteractionSink *interaction_sink = nullptr;
	std::shared_ptr<agi::SingleChoiceInteractionSink> single_choice_sink;
};

inline void ShowBootstrapUiInfo(RuntimeBootstrapUiHost const& host, std::string const& title, std::string const& message) {
	if (host.notification_sink)
		host.notification_sink->ShowInfo(title, message);
}

inline void ShowBootstrapUiWarning(RuntimeBootstrapUiHost const& host, std::string const& title, std::string const& message) {
	if (host.notification_sink)
		host.notification_sink->ShowWarning(title, message);
}

inline void ShowBootstrapUiError(RuntimeBootstrapUiHost const& host, std::string const& title, std::string const& message) {
	if (host.notification_sink)
		host.notification_sink->ShowError(title, message);
}

inline agi::InteractionResult RequestBootstrapUiInteraction(RuntimeBootstrapUiHost const& host, agi::InteractionRequest const& request) {
	if (host.interaction_sink)
		return host.interaction_sink->Request(request);
	return agi::DefaultInteractionResult(request.buttons);
}

inline std::optional<int> RequestBootstrapUiSingleChoice(RuntimeBootstrapUiHost const& host, agi::SingleChoiceInteractionRequest const& request) {
	if (host.single_choice_sink)
		return host.single_choice_sink->RequestSingleChoice(request);
	return std::nullopt;
}
