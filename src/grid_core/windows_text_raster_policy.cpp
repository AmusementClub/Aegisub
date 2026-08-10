#include "windows_text_raster_policy.h"

namespace aegisub::grid {

RemoteSessionState ClassifyRemoteSession(RemoteSessionSignals const& signals) noexcept {
	if (signals.sm_remote_session)
		return RemoteSessionState::Remote;
	if (!signals.process_session_id || !signals.glass_session_id)
		return RemoteSessionState::Unknown;
	return *signals.process_session_id == *signals.glass_session_id
		? RemoteSessionState::Local
		: RemoteSessionState::Remote;
}

WindowsTextAntialiasPolicy SelectWindowsTextAntialiasPolicy(
	WindowsTextRasterConditions const& conditions) noexcept {
	if (!conditions.local_clear_type_opt_in
		|| conditions.remote_state != RemoteSessionState::Local
		|| conditions.pixel_geometry == TextPixelGeometry::FlatOrUnknown
		|| !conditions.opaque_target
		|| !conditions.one_to_one_present
		|| !conditions.clear_type_enabled)
		return WindowsTextAntialiasPolicy::Grayscale;
	return WindowsTextAntialiasPolicy::ClearType;
}

} // namespace aegisub::grid
