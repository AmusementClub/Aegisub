#pragma once

#include "value_event.h"

#include <libaegisub/dispatch.h>

#include <wx/event.h>

wxDECLARE_EVENT(EVT_CALL_THUNK, ValueEvent<agi::dispatch::Thunk>);
