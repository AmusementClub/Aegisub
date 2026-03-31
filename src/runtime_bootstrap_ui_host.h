#pragma once

#include "ui_services.h"

#include <memory>

struct RuntimeBootstrapUiHost {
	agi::NotificationSink *notification_sink = nullptr;
	agi::InteractionSink *interaction_sink = nullptr;
	std::shared_ptr<agi::SingleChoiceInteractionSink> single_choice_sink;
};
