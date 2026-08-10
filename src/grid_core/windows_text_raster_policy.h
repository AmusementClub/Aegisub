#pragma once

#include <cstdint>
#include <optional>

namespace aegisub::grid {

enum class RemoteSessionState {
	Local,
	Remote,
	Unknown
};

struct RemoteSessionSignals {
	bool sm_remote_session = false;
	std::optional<std::uint32_t> process_session_id;
	std::optional<std::uint32_t> glass_session_id;
};

RemoteSessionState ClassifyRemoteSession(RemoteSessionSignals const& signals) noexcept;

enum class TextPixelGeometry {
	FlatOrUnknown,
	Rgb,
	Bgr
};

enum class WindowsTextAntialiasPolicy {
	Grayscale,
	ClearType
};

struct WindowsTextRasterConditions {
	RemoteSessionState remote_state = RemoteSessionState::Unknown;
	TextPixelGeometry pixel_geometry = TextPixelGeometry::FlatOrUnknown;
	bool opaque_target = true;
	bool one_to_one_present = true;
	bool clear_type_enabled = false;
	bool local_clear_type_opt_in = false;
};

WindowsTextAntialiasPolicy SelectWindowsTextAntialiasPolicy(
	WindowsTextRasterConditions const& conditions) noexcept;

} // namespace aegisub::grid
