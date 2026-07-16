#include "skia_audio_display_slot.h"

#include "skia_audio_display_contract.h"
#include "skia_audio_display.h"

#include "../../audio_display.h"
#include "../../time_range.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <utility>

#include <libaegisub/log.h>

#include <wx/sizer.h>
#include <wx/window.h>

namespace aegisub::skia::audio {

AudioDisplaySlot::AudioDisplaySlot(
	wxWindow *parent,
	AudioController *controller,
	agi::Context *context,
	std::function<void(wxWindow *)> replacement_callback)
: runtime_requested(ParseRuntimeOptIn(std::getenv("AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY")))
, create_skia_widget(ShouldCreateSkiaWidget(runtime_requested, true))
, parent(parent)
, controller(controller)
, context(context)
, replacement_callback(std::move(replacement_callback))
{
	if (!create_skia_widget) {
		CreateWxDisplay();
		return;
	}

	auto const *injection_value = std::getenv("AEGISUB_SKIA_AUDIO_FAILURE_INJECTION");
	auto const injection = ParseFailureInjection(injection_value ? injection_value : "");
	try {
		skia_display = new SkiaAudioDisplay(parent, controller, context, injection, [this](std::string message) {
			RequestWxFallback(std::move(message));
		});
		active_window = skia_display;
	}
	catch (std::exception const& err) {
		LOG_W("audio/display/skia") << "Skia Audio Display construction failed; using wx: " << err.what();
		create_skia_widget = false;
		CreateWxDisplay();
	}
	catch (...) {
		LOG_W("audio/display/skia") << "Skia Audio Display construction failed with an unknown exception; using wx";
		create_skia_widget = false;
		CreateWxDisplay();
	}
}

AudioDisplaySlot::~AudioDisplaySlot() {
	DeletePendingEvents();
	if (skia_display)
		skia_display->ClearFailureCallback();
}

void AudioDisplaySlot::CreateWxDisplay(wxWindow *replaced_window) {
	auto *new_display = new AudioDisplay(parent, controller, context);
	display = new_display;
	active_window = new_display;
	create_skia_widget = false;

	if (!replaced_window) {
		ApplyStateToWxDisplay();
		return;
	}

	replaced_window->Hide();
	bool replaced = false;
	if (auto *sizer = parent->GetSizer())
		replaced = sizer->Replace(replaced_window, new_display, true);
	if (!replaced)
		new_display->SetSize(replaced_window->GetRect());
	parent->Layout();
	ApplyStateToWxDisplay();
	if (replacement_callback)
		replacement_callback(new_display);
	replaced_window->Destroy();
	skia_display = nullptr;
	fallback_pending = false;
}

void AudioDisplaySlot::RequestWxFallback(std::string message) {
	if (fallback_pending || display)
		return;
	fallback_pending = true;
	LOG_W("audio/display/skia") << message;
	CallAfter([this] {
		if (!display && skia_display) {
			auto *failed_display = skia_display;
			failed_display->ClearFailureCallback();
			CreateWxDisplay(failed_display);
		}
	});

}

void AudioDisplaySlot::ApplyStateToWxDisplay() {
	if (!display)
		return;
	if (zoom_set)
		display->SetZoomLevel(zoom_level);
	if (amplitude_set)
		display->SetAmplitudeScale(amplitude_scale);
	if (sync_requested)
		display->SyncToCurrentAudioProvider();
	if (visible_range_set)
		display->ScrollTimeRangeInView(TimeRange(visible_range_begin, visible_range_end));
	if (pending_scroll_pixels) {
		auto const clamped = std::max<std::int64_t>(
			std::numeric_limits<int>::min(),
			std::min<std::int64_t>(std::numeric_limits<int>::max(), pending_scroll_pixels));
		display->ScrollBy(static_cast<int>(clamped));
	}
}

void AudioDisplaySlot::SyncToCurrentAudioProvider() {
	sync_requested = true;
	if (display)
		display->SyncToCurrentAudioProvider();
	else if (skia_display)
		skia_display->SyncToCurrentAudioProvider();
}

void AudioDisplaySlot::ScrollBy(int pixel_amount) {
	if (display)
		display->ScrollBy(pixel_amount);
	else if (skia_display) {
		skia_display->ScrollBy(pixel_amount);
		pending_scroll_pixels += pixel_amount;
	}
	else
		pending_scroll_pixels += pixel_amount;
}

void AudioDisplaySlot::ScrollBy(int pixel_amount, int mouse_x) {
	if (display)
		display->ScrollBy(pixel_amount, mouse_x);
	else if (skia_display) {
		skia_display->ScrollBy(pixel_amount, mouse_x);
		pending_scroll_pixels += pixel_amount;
	}
	else
		pending_scroll_pixels += pixel_amount;
}

void AudioDisplaySlot::ScrollTimeRangeInView(TimeRange const& range) {
	visible_range_set = true;
	visible_range_begin = range.begin();
	visible_range_end = range.end();
	pending_scroll_pixels = 0;
	if (display)
		display->ScrollTimeRangeInView(range);
	else if (skia_display)
		skia_display->ScrollTimeRangeInView(range);
}

void AudioDisplaySlot::SetZoomLevel(int zoom_level) {
	zoom_set = true;
	this->zoom_level = zoom_level;
	if (display)
		display->SetZoomLevel(zoom_level);
	else if (skia_display)
		skia_display->SetZoomLevel(zoom_level);
}

int AudioDisplaySlot::GetZoomLevel() const {
	return display ? display->GetZoomLevel() : zoom_level;
}

void AudioDisplaySlot::SetAmplitudeScale(float scale) {
	amplitude_set = true;
	amplitude_scale = scale;
	if (display)
		display->SetAmplitudeScale(scale);
	else if (skia_display)
		skia_display->SetAmplitudeScale(scale);
}

}
