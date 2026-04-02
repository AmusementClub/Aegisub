#pragma once

#include "ui_timer.h"

#include <memory>

std::shared_ptr<UiTimerHost> CreateThreadedUiTimerHost();
