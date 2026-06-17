#pragma once

#include "ui_services.h"
#include "video_property_update.h"

agi::SingleChoiceInteractionRequest BuildVideoResolutionMismatchRequest(VideoPropertyUpdateInput const& input,
                                                                        bool aspect_ratio_changed,
                                                                        int last_choice);
agi::SingleChoiceInteractionRequest BuildLayoutResRequest(VideoPropertyUpdateInput const& input);
