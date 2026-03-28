#pragma once

#include "ui_dispatch.h"

#include <functional>
#include <memory>

#include <wx/event.h>

class wxEvtHandler;

using AsyncVideoProviderEventSink = std::function<void(std::unique_ptr<wxEvent>)>;

AsyncVideoProviderEventSink CreateAsyncVideoProviderWxEventSink(
	wxEvtHandler *parent,
	agi::ui::WeakLifetime event_lifetime);
