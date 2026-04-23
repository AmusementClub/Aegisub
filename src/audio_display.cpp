// Copyright (c) 2005, Rodrigo Braz Monteiro
// Copyright (c) 2009-2010, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "audio_display.h"

#include "audio_controller.h"
#include "audio_display_invalidation_planner.h"
#ifdef WITH_SKIA
#include "audio_display_skia_renderer.h"
#include "audio_display_skia_target.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#endif
#include "audio_provider_factory.h"
#include "audio_renderer.h"
#include "audio_renderer_spectrum.h"
#include "audio_renderer_waveform.h"
#include "audio_tile_compositor.h"
#include "audio_timing.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "options.h"
#include "project.h"
#include "time_display_mode.h"
#include "ui_dispatch.h"
#include "utils.h"
#include "video_controller.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>
#endif

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/mousestate.h>
#include <wx/utils.h>

wxDEFINE_EVENT(EVT_AUDIO_DISPLAY_REBUILD_HOST, wxCommandEvent);

/// @class AudioDisplayInteractionObject
/// @brief Interface for objects on the audio display that can respond to mouse events
class AudioDisplayInteractionObject {
public:
	/// @brief The user is interacting with the object using the mouse
	/// @param event Mouse event data
	/// @return True to take mouse capture, false to release mouse capture
	///
	/// Assuming no object has the mouse capture, the audio display uses other methods
	/// in the object implementing this interface to determine whether a mouse event
	/// should go to the object. If the mouse event goes to the object, this method
	/// is called.
	///
	/// If this method returns true, the audio display takes the mouse capture and
	/// stores a pointer to the AudioDisplayInteractionObject interface for the object
	/// and redirects the next mouse event to that object.
	///
	/// If the object that has the mouse capture returns false from this method, the
	/// capture is released and regular processing is done for the next event.
	///
	/// If the object does not have mouse capture and returns false from this method,
	/// no capture is taken or released and regular processing is done for the next
	/// mouse event.
	virtual bool OnMouseEvent(wxMouseEvent &event) = 0;

	/// @brief Destructor
	///
	/// Empty virtual destructor for the cases that need it.
	virtual ~AudioDisplayInteractionObject() = default;
};

namespace {
AudioDisplayInvalidationPlanner::Rect ToPlannerRect(wxRect const& rect) {
	if (rect.IsEmpty())
		return { };
	return { rect.x, rect.y, rect.width, rect.height };
}

SubtitleTimeDisplayMode ReadAudioCursorTimeDisplayMode() {
	int64_t mode = OPT_GET("Audio/Display/Draw/Cursor Time Format")->GetInt();
	return mode == 1 ? SubtitleTimeDisplayMode::Exact : SubtitleTimeDisplayMode::Ass;
}

wxString TryReadFontFace(std::string const& opt_prefix) {
	try {
		return FontFace(opt_prefix);
	}
	catch (agi::InternalError const&) {
		return wxString();
	}
}

wxString ResolveAudioLabelFontFace() {
	auto face = TryReadFontFace("Audio/Karaoke");
	if (!face.empty())
		return face;
	return TryReadFontFace("Audio/Track Cursor");
}

wxRect ToWxRect(AudioDisplayInvalidationPlanner::Rect const& rect) {
	return wxRect(rect.x, rect.y, rect.width, rect.height);
}

/// Convert wxRect to AudioDisplayRect (render model boundary conversion).
AudioDisplayRect ToDisplayRect(wxRect const& r) {
	return { r.x, r.y, r.width, r.height };
}

/// Convert wxColour to packed BGRA uint32_t (render model boundary conversion).
uint32_t PackWxColour(wxColour const& c) {
	return AudioDisplayPackColour(c.Red(), c.Green(), c.Blue(), c.Alpha());
}

/// Convert packed BGRA uint32_t to wxColour (fallback wxDC render path).
wxColour UnpackToWxColour(uint32_t c) {
	return wxColour(AudioDisplayColourR(c), AudioDisplayColourG(c), AudioDisplayColourB(c), AudioDisplayColourA(c));
}

/// Convert AudioDisplayRect to wxRect (fallback wxDC render path).
wxRect ToWxRect(AudioDisplayRect const& r) {
	return wxRect(r.x, r.y, r.width, r.height);
}

#ifdef WITH_SKIA
SkColor ToSkColorFromPacked(uint32_t colour) {
	return SkColorSetARGB(
		AudioDisplayColourA(colour),
		AudioDisplayColourR(colour),
		AudioDisplayColourG(colour),
		AudioDisplayColourB(colour));
}
#endif

bool ReadEnvFlagDefaultOn(char const *name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return true;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

std::string ReadEnvString(char const *name) {
	auto const* value = std::getenv(name);
	return (value && *value) ? std::string(value) : std::string();
}

int ReadEnvInt(char const *name, int default_value, int min_value, int max_value) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return default_value;

	char *end = nullptr;
	long parsed = std::strtol(value, &end, 10);
	if (!end || end == value)
		return default_value;

	parsed = std::clamp(parsed, static_cast<long>(min_value), static_cast<long>(max_value));
	return static_cast<int>(parsed);
}

bool IsSkiaAudioRenderBackendEnabled() {
#ifdef WITH_SKIA
	// Render Backend preference: 0=Auto, 1=GPU (Skia), 2=Bitmap (Skia), 3=Bitmap (wxDC/GDI)
	int64_t backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
	return backend != 3; // anything except explicit wxDC bitmap keeps Skia enabled
#else
	return false;
#endif
}

bool IsFalseLikeEnvValue(char const *value) {
	if (!value || !*value)
		return true;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first == '0' || first == 'f' || first == 'n';
}

bool IsTrueLikeEnvValue(std::string const& value) {
	if (value.empty())
		return false;

	auto lower = value;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool IsAudioDebugLogEnabled() {
	static bool enabled = false;
	static bool initialized = false;
	if (initialized)
		return enabled;
	initialized = true;

	auto const* value = std::getenv("AEGISUB_AUDIO_DEBUG_LOG");
	if (!value || !*value)
		return enabled;

	std::string const setting(value);
	enabled = IsTrueLikeEnvValue(setting) || !IsFalseLikeEnvValue(value);
	return enabled;
}

bool IsAudioCursorDebugLogEnabled() {
	static bool enabled = false;
	static bool initialized = false;
	if (initialized)
		return enabled;
	initialized = true;

	auto const* value = std::getenv("AEGISUB_AUDIO_CURSOR_DEBUG_LOG");
	if (!value || !*value)
		return enabled;

	std::string const setting(value);
	enabled = IsTrueLikeEnvValue(setting) || !IsFalseLikeEnvValue(value);
	return enabled;
}

void MaybeAnnounceAudioCursorDebugLog() {
	static bool announced = false;
	if (announced || !IsAudioCursorDebugLogEnabled())
		return;

	announced = true;
	auto const session_log = agi::log::GetSessionLogFile();
	if (!session_log.empty())
		LOG_I("audio/debug/cursor") << "enabled=1 source=AEGISUB_AUDIO_CURSOR_DEBUG_LOG session_log_file=" << agi::fs::PathToString(session_log);
	else
		LOG_I("audio/debug/cursor") << "enabled=1 source=AEGISUB_AUDIO_CURSOR_DEBUG_LOG session_log_file=<none>";
}

void AppendRect(std::ostringstream& out, char const* key, wxRect const& rect) {
	out << ' ' << key << '='
		<< rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height;
}

template <typename Fn>
void LogAudioCursorDebug(char const* event, Fn&& fn) {
	if (!IsAudioCursorDebugLogEnabled())
		return;

	MaybeAnnounceAudioCursorDebugLog();
	std::ostringstream out;
	out << "event=" << event;
	fn(out);
	LOG_D("audio/debug/cursor") << out.str();
}

void AppendQuoted(std::ostringstream& out, char const* key, std::string const& value) {
	out << ' ' << key << '=' << std::quoted(value);
}

std::string PathForLog(agi::fs::path const& path) {
	return path.empty() ? std::string("<none>") : agi::fs::PathToString(path);
}

std::string ResolveTokenPathForLog(agi::Context const* context, std::string const& token_path) {
	if (!context || !context->path)
		return "<unavailable>";

	try {
		return PathForLog(context->path->Decode(token_path));
	}
	catch (agi::Exception const& err) {
		return std::string("<error: ") + err.GetMessage() + ">";
	}
	catch (...) {
		return "<error>";
	}
}

std::string ResolveAudioHdCachePathForLog(agi::Context const* context) {
	auto configured = OPT_GET("Audio/Cache/HD/Location")->GetString();
	if (configured == "default")
		configured = "?temp";

	if (!context || !context->path)
		return configured;

	try {
		return PathForLog(context->path->MakeAbsolute(context->path->Decode(configured), "?temp"));
	}
	catch (agi::Exception const& err) {
		return std::string("<error: ") + err.GetMessage() + ">";
	}
	catch (...) {
		return "<error>";
	}
}

char const* AudioCacheTypeName(int64_t value) {
	switch (value) {
		case 0: return "none";
		case 1: return "ram";
		case 2: return "hard_disk";
		default: return "unknown";
	}
}

char const* RenderBackendName(int64_t value) {
	switch (value) {
		case 0: return "auto";
		case 1: return "gpu";
		case 2: return "bitmap_skia";
		case 3: return "bitmap_wxdc";
		default: return "unknown";
	}
}

#ifdef WITH_SKIA
char const* SkiaBackendTypeName(AudioDisplaySkiaBackendType value) {
	switch (value) {
		case AudioDisplaySkiaBackendType::Bitmap: return "bitmap";
		case AudioDisplaySkiaBackendType::Gpu: return "gpu";
		default: return "unknown";
	}
}

std::string ToLowerAscii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

enum class AudioDisplaySkiaHostPreference {
	Auto = 0,
	Bitmap,
	Gpu,
};

char const* AudioDisplaySkiaHostPreferenceName(AudioDisplaySkiaHostPreference value) {
	switch (value) {
		case AudioDisplaySkiaHostPreference::Auto: return "auto";
		case AudioDisplaySkiaHostPreference::Bitmap: return "bitmap";
		case AudioDisplaySkiaHostPreference::Gpu: return "gpu";
		default: return "unknown";
	}
}

AudioDisplaySkiaHostPreference ReadAudioDisplaySkiaHostPreference() {
	auto const value = ToLowerAscii(ReadEnvString("AEGISUB_AUDIO_SKIA_HOST"));
	if (value == "bitmap")
		return AudioDisplaySkiaHostPreference::Bitmap;
	if (value == "gpu"
		|| value == "direct"
		|| value == "direct-gpu"
		|| value == "direct_gpu"
		|| value == "offscreen"
		|| value == "offscreen-gpu"
		|| value == "offscreen_gpu")
		return AudioDisplaySkiaHostPreference::Gpu;
	return AudioDisplaySkiaHostPreference::Auto;
}

bool audio_display_auto_wxdc_forced = false;
std::string audio_display_auto_downgrade_reason;

struct RemoteSessionInfo {
	bool remote = false;
	bool wts_query_ok = false;
	unsigned int wts_protocol = 0;
	int sm_remote_session = 0;
	int sm_remote_control = 0;
	std::string session_name;
};

RemoteSessionInfo QueryRemoteSessionInfo() {
	RemoteSessionInfo info;
#ifdef _WIN32
	info.sm_remote_session = GetSystemMetrics(SM_REMOTESESSION);
	info.sm_remote_control = GetSystemMetrics(SM_REMOTECONTROL);
	if (info.sm_remote_session || info.sm_remote_control)
		info.remote = true;

	LPWSTR buffer = nullptr;
	DWORD bytes_returned = 0;
	if (WTSQuerySessionInformationW(
		WTS_CURRENT_SERVER_HANDLE,
		WTS_CURRENT_SESSION,
		WTSClientProtocolType,
		&buffer,
		&bytes_returned)) {
		info.wts_query_ok = true;
		if (buffer && bytes_returned >= sizeof(USHORT)) {
			info.wts_protocol = *reinterpret_cast<USHORT const*>(buffer);
			if (info.wts_protocol != 0)
				info.remote = true;
		}
		if (buffer)
			WTSFreeMemory(buffer);
	}
#endif

	info.session_name = ReadEnvString("SESSIONNAME");
	auto const session = ToLowerAscii(info.session_name);
	if (session.find("rdp-tcp") != std::string::npos
		|| session.find("rdp-") != std::string::npos)
		info.remote = true;

	return info;
}

bool IsAutoWxDcForced() {
	return audio_display_auto_wxdc_forced;
}

std::string GetAutoDowngradeReason() {
	return audio_display_auto_downgrade_reason;
}

void MarkAutoDowngradedToWxDc(std::string reason) {
	if (reason.empty())
		reason = "gpu_backend_failed";

	audio_display_auto_wxdc_forced = true;
	audio_display_auto_downgrade_reason = std::move(reason);
}

void LogSkiaGpuDiagnostics(AudioDisplaySkiaBackend const* backend, AudioDisplaySkiaGpuDiagnostics const& info) {
	std::ostringstream log;
	log << "backend=" << (backend ? SkiaBackendTypeName(backend->GetBackendType()) : "none")
		<< " available=" << (info.available ? 1 : 0)
		<< " software_like=" << (info.software_like ? 1 : 0);
	AppendQuoted(log, "gl_vendor", info.vendor.empty() ? std::string("<unknown>") : info.vendor);
	AppendQuoted(log, "gl_renderer", info.renderer.empty() ? std::string("<unknown>") : info.renderer);
	AppendQuoted(log, "gl_version", info.version.empty() ? std::string("<unknown>") : info.version);
	AppendQuoted(log, "glsl_version", info.shading_language_version.empty() ? std::string("<unknown>") : info.shading_language_version);
	LOG_I("audio/render/skia") << log.str();
}

std::string MatchDenylistToken(std::string const& haystack, std::string const& raw_list) {
	if (haystack.empty() || raw_list.empty())
		return {};

	auto const haystack_lower = ToLowerAscii(haystack);
	std::string token;
	auto flush_token = [&](std::string &value) -> std::string {
		auto trimmed = value;
		trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char c) {
			return !std::isspace(c);
		}));
		trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char c) {
			return !std::isspace(c);
		}).base(), trimmed.end());
		value.clear();
		if (trimmed.empty())
			return {};
		auto const lowered = ToLowerAscii(trimmed);
		return haystack_lower.find(lowered) != std::string::npos ? trimmed : std::string();
	};

	for (char ch : raw_list) {
		if (ch == ',' || ch == ';' || ch == '|') {
			auto const match = flush_token(token);
			if (!match.empty())
				return match;
		}
		else {
			token.push_back(ch);
		}
	}
	return flush_token(token);
}

std::string GetSkiaGpuAutoDowngradeReason(AudioDisplaySkiaGpuDiagnostics const& info) {
	if (!info.available)
		return {};

	if (ReadEnvFlagDefaultOn("AEGISUB_AUDIO_SKIA_DISABLE_SOFTWARE_GL") && info.software_like)
		return "software_gl_stack";

	auto const denylist = ReadEnvString("AEGISUB_AUDIO_SKIA_GL_DENYLIST");
	if (denylist.empty())
		return {};

	auto const combined = info.vendor + " " + info.renderer + " " + info.version + " " + info.shading_language_version;
	auto const match = MatchDenylistToken(combined, denylist);
	return match.empty() ? std::string() : std::string("gl_denylist:") + match;
}

std::string EffectiveSkiaBackendName(bool skia_enabled, AudioDisplaySkiaBackend const* backend, std::string const& auto_downgrade_reason) {
	if (skia_enabled && backend) {
		switch (backend->GetBackendType()) {
			case AudioDisplaySkiaBackendType::Gpu: return "skia_gpu";
			case AudioDisplaySkiaBackendType::Bitmap: return "skia_bitmap";
		}
	}
	return auto_downgrade_reason.empty() ? std::string("wx_dc") : std::string("wx_dc_auto_downgraded");
}
#endif

char const* SpectrumComputationModeName(int64_t value) {
	switch (value) {
		case 0: return "legacy_linear";
		case 1: return "frequency_curve";
		default: return "unknown";
	}
}

char const* SpectrumMonoMixModeName(AudioSpectrumMonoMixMode value) {
	switch (value) {
		case AudioSpectrumMonoMixMode::MonoAverage: return "mono_average";
		case AudioSpectrumMonoMixMode::PerBinMaxPower: return "per_bin_max_power";
		case AudioSpectrumMonoMixMode::PerBinAveragePower: return "per_bin_average_power";
		default: return "unknown";
	}
}

char const* SpectrumChannelModeName(AudioSpectrumChannelMode value) {
	switch (value) {
		case AudioSpectrumChannelMode::MonoMix: return "mono_mix";
		case AudioSpectrumChannelMode::ChannelSplit: return "channel_split";
		default: return "unknown";
	}
}

char const* WaveformStyleName(int64_t value) {
	switch (value) {
		case 0: return "max_only";
		case 1: return "max_avg";
		case 2: return "continuous";
		default: return "unknown";
	}
}

char const* RendererProviderName(AudioRendererBitmapProvider const* renderer_provider) {
	if (dynamic_cast<AudioSpectrumRenderer const*>(renderer_provider))
		return "spectrum";
	if (dynamic_cast<AudioWaveformRenderer const*>(renderer_provider))
		return "waveform";
	return renderer_provider ? "unknown" : "none";
}

std::string JoinSelectedChannels(std::vector<int> const& channels) {
	if (channels.empty())
		return "all";

	std::ostringstream out;
	for (size_t i = 0; i < channels.size(); ++i) {
		if (i)
			out << ',';
		out << channels[i];
	}
	return out.str();
}

/// Emit audio renderer debug info into the standard NDJSON session log when
/// AEGISUB_AUDIO_DEBUG_LOG is enabled. Any non-false-like value enables it.
/// Throttled to at most once per second to avoid excessive log spam.
void MaybeLogDebugInfo(AudioRendererBitmapProvider *renderer_provider) {
	if (!renderer_provider)
		return;
	if (!IsAudioDebugLogEnabled())
		return;

	using clock = std::chrono::steady_clock;
	static auto last_write = clock::time_point{};
	static bool announced = false;
	auto now = clock::now();
	if (now - last_write < std::chrono::seconds(1))
		return;
	last_write = now;

	if (!announced) {
		announced = true;
		auto const session_log = agi::log::GetSessionLogFile();
		if (!session_log.empty())
			LOG_I("audio/debug") << "enabled=1 session_log_file=" << agi::fs::PathToString(session_log);
		else
			LOG_I("audio/debug") << "enabled=1 session_log_file=<none>";
	}

	auto lines = renderer_provider->GetDebugInfo();
	if (lines.empty())
		return;

	std::string message;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i != 0)
			message += " | ";
		message += lines[i];
	}
	if (message.empty())
		return;

	LOG_D("audio/debug") << message;
}

/// @brief Colourscheme-based UI colour provider
///
/// This class provides UI colours corresponding to the supplied audio colour
/// scheme.
///
/// SetColourScheme must be called to set the active colour scheme before
/// colours can be retrieved
class UIColours {
	wxColour light_colour;         ///< Light unfocused colour from the colour scheme
	wxColour dark_colour;          ///< Dark unfocused colour from the colour scheme
	wxColour sel_colour;           ///< Selection unfocused colour from the colour scheme
	wxColour light_focused_colour; ///< Light focused colour from the colour scheme
	wxColour dark_focused_colour;  ///< Dark focused colour from the colour scheme
	wxColour sel_focused_colour;   ///< Selection focused colour from the colour scheme

	bool focused = false; ///< Use the focused colours?
public:
	/// Set the colour scheme to load colours from
	/// @param name Name of the colour scheme
	void SetColourScheme(std::string const& name)
	{
		std::string opt_prefix = "Colour/Schemes/" + name + "/UI/";
		light_colour = to_wx(OPT_GET(opt_prefix + "Light")->GetColor());
		dark_colour = to_wx(OPT_GET(opt_prefix + "Dark")->GetColor());
		sel_colour = to_wx(OPT_GET(opt_prefix + "Selection")->GetColor());

		opt_prefix = "Colour/Schemes/" + name + "/UI Focused/";
		light_focused_colour = to_wx(OPT_GET(opt_prefix + "Light")->GetColor());
		dark_focused_colour = to_wx(OPT_GET(opt_prefix + "Dark")->GetColor());
		sel_focused_colour = to_wx(OPT_GET(opt_prefix + "Selection")->GetColor());
	}

	/// Set whether to use the focused or unfocused colours
	/// @param focused If true, focused colours will be returned
	void SetFocused(bool focused) { this->focused = focused; }

	/// Get the current Light colour
	wxColour Light() const { return focused ? light_focused_colour : light_colour; }
	/// Get the current Dark colour
	wxColour Dark() const { return focused ? dark_focused_colour : dark_colour; }
	/// Get the current Selection colour
	wxColour Selection() const { return focused ? sel_focused_colour : sel_colour; }
};
}

class AudioDisplay::AudioDisplayScrollbar final : public AudioDisplayInteractionObject {
	static const int base_height = 15;
	static const int base_min_width = 10;

	wxRect bounds;
	wxRect thumb;

	bool dragging = false;   ///< user is dragging with the primary mouse button

	int data_length = 1; ///< total amount of data in control
	int page_length = 1; ///< amount of data in one page
	int position    = 0; ///< first item displayed

	int sel_start  = -1; ///< first data item in selection
	int sel_length = 0;  ///< number of data items in selection

	UIColours colours; ///< Colour provider

	/// Containing display to send scroll events to
	AudioDisplay *display;

	int GetHeight() const { return display->FromDIP(base_height); }
	int GetMinWidth() const { return display->FromDIP(base_min_width); }

	// Recalculate thumb bounds from position and length data
	void RecalculateThumb()
	{
		thumb.width = int((int64_t)bounds.width * page_length / data_length);
		thumb.height = GetHeight();
		thumb.x = int((int64_t)bounds.width * position / data_length);
		thumb.y = bounds.y;
	}

public:
	AudioDisplayScrollbar(AudioDisplay *display)
	: display(display)
	{
	}

	/// The audio display has changed size
	void SetDisplaySize(const wxSize &display_size)
	{
		bounds.x = 0;
		bounds.y = display_size.y - GetHeight();
		bounds.width = display_size.x;
		bounds.height = GetHeight();
		page_length = display_size.x;

		RecalculateThumb();
	}

	void SetColourScheme(std::string const& name)
	{
		colours.SetColourScheme(name);
	}

	const wxRect & GetBounds() const { return bounds; }
	int GetPosition() const { return position; }

	int SetPosition(int new_position)
	{
		// These two conditionals can't be swapped, otherwise the position can become
		// negative if the entire data is shorter than one page.
		if (new_position + page_length >= data_length)
			new_position = data_length - page_length - 1;
		if (new_position < 0)
			new_position = 0;

		position = new_position;
		RecalculateThumb();

		return position;
	}

	void SetSelection(int new_start, int new_length)
	{
		sel_start = (int64_t)new_start * bounds.width / data_length;
		sel_length = (int64_t)new_length * bounds.width / data_length;
	}

	void ChangeLengths(int new_data_length, int new_page_length)
	{
		data_length = new_data_length;
		page_length = new_page_length;

		RecalculateThumb();
	}

	bool OnMouseEvent(wxMouseEvent &event) override
	{
		if (event.LeftIsDown())
		{
			const int thumb_left = event.GetPosition().x - thumb.width/2;
			const int data_length_less_page = data_length - page_length;
			const int shaft_length_less_thumb = bounds.width - thumb.width;

			display->ScrollPixelToLeft((int64_t)data_length_less_page * thumb_left / shaft_length_less_thumb);

			dragging = true;
		}
		else if (event.LeftUp())
		{
			dragging = false;
		}

		return dragging;
	}

	void Paint(wxDC &dc, bool has_focus, int load_progress)
	{
		colours.SetFocused(has_focus);

		dc.SetPen(wxPen(colours.Light()));
		dc.SetBrush(wxBrush(colours.Dark()));
		dc.DrawRectangle(bounds);

		if (sel_length > 0 && sel_start >= 0)
		{
			dc.SetPen(wxPen(colours.Selection()));
			dc.SetBrush(wxBrush(colours.Selection()));
			dc.DrawRectangle(wxRect(sel_start, bounds.y, sel_length, bounds.height));
		}

		dc.SetPen(wxPen(colours.Light()));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.DrawRectangle(bounds);

		if (load_progress > 0 && load_progress < data_length)
		{
			wxRect marker(
				(int64_t)bounds.width * load_progress / data_length - 25, bounds.y + 1,
				25, bounds.height - 2);
			dc.GradientFillLinear(marker, colours.Dark(), colours.Light());
		}

		dc.SetPen(wxPen(colours.Light()));
		dc.SetBrush(wxBrush(colours.Light()));

		// Paint the thumb at least min_width, expand to both left and right
		int min_width = GetMinWidth();
		if (thumb.width < min_width)
			dc.DrawRectangle(wxRect(thumb.x - (min_width - thumb.width) / 2, thumb.y, min_width, thumb.height));
		else
			dc.DrawRectangle(thumb);
	}

	AudioDisplayScrollbarRenderData BuildRenderModel(bool has_focus, int load_progress) {
		colours.SetFocused(has_focus);

		AudioDisplayScrollbarRenderData model;
		model.visible = true;
		model.bounds = ToDisplayRect(bounds);
		model.thumb = ToDisplayRect(thumb);
		model.light_colour = PackWxColour(colours.Light());
		model.dark_colour = PackWxColour(colours.Dark());
		model.selection_colour = PackWxColour(colours.Selection());
		if (sel_length > 0 && sel_start >= 0) {
			model.has_selection = true;
			model.selection_rect = ToDisplayRect(wxRect(sel_start, bounds.y, sel_length, bounds.height));
		}
		if (load_progress > 0 && load_progress < data_length) {
			model.has_load_marker = true;
			model.load_marker_rect = ToDisplayRect(wxRect(
				(int64_t)bounds.width * load_progress / data_length - 25, bounds.y + 1,
				25, bounds.height - 2));
		}

		int const min_width = GetMinWidth();
		if (model.thumb.width < min_width)
			model.thumb = ToDisplayRect(wxRect(thumb.x - (min_width - thumb.width) / 2, thumb.y, min_width, thumb.height));
		return model;
	}
};

class AudioDisplay::AudioDisplayTimeline final : public AudioDisplayInteractionObject {
	int duration = 0;          ///< Total duration in ms
	double ms_per_pixel = 1.0; ///< Milliseconds per pixel
	int pixel_left = 0;        ///< Leftmost visible pixel (i.e. scroll position)

	wxRect bounds;

	wxPoint drag_lastpos;
	bool dragging = false;

	enum Scale {
		Sc_Millisecond,
		Sc_Centisecond,
		Sc_Decisecond,
		Sc_Second,
		Sc_Decasecond,
		Sc_Minute,
		Sc_Decaminute,
		Sc_Hour,
		Sc_Decahour, // If anyone needs this they should reconsider their project
		Sc_MAX = Sc_Decahour
	};
	Scale scale_minor;
	int scale_major_modulo; ///< If minor_scale_mark_index % scale_major_modulo == 0 the mark is a major mark
	double scale_minor_divisor; ///< Absolute scale-mark index multiplied by this number gives sample index for scale mark

	AudioDisplay *display; ///< Containing audio display

	UIColours colours; ///< Colour provider

public:
	AudioDisplayTimeline(AudioDisplay *display)
	: display(display)
	{
		int width, height;
		display->GetTextExtent(wxS("0123456789:."), &width, &height);
		bounds.height = height + display->FromDIP(4);
	}

	void SetColourScheme(std::string const& name)
	{
		colours.SetColourScheme(name);
	}

	void SetDisplaySize(const wxSize &display_size)
	{
		// The size is without anything that goes below the timeline (like scrollbar)
		bounds.width = display_size.x;
		bounds.x = 0;
		bounds.y = 0;
	}

	int GetHeight() const { return bounds.height; }
	const wxRect & GetBounds() const { return bounds; }

	void ChangeAudio(int new_duration)
	{
		duration = new_duration;
	}

	void ChangeZoom(double new_ms_per_pixel)
	{
		ms_per_pixel = new_ms_per_pixel;

		double px_sec = 1000.0 / ms_per_pixel;

		if (px_sec > 3000) {
			scale_minor = Sc_Millisecond;
			scale_minor_divisor = 1.0;
			scale_major_modulo = 10;
		} else if (px_sec > 300) {
			scale_minor = Sc_Centisecond;
			scale_minor_divisor = 10.0;
			scale_major_modulo = 10;
		} else if (px_sec > 30) {
			scale_minor = Sc_Decisecond;
			scale_minor_divisor = 100.0;
			scale_major_modulo = 10;
		} else if (px_sec > 3) {
			scale_minor = Sc_Second;
			scale_minor_divisor = 1000.0;
			scale_major_modulo = 10;
		} else if (px_sec > 1.0/3.0) {
			scale_minor = Sc_Decasecond;
			scale_minor_divisor = 10000.0;
			scale_major_modulo = 6;
		} else if (px_sec > 1.0/9.0) {
			scale_minor = Sc_Minute;
			scale_minor_divisor = 60000.0;
			scale_major_modulo = 10;
		} else if (px_sec > 1.0/90.0) {
			scale_minor = Sc_Decaminute;
			scale_minor_divisor = 600000.0;
			scale_major_modulo = 6;
		} else {
			scale_minor = Sc_Hour;
			scale_minor_divisor = 3600000.0;
			scale_major_modulo = 10;
		}
	}

	void SetPosition(int new_pixel_left)
	{
		pixel_left = std::max(new_pixel_left, 0);
	}

	bool OnMouseEvent(wxMouseEvent &event) override
	{
		if (event.LeftDown())
		{
			drag_lastpos = event.GetPosition();
			dragging = true;
		}
		else if (event.LeftIsDown())
		{
			display->ScrollPixelToLeft(pixel_left - event.GetPosition().x + drag_lastpos.x);

			drag_lastpos = event.GetPosition();
			dragging = true;
		}
		else if (event.LeftUp())
		{
			dragging = false;
		}

		return dragging;
	}

	void Paint(wxDC &dc)
	{
		int bottom = bounds.y + bounds.height;

		// Background
		dc.SetPen(wxPen(colours.Dark()));
		dc.SetBrush(wxBrush(colours.Dark()));
		dc.DrawRectangle(bounds);

		// Top line
		dc.SetPen(wxPen(colours.Light()));
		dc.DrawLine(bounds.x, bottom-1, bounds.x+bounds.width, bottom-1);

		// Prepare for writing text
		dc.SetTextBackground(colours.Dark());
		dc.SetTextForeground(colours.Light());

		// Figure out the first scale mark to show
		int ms_left = int(pixel_left * ms_per_pixel);
		int next_scale_mark = int(ms_left / scale_minor_divisor);
		if (next_scale_mark * scale_minor_divisor < ms_left)
			next_scale_mark += 1;
		assert(next_scale_mark * scale_minor_divisor >= ms_left);

		// Draw scale marks
		int next_scale_mark_pos;
		int last_text_right = -1;
		int last_hour = -1, last_minute = -1;
		int major_tick_height = display->FromDIP(6);
		int minor_tick_height = display->FromDIP(4);
		if (duration < 3600) last_hour = 0; // Trick to only show hours if audio is longer than 1 hour
		do {
			next_scale_mark_pos = int(next_scale_mark * scale_minor_divisor / ms_per_pixel) - pixel_left;
			bool mark_is_major = next_scale_mark % scale_major_modulo == 0;

			if (mark_is_major)
				dc.DrawLine(next_scale_mark_pos, bottom - major_tick_height, next_scale_mark_pos, bottom-1);
			else
				dc.DrawLine(next_scale_mark_pos, bottom - minor_tick_height, next_scale_mark_pos, bottom-1);

			// Print time labels on major scale marks
			if (mark_is_major && next_scale_mark_pos > last_text_right)
			{
				double mark_time = next_scale_mark * scale_minor_divisor / 1000.0;
				int mark_hour = (int)(mark_time / 3600);
				int mark_minute = (int)(mark_time / 60) % 60;
				double mark_second = mark_time - mark_hour*3600.0 - mark_minute*60.0;

				wxString time_string;
				bool changed_hour = mark_hour != last_hour;
				bool changed_minute = mark_minute != last_minute;

				if (changed_hour)
				{
					time_string = fmt_wx("%d:%02d:", mark_hour, mark_minute);
					last_hour = mark_hour;
					last_minute = mark_minute;
				}
				else if (changed_minute)
				{
					time_string = fmt_wx("%d:", mark_minute);
					last_minute = mark_minute;
				}
				if (scale_minor >= Sc_Decisecond)
					time_string += fmt_wx("%02d", mark_second);
				else if (scale_minor == Sc_Centisecond)
					time_string += fmt_wx("%02.1f", mark_second);
				else
					time_string += fmt_wx("%02.2f", mark_second);

				int tw, th;
				dc.GetTextExtent(time_string, &tw, &th);
				last_text_right = next_scale_mark_pos + tw;

				dc.DrawText(time_string, next_scale_mark_pos, 0);
			}

			next_scale_mark += 1;

		} while (next_scale_mark_pos < bounds.width);
	}

	AudioDisplayTimelineRenderData BuildRenderModel() {
		AudioDisplayTimelineRenderData model;
		model.visible = true;
		model.bounds = ToDisplayRect(bounds);
		model.light_colour = PackWxColour(colours.Light());
		model.dark_colour = PackWxColour(colours.Dark());

		int const bottom = bounds.y + bounds.height;
		int const ms_left = int(pixel_left * ms_per_pixel);
		int next_scale_mark = int(ms_left / scale_minor_divisor);
		if (next_scale_mark * scale_minor_divisor < ms_left)
			next_scale_mark += 1;
		assert(next_scale_mark * scale_minor_divisor >= ms_left);

		int next_scale_mark_pos = 0;
		int last_text_right = -1;
		int last_hour = -1, last_minute = -1;
		if (duration < 3600)
			last_hour = 0;

		do {
			next_scale_mark_pos = int(next_scale_mark * scale_minor_divisor / ms_per_pixel) - pixel_left;
			bool const mark_is_major = next_scale_mark % scale_major_modulo == 0;

			AudioDisplayTimelineTick tick;
			tick.x = next_scale_mark_pos;
			tick.major = mark_is_major;

			if (mark_is_major && next_scale_mark_pos > last_text_right) {
				double const mark_time = next_scale_mark * scale_minor_divisor / 1000.0;
				int const mark_hour = (int)(mark_time / 3600);
				int const mark_minute = (int)(mark_time / 60) % 60;
				double const mark_second = mark_time - mark_hour * 3600.0 - mark_minute * 60.0;

				bool const changed_hour = mark_hour != last_hour;
				bool const changed_minute = mark_minute != last_minute;

				wxString label_wx;
				if (changed_hour) {
					label_wx = fmt_wx("%d:%02d:", mark_hour, mark_minute);
					last_hour = mark_hour;
					last_minute = mark_minute;
				}
				else if (changed_minute) {
					label_wx = fmt_wx("%d:", mark_minute);
					last_minute = mark_minute;
				}

				if (scale_minor >= Sc_Decisecond)
					label_wx += fmt_wx("%02d", mark_second);
				else if (scale_minor == Sc_Centisecond)
					label_wx += fmt_wx("%02.1f", mark_second);
				else
					label_wx += fmt_wx("%02.2f", mark_second);

				tick.label = std::string(label_wx.utf8_str());

				int tw = 0;
				int th = 0;
				display->GetTextExtent(label_wx, &tw, &th);
				last_text_right = next_scale_mark_pos + tw;
			}

			model.ticks.push_back(std::move(tick));
			next_scale_mark += 1;
		} while (next_scale_mark_pos < bounds.width);

		return model;
	}
};

namespace {
class AudioStyleRangeMerger final : public AudioRenderingStyleRanges {
	typedef std::map<int, AudioRenderingStyle> style_map;
public:
	typedef style_map::iterator iterator;

private:
	style_map points;

	void Split(int point)
	{
		auto it = points.lower_bound(point);
		if (it == points.end() || it->first != point)
		{
			assert(it != points.begin());
			points[point] = (--it)->second;
		}
	}

	void Restyle(int start, int end, AudioRenderingStyle style)
	{
		assert(points.lower_bound(end) != points.end());
		for (auto pt = points.lower_bound(start); pt->first < end; ++pt)
		{
			if (style > pt->second)
				pt->second = style;
		}
	}

public:
	AudioStyleRangeMerger()
	{
		points[0] = AudioStyle_Normal;
	}

	void AddRange(int start, int end, AudioRenderingStyle style) override
	{

		if (start < 0) start = 0;
		if (end < start) return;

		Split(start);
		Split(end);
		Restyle(start, end, style);
	}

	iterator begin() { return points.begin(); }
	iterator end() { return points.end(); }
};

}

class AudioMarkerInteractionObject final : public AudioDisplayInteractionObject {
	// Object-pair being interacted with
	std::vector<AudioMarker*> markers;
	AudioTimingController *timing_controller;
	// Audio display drag is happening on
	AudioDisplay *display;
	// Mouse button used to initiate the drag
	wxMouseButton button_used;
	// Default to snapping to snappable markers
	bool default_snap = OPT_GET("Audio/Snap/Enable")->GetBool();
	// Range in pixels to snap at
	int snap_range = OPT_GET("Audio/Snap/Distance")->GetInt();

public:
	AudioMarkerInteractionObject(std::vector<AudioMarker*> markers, AudioTimingController *timing_controller, AudioDisplay *display, wxMouseButton button_used)
	: markers(std::move(markers))
	, timing_controller(timing_controller)
	, display(display)
	, button_used(button_used)
	{
	}

	bool OnMouseEvent(wxMouseEvent &event) override
	{
		if (event.Dragging())
		{
			timing_controller->OnMarkerDrag(
				markers,
				display->TimeFromRelativeX(event.GetPosition().x),
				default_snap != event.ShiftDown() ? display->TimeFromAbsoluteX(snap_range) : 0);
		}

		// We lose the marker drag if the button used to initiate it goes up
		return !event.ButtonUp(button_used);
	}

	/// Get the position in milliseconds of this group of markers
	int GetPosition() const { return markers.front()->GetPosition(); }
};

enum class AudioDisplayHostKind {
	Window = 0,
	GpuCanvas,
};

#ifdef WITH_SKIA
void LogAudioDisplayHostSelection(
	AudioDisplayHostKind host,
	int64_t render_backend,
	AudioDisplaySkiaHostPreference host_preference,
	RemoteSessionInfo const& remote_info,
	std::string const& reason) {
	std::ostringstream log;
	log << "host=" << (host == AudioDisplayHostKind::GpuCanvas ? "gpu_canvas" : "window")
		<< " render_backend=" << render_backend;
	AppendQuoted(log, "render_backend_name", RenderBackendName(render_backend));
	AppendQuoted(log, "skia_host_preference", AudioDisplaySkiaHostPreferenceName(host_preference));
	AppendQuoted(log, "reason", reason.empty() ? std::string("normal") : reason);
	log << " remote_session=" << (remote_info.remote ? 1 : 0)
		<< " sm_remote_session=" << remote_info.sm_remote_session
		<< " sm_remote_control=" << remote_info.sm_remote_control
		<< " wts_query_ok=" << (remote_info.wts_query_ok ? 1 : 0)
		<< " wts_protocol=" << remote_info.wts_protocol;
	AppendQuoted(log, "session_name", remote_info.session_name.empty() ? std::string("<empty>") : remote_info.session_name);
	if (!audio_display_auto_downgrade_reason.empty())
		AppendQuoted(log, "auto_downgrade_reason", audio_display_auto_downgrade_reason);
	LOG_I("audio/render/host") << log.str();
}
#endif

AudioDisplayHostKind SelectAudioDisplayHostKind() {
#ifdef WITH_SKIA
	auto const render_backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
	auto const host_preference = ReadAudioDisplaySkiaHostPreference();
	auto const remote_info = QueryRemoteSessionInfo();
	AudioDisplayHostKind host = AudioDisplayHostKind::Window;
	std::string reason;

	if (render_backend == 1) {
		host = AudioDisplayHostKind::GpuCanvas;
		reason = "explicit_gpu";
	}
	else if (render_backend == 2) {
		host = AudioDisplayHostKind::Window;
		reason = "explicit_skia_bitmap";
	}
	else if (render_backend == 3 || !IsSkiaAudioRenderBackendEnabled()) {
		host = AudioDisplayHostKind::Window;
		reason = "explicit_wxdc";
	}
	else switch (host_preference) {
		case AudioDisplaySkiaHostPreference::Bitmap:
			host = AudioDisplayHostKind::Window;
			reason = "host_preference_bitmap";
			break;
		case AudioDisplaySkiaHostPreference::Gpu:
			host = AudioDisplayHostKind::GpuCanvas;
			reason = "host_preference_gpu";
			break;
		case AudioDisplaySkiaHostPreference::Auto:
		default:
			if (IsAutoWxDcForced()) {
				host = AudioDisplayHostKind::Window;
				reason = "auto_wxdc_forced";
			}
			else if (remote_info.remote) {
				host = AudioDisplayHostKind::Window;
				reason = "remote_session";
				MarkAutoDowngradedToWxDc(reason);
			}
			else {
				host = AudioDisplayHostKind::GpuCanvas;
				reason = "auto_gpu";
			}
			break;
	}

	LogAudioDisplayHostSelection(host, render_backend, host_preference, remote_info, reason);
	return host;
#else
	return AudioDisplayHostKind::Window;
#endif
}

class AudioDisplayWindowHost final : public wxWindow, public AudioDisplay {
public:
	AudioDisplayWindowHost(wxWindow *parent, AudioController *controller, agi::Context *context)
	: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_SIMPLE)
	, AudioDisplay(controller, context) {
		InitializeHost();
	}

	wxWindow* GetWindow() override { return this; }
	wxWindow const* GetWindow() const override { return this; }
};

#ifdef WITH_SKIA
class AudioDisplayGpuCanvasHost final : public wxGLCanvas, public AudioDisplay {
public:
	AudioDisplayGpuCanvasHost(wxWindow *parent, AudioController *controller, agi::Context *context)
	: wxGLCanvas(parent, -1, GetAudioDisplayGlAttribs(), wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_SIMPLE)
	, AudioDisplay(controller, context) {
		InitializeHost();
	}

	wxWindow* GetWindow() override { return this; }
	wxWindow const* GetWindow() const override { return this; }
	wxGLCanvas* GetGlCanvas() override { return this; }
	wxGLCanvas const* GetGlCanvas() const override { return this; }
};
#endif

AudioDisplay::AudioDisplay(AudioController *controller, agi::Context *context)
: audio_open_connection(context->project->AddAudioProviderListener(&AudioDisplay::OnAudioOpen, this))
, context(context)
, audio_renderer(agi::make_unique<AudioRenderer>(ReadEnvInt("AEGISUB_AUDIO_RENDERER_CACHE_BITMAP_WIDTH", 32, 8, 512)))
, audio_tile_compositor(agi::make_unique<AudioTileCompositor>())
, controller(controller)
, scrollbar()
, timeline()
, style_ranges({{0, 0}})
{
	audio_renderer->SetAmplitudeScale(scale_amplitude);
	content_backing_enabled = true;
#ifdef WITH_SKIA
	skia_gpu_auto_downgrade_reason = GetAutoDowngradeReason();
#endif
}

void AudioDisplay::BindHostEvents() {
	auto *window = GetWindow();
	window->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_MIDDLE_DOWN, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_MOTION, [this](wxMouseEvent &event) { OnMouseEvent(event); });
	window->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &event) { OnMouseEnter(event); });
	window->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) { OnMouseLeave(event); });
	window->Bind(wxEVT_PAINT, [this](wxPaintEvent &event) { OnPaint(event); });
	window->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) { OnSize(event); });
	window->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) { OnFocus(event); });
	window->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &event) { OnFocus(event); });
	window->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) { OnKeyDown(event); });
	window->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) { OnKeyDown(event); });
	scroll_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent &event) { OnScrollTimer(event); });
	high_frequency_refresh_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent &event) { OnHighFrequencyRefreshTimer(event); });
	middle_scrub_seek_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent &event) { OnMiddleScrubSeekTimer(event); });
	load_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent &event) { OnLoadTimer(event); });
}

void AudioDisplay::InitializeHost() {
	if (!scrollbar)
		scrollbar = agi::make_unique<AudioDisplayScrollbar>(this);
	if (!timeline)
		timeline = agi::make_unique<AudioDisplayTimeline>(this);

#ifdef WITH_SKIA
	// Render Backend preference: 0=Auto, 1=GPU (Skia), 2=Bitmap (Skia), 3=Bitmap (wxDC/GDI).
	// GPU host uses direct present on its own wxGLCanvas. Window host keeps
	// bitmap_skia/wxDC on a plain wxWindow so the wxDC path matches dev more closely.
	if (IsSkiaAudioRenderBackendEnabled()) {
		auto const render_backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
		bool const auto_forced_wxdc = render_backend == 0 && IsAutoWxDcForced();

		if (!auto_forced_wxdc) {
			skia_renderer = agi::make_unique<AudioDisplaySkiaRenderer>();

			if (render_backend == 2) {
				skia_backend = CreateAudioDisplaySkiaBitmapBackend();
			}
			else if (auto *gl_canvas = GetGlCanvas()) {
				switch (ReadAudioDisplaySkiaHostPreference()) {
					case AudioDisplaySkiaHostPreference::Bitmap:
						if (render_backend == 0)
							skia_backend = CreateAudioDisplaySkiaBitmapBackend();
						break;
					case AudioDisplaySkiaHostPreference::Gpu:
					case AudioDisplaySkiaHostPreference::Auto:
						skia_gl_context = agi::make_unique<wxGLContext>(gl_canvas);
						if (skia_gl_context && skia_gl_context->IsOK())
							skia_backend = CreateAudioDisplaySkiaGpuBackend(gl_canvas, skia_gl_context.get());
						else
							skia_gl_context.reset();
						break;
				}
			}
			else if (render_backend == 0) {
				skia_backend = CreateAudioDisplaySkiaBitmapBackend();
			}
		}

		if (skia_backend && skia_backend->GetBackendType() != AudioDisplaySkiaBackendType::Gpu)
			skia_gl_context.reset();

		skia_waveform_content_enabled = static_cast<bool>(skia_backend) && static_cast<bool>(skia_renderer);
		content_backing_enabled = auto_forced_wxdc
			? true
			: skia_backend && skia_backend->GetBackendType() == AudioDisplaySkiaBackendType::Bitmap;

		if (skia_backend) {
			skia_backend->QueryGpuDiagnostics(skia_gpu_diagnostics);
			LogSkiaGpuDiagnostics(skia_backend.get(), skia_gpu_diagnostics);

			auto const downgrade_reason = GetSkiaGpuAutoDowngradeReason(skia_gpu_diagnostics);
			if (!downgrade_reason.empty()) {
				if (render_backend == 0) {
					skia_gpu_auto_downgrade_reason = downgrade_reason;
					MarkAutoDowngradedToWxDc(downgrade_reason);
					LOG_W("audio/render/skia") << "auto backend downgraded to wx_dc reason=" << downgrade_reason;
					skia_waveform_content_enabled = false;
					skia_backend.reset();
					skia_renderer.reset();
					skia_gl_context.reset();
					content_backing_enabled = true;
					RequestWindowHostRebuild();
				}
				else {
					LOG_W("audio/render/skia") << "explicit gpu backend kept despite downgrade candidate reason=" << downgrade_reason;
				}
			}
		}
		else if (!auto_forced_wxdc) {
			LOG_W("audio/render/skia") << "gpu backend unavailable; falling back to wx_dc";
			skia_renderer.reset();
			content_backing_enabled = true;
			if (render_backend == 0) {
				skia_gpu_auto_downgrade_reason = "gpu_backend_unavailable";
				MarkAutoDowngradedToWxDc(skia_gpu_auto_downgrade_reason);
				RequestWindowHostRebuild();
			}
		}
	}
#endif
	SetZoomLevel(0);

	SetMinClientSize(wxSize(-1, 70));
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetThemeEnabled(false);
	BindHostEvents();
}

AudioDisplay::~AudioDisplay()
{
	ui_activation.Deactivate();
}

void AudioDisplay::RequestWindowHostRebuild() {
#ifdef WITH_SKIA
	if (host_rebuild_requested
		|| OPT_GET("Audio/Display/Draw/Render Backend")->GetInt() != 0
		|| !IsAutoWxDcForced()
		|| !GetGlCanvas())
		return;

	host_rebuild_requested = true;
	auto *window = GetWindow();
	auto *parent = window ? window->GetParent() : nullptr;
	if (!parent)
		return;

	auto *event = new wxCommandEvent(EVT_AUDIO_DISPLAY_REBUILD_HOST);
	event->SetEventObject(window);
	wxQueueEvent(parent, event);
#endif
}

AudioDisplay *CreateAudioDisplay(wxWindow *parent, AudioController *controller, agi::Context *context) {
	switch (SelectAudioDisplayHostKind()) {
		case AudioDisplayHostKind::GpuCanvas:
#ifdef WITH_SKIA
			try {
				return new AudioDisplayGpuCanvasHost(parent, controller, context);
			}
			catch (...) {
				if (OPT_GET("Audio/Display/Draw/Render Backend")->GetInt() != 0)
					throw;

				MarkAutoDowngradedToWxDc("gpu_canvas_create_failed");
				return new AudioDisplayWindowHost(parent, controller, context);
			}
#else
			break;
#endif
		case AudioDisplayHostKind::Window:
		default:
			return new AudioDisplayWindowHost(parent, controller, context);
	}

	return new AudioDisplayWindowHost(parent, controller, context);
}

void AudioDisplay::QueueHighFrequencyRefresh(const wxRect *rect, bool update) {
	pending_high_frequency_refresh = true;
	pending_high_frequency_update |= update;

	if (!rect) {
		pending_high_frequency_full_refresh = true;
	}
	else if (!pending_high_frequency_full_refresh) {
		if (pending_high_frequency_rect.IsEmpty())
			pending_high_frequency_rect = *rect;
		else
			pending_high_frequency_rect.Union(*rect);
	}

	if (!high_frequency_refresh_timer.IsRunning())
		high_frequency_refresh_timer.Start(high_frequency_refresh_interval_ms, true);
}

void AudioDisplay::FlushHighFrequencyRefresh() {
	if (!pending_high_frequency_refresh)
		return;

	if (high_frequency_refresh_timer.IsRunning())
		high_frequency_refresh_timer.Stop();

	if (pending_high_frequency_full_refresh)
		RequestPaint();
	else if (!pending_high_frequency_rect.IsEmpty())
		RequestPaint(&pending_high_frequency_rect);

	if (pending_high_frequency_update) {
		Update();
	}

	pending_high_frequency_full_refresh = false;
	pending_high_frequency_refresh = false;
	pending_high_frequency_update = false;
	pending_high_frequency_rect = wxRect();
}

void AudioDisplay::OnHighFrequencyRefreshTimer(wxTimerEvent &) {
	FlushHighFrequencyRefresh();
}

#ifdef WITH_SKIA
bool AudioDisplay::IsGpuSkiaBackendActive() const {
	return skia_waveform_content_enabled
		&& skia_backend
		&& skia_backend->GetBackendType() == AudioDisplaySkiaBackendType::Gpu;
}

void AudioDisplay::DisableSkiaBackend(std::string const& reason, char const* trigger) {
	if (!reason.empty())
		LOG_W("audio/render/skia") << reason;

	if (OPT_GET("Audio/Display/Draw/Render Backend")->GetInt() == 0) {
		auto downgrade_reason = skia_gpu_auto_downgrade_reason.empty() ? reason : skia_gpu_auto_downgrade_reason;
		MarkAutoDowngradedToWxDc(downgrade_reason);
		skia_gpu_auto_downgrade_reason = GetAutoDowngradeReason();
	}

	skia_waveform_content_enabled = false;
	skia_content_backing_image.reset();
	skia_frame_backing_image.reset();
	skia_frame_backing_valid = false;
	skia_backend.reset();
	skia_renderer.reset();
	skia_gl_context.reset();
	content_backing_enabled = true;
	InvalidateContentBacking();
	LogRenderConfiguration(trigger);
	RequestWindowHostRebuild();
}

bool AudioDisplay::EnsureGpuSkiaContentSurface() {
	if (!IsGpuSkiaBackendActive() || !skia_renderer || !audio_renderer_provider || !provider)
		return false;

	int const width = GetClientSize().GetWidth();
	if (width <= 0 || audio_height <= 0) {
		skia_content_backing_image.reset();
		content_backing_valid = false;
		return false;
	}

	bool const up_to_date = content_backing_valid
		&& content_backing_scroll_left == scroll_left
		&& content_backing_audio_top == audio_top
		&& content_backing_audio_height == audio_height
		&& content_backing_client_width == width
		&& content_backing_ms_per_pixel == ms_per_pixel;
	if (up_to_date)
		return true;

	auto surface = skia_backend->AcquireCachedContentSurface(width, audio_height);
	auto *canvas = surface ? surface->getCanvas() : nullptr;
	if (!surface || !canvas) {
		skia_content_backing_image.reset();
		content_backing_valid = false;
		return false;
	}

	wxRect full_audio_rect(0, audio_top, width, audio_height);
	auto model = BuildRenderModel(full_audio_rect, false, false);
	model.viewport.audio_top = 0;
	model.viewport.update_rect = AudioDisplayRect{0, 0, width, audio_height};
	model.audio_bounds = AudioDisplayRect{0, 0, width, audio_height};
	audio_renderer_provider->PopulateRenderModel(model);

	canvas->clear(SK_ColorTRANSPARENT);
	if (model.audio_bounds.width > 0 && model.audio_bounds.height > 0) {
		SkColor bg_color = SK_ColorBLACK;
		if (model.content_kind == AudioDisplayContentKind::Waveform && !model.waveform.palettes.empty())
			bg_color = ToSkColorFromPacked(model.waveform.palettes[AudioStyle_Normal].background);
		else if (model.content_kind == AudioDisplayContentKind::Spectrum && !model.spectrum.palettes.empty())
			bg_color = ToSkColorFromPacked(model.spectrum.palettes[AudioStyle_Normal].colours[0]);

		SkPaint bg;
		bg.setAntiAlias(false);
		bg.setStyle(SkPaint::kFill_Style);
		bg.setColor(bg_color);
		canvas->drawRect(SkRect::MakeXYWH(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(audio_height)), bg);
	}

	if (!skia_renderer->DrawContentToCanvas(*canvas, wxRect(0, 0, width, audio_height), model)) {
		skia_content_backing_image.reset();
		content_backing_valid = false;
		return false;
	}

	skia_content_backing_image = surface->makeImageSnapshot();
	if (!skia_content_backing_image) {
		content_backing_valid = false;
		return false;
	}

	content_backing_scroll_left = scroll_left;
	content_backing_ms_per_pixel = ms_per_pixel;
	content_backing_audio_top = audio_top;
	content_backing_audio_height = audio_height;
	content_backing_client_width = width;
	content_backing_valid = true;
	return true;
}

bool AudioDisplay::TryReuseGpuSkiaContentSurfaceForScroll(int old_scroll_left, int new_scroll_left) {
	if (!IsGpuSkiaBackendActive() || !skia_renderer || !audio_renderer_provider || !provider || !skia_content_backing_image)
		return false;
	if (!content_backing_valid || content_backing_scroll_left != old_scroll_left)
		return false;

	int const width = GetClientSize().GetWidth();
	int const delta = new_scroll_left - old_scroll_left;
	if (width <= 0 || audio_height <= 0 || delta == 0 || std::abs(delta) >= width)
		return false;
	if (content_backing_audio_top != audio_top
		|| content_backing_audio_height != audio_height
		|| content_backing_client_width != width
		|| content_backing_ms_per_pixel != ms_per_pixel)
		return false;

	auto surface = skia_backend->AcquireCachedContentSurface(width, audio_height);
	auto *canvas = surface ? surface->getCanvas() : nullptr;
	if (!surface || !canvas)
		return false;

	int const exposed_width = std::abs(delta);
	int const exposed_x = delta > 0 ? width - exposed_width : 0;
	if (exposed_width <= 0)
		return false;

	canvas->clear(SK_ColorTRANSPARENT);
	canvas->drawImage(skia_content_backing_image, static_cast<float>(-delta), 0.0f);

	wxRect exposed_display_rect(exposed_x, audio_top, exposed_width, audio_height);
	auto model = BuildRenderModel(exposed_display_rect, false, false);
	model.viewport.audio_top = 0;
	model.viewport.update_rect = AudioDisplayRect{exposed_x, 0, exposed_width, audio_height};
	model.audio_bounds = AudioDisplayRect{0, 0, width, audio_height};
	audio_renderer_provider->PopulateRenderModel(model);

	if (!skia_renderer->DrawContentToCanvas(*canvas, wxRect(exposed_x, 0, exposed_width, audio_height), model)) {
		skia_content_backing_image.reset();
		content_backing_valid = false;
		return false;
	}

	skia_content_backing_image = surface->makeImageSnapshot();
	if (!skia_content_backing_image) {
		content_backing_valid = false;
		return false;
	}

	content_backing_scroll_left = new_scroll_left;
	content_backing_ms_per_pixel = ms_per_pixel;
	content_backing_audio_top = audio_top;
	content_backing_audio_height = audio_height;
	content_backing_client_width = width;
	content_backing_valid = true;
	return true;
}

bool AudioDisplay::TryPaintWithSkiaGpu(wxDC &dc, wxRect const& full_rect) {
	if (!IsGpuSkiaBackendActive() || !skia_renderer || !skia_backend)
		return false;

	auto frame_target = skia_backend->CreatePresentTarget(full_rect);
	auto *canvas = frame_target ? frame_target->GetCanvas() : nullptr;
	if (!(frame_target && frame_target->IsValid() && canvas))
		return false;
	auto frame_surface = skia_backend->AcquireCachedFrameSurface(full_rect.width, full_rect.height);
	auto *frame_canvas = frame_surface ? frame_surface->getCanvas() : nullptr;
	if (!(frame_surface && frame_canvas))
		return false;

	auto &model = reusable_render_model;
	bool const frame_size_changed = !skia_frame_backing_valid
		|| skia_frame_backing_client_width != full_rect.width
		|| skia_frame_backing_client_height != full_rect.height;

	auto rebuild_full_frame = [&]() -> bool {
		FillRenderModel(model, full_rect, true, true);
		frame_canvas->clear(SK_ColorTRANSPARENT);

		if (EnsureGpuSkiaContentSurface() && skia_content_backing_image) {
			frame_canvas->drawImage(skia_content_backing_image, 0.0f, static_cast<float>(audio_top));
			if (skia_renderer->CanDrawAudioAreaOverlays(model)
				&& !skia_renderer->CompositeAudioAreaOverlaysToCanvas(*frame_canvas, full_rect, model))
				return false;
			skia_renderer->DrawChromeToCanvas(*frame_canvas, full_rect, model);
		}
		else {
			audio_renderer_provider->PopulateRenderModel(model);
			if (!skia_renderer->DrawFrameToCanvas(*frame_canvas, full_rect, model))
				return false;
		}

		skia_frame_backing_image = frame_surface->makeImageSnapshot();
		if (!skia_frame_backing_image)
			return false;
		skia_frame_backing_client_width = full_rect.width;
		skia_frame_backing_client_height = full_rect.height;
		skia_frame_backing_valid = true;
		return true;
	};

	if (frame_size_changed) {
		skia_frame_backing_image.reset();
		skia_frame_backing_valid = false;
	}

	if (!skia_frame_backing_valid) {
		LogAudioCursorDebug("paint_frame.skia_full", [&](std::ostringstream& out) {
			AppendRect(out, "full_rect", full_rect);
			out << " frame_backing_valid=0"
				<< " track_cursor_pos=" << track_cursor_pos
				<< " scroll_left=" << scroll_left
				<< " audio_top=" << audio_top
				<< " audio_height=" << audio_height;
		});
		if (!rebuild_full_frame())
			return false;
	}
	else {
		wxRect audio_bounds(0, audio_top, full_rect.width, audio_height);
		for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
			wxRect rect = region.GetRect();
			if (rect.width <= 0 || rect.height <= 0)
				continue;
			rect.Intersect(full_rect);
			if (rect.width <= 0 || rect.height <= 0)
				continue;

			FillRenderModel(
				model,
				rect,
				scrollbar && scrollbar->GetBounds().Intersects(rect),
				timeline && timeline->GetBounds().Intersects(rect));

			LogAudioCursorDebug("paint_region.skia_cached", [&](std::ostringstream& out) {
				AppendRect(out, "rect", rect);
				out << " cursor_visible=" << model.track_cursor.visible
					<< " cursor_abs=" << model.track_cursor_absolute_x
					<< " cursor_rel=" << model.track_cursor.x
					<< " marker_count=" << model.marker_geometry.size()
					<< " redraw_scrollbar=" << model.redraw_scrollbar
					<< " redraw_timeline=" << model.redraw_timeline;
			});

			frame_canvas->save();
			frame_canvas->clipRect(SkRect::MakeXYWH(
				static_cast<float>(rect.x),
				static_cast<float>(rect.y),
				static_cast<float>(rect.width),
				static_cast<float>(rect.height)));

			bool drew_region = false;
			if (audio_bounds.Intersects(rect)) {
				if (EnsureGpuSkiaContentSurface() && skia_content_backing_image) {
					frame_canvas->drawImage(skia_content_backing_image, 0.0f, static_cast<float>(audio_top));
					if (skia_renderer->CanDrawAudioAreaOverlays(model)
						&& !skia_renderer->CompositeAudioAreaOverlaysToCanvas(*frame_canvas, rect, model)) {
						frame_canvas->restore();
						return false;
					}
					drew_region = true;
				}
				else {
					audio_renderer_provider->PopulateRenderModel(model);
					if (!skia_renderer->DrawFrameToCanvas(*frame_canvas, rect, model)) {
						frame_canvas->restore();
						return false;
					}
					drew_region = true;
				}
			}

			if (model.redraw_timeline || model.redraw_scrollbar) {
				skia_renderer->DrawChromeToCanvas(*frame_canvas, rect, model);
				drew_region = true;
			}

			frame_canvas->restore();
			if (!drew_region)
				continue;
		}

		skia_frame_backing_image = frame_surface->makeImageSnapshot();
		if (!skia_frame_backing_image)
			return false;
	}

	canvas->clear(SK_ColorTRANSPARENT);
	canvas->drawImage(skia_frame_backing_image, 0.0f, 0.0f);
	return frame_target->PresentTo(dc, false);
}
#endif

void AudioDisplay::InvalidateContentBacking() {
#ifdef WITH_SKIA
	skia_content_backing_image.reset();
	skia_frame_backing_image.reset();
	skia_frame_backing_valid = false;
#endif
#ifdef WITH_SKIA
	skia_frame_backing_client_width = 0;
	skia_frame_backing_client_height = 0;
#endif
	content_backing_valid = false;
}

bool AudioDisplay::CanUseContentBackingForCurrentViewport() const {
	if (!content_backing_enabled || !audio_renderer_provider || !provider)
		return false;
	if (!audio_renderer_provider->AllowsPlaceholder())
		return true;

	int const width = std::max(0, GetClientSize().GetWidth());
	if (width <= 0)
		return false;

	return audio_renderer_provider->IsCacheRangeReady(scroll_left, width);
}

bool AudioDisplay::EnsureContentBackingBitmapStorage(int width, int height) {
	if (width <= 0 || height <= 0) {
		content_backing_bitmap = wxBitmap();
		content_backing_valid = false;
		return false;
	}

	bool const needs_recreate = !content_backing_bitmap.IsOk()
		|| content_backing_bitmap.GetWidth() != width
		|| content_backing_bitmap.GetHeight() != height;
	if (needs_recreate) {
		content_backing_bitmap = wxBitmap(width, height, wxBITMAP_SCREEN_DEPTH);
		content_backing_valid = false;
	}
	return content_backing_bitmap.IsOk();
}

bool AudioDisplay::EnsureContentBackingBitmap() {
	if (!CanUseContentBackingForCurrentViewport()) {
		content_backing_valid = false;
		return false;
	}

	int const width = GetClientSize().GetWidth();
	if (width <= 0 || audio_height <= 0) {
		content_backing_valid = false;
		content_backing_bitmap = wxBitmap();
		return false;
	}

	if (!EnsureContentBackingBitmapStorage(width, audio_height))
		return false;

	bool const up_to_date = content_backing_valid
		&& content_backing_scroll_left == scroll_left
		&& content_backing_audio_top == audio_top
		&& content_backing_audio_height == audio_height
		&& content_backing_client_width == width
		&& content_backing_ms_per_pixel == ms_per_pixel;
	if (!up_to_date)
		UpdateContentBackingBitmap();

	return content_backing_valid && content_backing_bitmap.IsOk();
}

bool AudioDisplay::TryReuseContentBackingBitmapForScroll(int old_scroll_left, int new_scroll_left) {
	if (!CanUseContentBackingForCurrentViewport()) {
		content_backing_valid = false;
		return false;
	}
	if (!content_backing_enabled || !content_backing_valid || !content_backing_bitmap.IsOk())
		return false;
	if (content_backing_scroll_left != old_scroll_left)
		return false;

	int const width = GetClientSize().GetWidth();
	int const delta = new_scroll_left - old_scroll_left;
	if (width <= 0 || audio_height <= 0 || delta == 0 || std::abs(delta) >= width)
		return false;
	if (content_backing_audio_top != audio_top
		|| content_backing_audio_height != audio_height
		|| content_backing_client_width != width
		|| content_backing_ms_per_pixel != ms_per_pixel)
		return false;
	if (!EnsureContentBackingBitmapStorage(width, audio_height))
		return false;

	int const exposed_width = std::abs(delta);
	int const exposed_x = delta > 0 ? width - exposed_width : 0;
	if (exposed_width <= 0)
		return false;
	int const reused_width = width - exposed_width;
	if (reused_width <= 0)
		return false;

	if (!content_backing_scratch_bitmap.IsOk()
		|| content_backing_scratch_bitmap.GetWidth() != width
		|| content_backing_scratch_bitmap.GetHeight() != audio_height) {
		content_backing_scratch_bitmap = wxBitmap(width, audio_height, wxBITMAP_SCREEN_DEPTH);
	}
	if (!content_backing_scratch_bitmap.IsOk())
		return false;

	{
		std::swap(content_backing_bitmap, content_backing_scratch_bitmap);

		wxMemoryDC src_dc;
		wxMemoryDC dst_dc;
		src_dc.SelectObject(content_backing_scratch_bitmap);
		dst_dc.SelectObject(content_backing_bitmap);
		int const src_x = delta > 0 ? delta : 0;
		int const dst_x = delta > 0 ? 0 : exposed_width;
		dst_dc.Blit(dst_x, 0, reused_width, audio_height, &src_dc, src_x, 0, wxCOPY, false);
		src_dc.SelectObject(wxNullBitmap);
		dst_dc.SelectObject(wxNullBitmap);
	}

	wxRect exposed_display_rect(exposed_x, audio_top, exposed_width, audio_height);
	auto model = BuildRenderModel(exposed_display_rect, false, false);
	model.viewport.audio_top = 0;
	model.viewport.update_rect = AudioDisplayRect{exposed_x, 0, exposed_width, audio_height};
	model.audio_bounds = AudioDisplayRect{0, 0, width, audio_height};

	wxMemoryDC backing_dc;
	backing_dc.SelectObject(content_backing_bitmap);
	backing_dc.SetClippingRegion(exposed_x, 0, exposed_width, audio_height);
	PaintAudio(backing_dc, model);
	backing_dc.DestroyClippingRegion();
	backing_dc.SelectObject(wxNullBitmap);

	content_backing_scroll_left = new_scroll_left;
	content_backing_ms_per_pixel = ms_per_pixel;
	content_backing_audio_top = audio_top;
	content_backing_audio_height = audio_height;
	content_backing_client_width = width;
	content_backing_valid = true;
	return true;
}

void AudioDisplay::UpdateContentBackingBitmap() {
	if (!CanUseContentBackingForCurrentViewport()) {
		content_backing_valid = false;
		return;
	}
	if (!audio_renderer_provider || !provider)
		return;

	int const width = GetClientSize().GetWidth();
	if (width <= 0 || audio_height <= 0) {
		content_backing_valid = false;
		return;
	}

	if (!EnsureContentBackingBitmapStorage(width, audio_height))
		return;

	wxRect full_audio_rect(0, audio_top, width, audio_height);
	auto model = BuildRenderModel(full_audio_rect, false, false);
	model.viewport.audio_top = 0;
	model.viewport.update_rect = AudioDisplayRect{0, 0, width, audio_height};
	model.audio_bounds = AudioDisplayRect{0, 0, width, audio_height};

	bool rendered_with_skia = false;
#ifdef WITH_SKIA
	if (skia_waveform_content_enabled && skia_renderer && audio_renderer_provider) {
		audio_renderer_provider->PopulateRenderModel(model);
		auto target = skia_backend ? skia_backend->CreateContentTarget(content_backing_bitmap, wxRect(0, 0, width, audio_height)) : nullptr;
		if (target && target->IsValid()) {
			auto *canvas = target->GetCanvas();
			rendered_with_skia = canvas
				&& skia_renderer->DrawContentToCanvas(*canvas, target->GetRect(), model)
				&& target->Finalize();
		}
	}
#endif

	if (!rendered_with_skia) {
		wxMemoryDC backing_dc;
		backing_dc.SelectObject(content_backing_bitmap);
		backing_dc.SetClippingRegion(model.viewport.update_rect.x, model.viewport.update_rect.y,
			model.viewport.update_rect.width, model.viewport.update_rect.height);
		PaintAudio(backing_dc, model);
		backing_dc.DestroyClippingRegion();
		backing_dc.SelectObject(wxNullBitmap);
	}

	content_backing_scroll_left = scroll_left;
	content_backing_ms_per_pixel = ms_per_pixel;
	content_backing_audio_top = audio_top;
	content_backing_audio_height = audio_height;
	content_backing_client_width = width;
	content_backing_valid = true;
}

void AudioDisplay::ScrollBy(int pixel_amount)
{
	ScrollPixelToLeft(scroll_left + pixel_amount);
}

void AudioDisplay::ScrollPixelToLeft(int pixel_position)
{
	const int client_width = GetClientRect().GetWidth();

	if (pixel_position + client_width >= pixel_audio_width)
		pixel_position = pixel_audio_width - client_width;
	if (pixel_position < 0)
		pixel_position = 0;

	if (pixel_position == scroll_left)
		return;

	const int old_scroll_left = scroll_left;
	scroll_left = pixel_position;
	scrollbar->SetPosition(scroll_left);
	timeline->SetPosition(scroll_left);
	SyncTrackCursorToMouseAfterScroll();
	HintVisibleAudioRange();
	WarmVisibleAudioCache();
	bool reused_content = false;
#ifdef WITH_SKIA
	reused_content = TryReuseGpuSkiaContentSurfaceForScroll(old_scroll_left, scroll_left);
#endif
	if (!reused_content)
		reused_content = TryReuseContentBackingBitmapForScroll(old_scroll_left, scroll_left);
	if (!reused_content)
		InvalidateContentBacking();
	if (dragged_object)
		QueueHighFrequencyRefresh(nullptr, true);
	else if (controller && controller->IsPlaying())
		QueueHighFrequencyRefresh(nullptr, false);
	else
		RequestPaint();
}

void AudioDisplay::ScrollTimeRangeInView(const TimeRange &range)
{
	int client_width = GetClientRect().GetWidth();
	int range_begin = AbsoluteXFromTime(range.begin());
	int range_end = AbsoluteXFromTime(range.end());
	int range_len = range_end - range_begin;

	// Remove 5 % from each side of the client area.
	int leftadjust = client_width / 20;
	int client_left = scroll_left + leftadjust;
	client_width = client_width * 9 / 10;

	// Is everything already in view?
	if (range_begin >= client_left && range_end <= client_left+client_width)
		return;

	// The entire range can fit inside the view, center it
	if (range_len < client_width)
	{
		ScrollPixelToLeft(range_begin - (client_width-range_len)/2 - leftadjust);
	}

	// Range doesn't fit in view and we're viewing a middle part of it, just leave it alone
	else if (range_begin < client_left && range_end > client_left+client_width)
	{
		// nothing
	}

	// Right edge is in view, scroll it as far to the right as possible
	else if (range_end >= client_left && range_end < client_left+client_width)
	{
		ScrollPixelToLeft(range_end - client_width - leftadjust);
	}

	// Nothing is in view or the left edge is in view, scroll left edge as far to the left as possible
	else
	{
		ScrollPixelToLeft(range_begin - leftadjust);
	}
}

void AudioDisplay::SetZoomLevel(int new_zoom_level)
{
	zoom_level = new_zoom_level;

	const int factor = GetZoomLevelFactor(zoom_level);
	const int base_pixels_per_second = 50; /// @todo Make this customisable
	const double base_ms_per_pixel = 1000.0 / base_pixels_per_second;
	const double new_ms_per_pixel = 100.0 * base_ms_per_pixel / factor;

	if (ms_per_pixel == new_ms_per_pixel) return;

	int client_width = GetClientSize().GetWidth();
	double cursor_pos = track_cursor_pos >= 0 ? track_cursor_pos - scroll_left : client_width / 2.0;
	double cursor_time = (scroll_left + cursor_pos) * ms_per_pixel;

	ms_per_pixel = new_ms_per_pixel;
	pixel_audio_width = std::max(1, int(GetDuration() / ms_per_pixel));

	audio_renderer->SetMillisecondsPerPixel(ms_per_pixel);
	scrollbar->ChangeLengths(pixel_audio_width, client_width);
	timeline->ChangeZoom(ms_per_pixel);

	const int old_scroll_left = scroll_left;
	ScrollPixelToLeft(AbsoluteXFromTime(cursor_time) - cursor_pos);
	if (scroll_left == old_scroll_left) {
		HintVisibleAudioRange();
		WarmVisibleAudioCache();
	}
	if (track_cursor_pos >= 0)
		track_cursor_pos = AbsoluteXFromTime(cursor_time);
	InvalidateContentBacking();
	RequestPaint();
}

wxString AudioDisplay::GetZoomLevelDescription(int level) const
{
	const int factor = GetZoomLevelFactor(level);
	const int base_pixels_per_second = 50; /// @todo Make this customisable along with the above
	const int second_pixels = 100 * base_pixels_per_second / factor;

	return fmt_tl("%d%%, %d pixel/second", factor, second_pixels);
}

int AudioDisplay::GetZoomLevelFactor(int level)
{
	int factor = 100;

	if (level > 0)
	{
		factor += 25 * level;
	}
	else if (level < 0)
	{
		if (level >= -5)
			factor += 10 * level;
		else if (level >= -11)
			factor = 50 + (level+5) * 5;
		else
			factor = 20 + level + 11;
		if (factor <= 0)
			factor = 1;
	}

	return factor;
}

void AudioDisplay::SetAmplitudeScale(float scale)
{
	scale_amplitude = scale;
	audio_renderer->SetAmplitudeScale(scale);
	InvalidateContentBacking();
	RequestPaint();
}

void AudioDisplay::SetInteractivePrefetchEnabled(bool enabled) {
	if (audio_renderer_provider)
		audio_renderer_provider->SetInteractivePrefetchEnabled(enabled);
}

void AudioDisplay::SyncToCurrentAudioProvider() {
	if (context->project->AudioProvider())
		ApplyAudioProvider(context->project->AudioProvider());
}

void AudioDisplay::SetSpectrumChannelMode(AudioSpectrumChannelMode mode) {
	if (spectrum_channel_mode_runtime == mode)
		return;
	spectrum_channel_mode_runtime = mode;
	if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get())) {
		spectrum->SetChannelMode(mode);
		audio_renderer->Invalidate();
		InvalidateContentBacking();
		LogRenderConfiguration("spectrum_channel_mode");
		RequestPaint();
	}
}

AudioSpectrumChannelMode AudioDisplay::GetSpectrumChannelMode() const {
	return spectrum_channel_mode_runtime;
}

void AudioDisplay::SetSpectrumMonoMixMode(AudioSpectrumMonoMixMode mode) {
	if (spectrum_mono_mix_mode_runtime == mode)
		return;
	spectrum_mono_mix_mode_runtime = mode;
	if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get())) {
		spectrum->SetMonoMixMode(mode);
		audio_renderer->Invalidate();
		InvalidateContentBacking();
		LogRenderConfiguration("spectrum_mono_mix_mode");
		RequestPaint();
	}
}

AudioSpectrumMonoMixMode AudioDisplay::GetSpectrumMonoMixMode() const {
	return spectrum_mono_mix_mode_runtime;
}

void AudioDisplay::OnSpectrumMonoMixModeChanged(agi::OptionValue const& opt) {
	auto mode = static_cast<AudioSpectrumMonoMixMode>(mid<int64_t>(0, opt.GetInt(), 2));
	SetSpectrumMonoMixMode(mode);
}

void AudioDisplay::OnSpectrumComputationModeChanged(agi::OptionValue const& opt) {
	auto mode = static_cast<AudioSpectrumComputationMode>(mid<int64_t>(0, opt.GetInt(), 1));
	if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get())) {
		spectrum->SetComputationMode(mode);
		audio_renderer->Invalidate();
		InvalidateContentBacking();
		LogRenderConfiguration("spectrum_computation_mode");
		RequestPaint();
	}
}

void AudioDisplay::OnSpectrumFrequencyCurveChanged(agi::OptionValue const& opt) {
	auto preset = static_cast<int>(mid<int64_t>(0, opt.GetInt(), 4));
	if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get())) {
		spectrum->SetFrequencyCurvePreset(preset);
		audio_renderer->Invalidate();
		InvalidateContentBacking();
		LogRenderConfiguration("spectrum_frequency_curve");
		RequestPaint();
	}
}

void AudioDisplay::SetSpectrumSelectedChannels(const std::vector<int> &channels) {
	if (spectrum_selected_channels_runtime == channels)
		return;
	spectrum_selected_channels_runtime = channels;
	if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get())) {
		spectrum->SetSelectedChannels(spectrum_selected_channels_runtime);
		audio_renderer->Invalidate();
		InvalidateContentBacking();
		LogRenderConfiguration("spectrum_selected_channels");
		RequestPaint();
	}
}

int AudioDisplay::GetProviderChannels() const {
	return provider ? std::max(1, provider->GetChannels()) : 1;
}

void AudioDisplay::LogRenderConfiguration(char const* trigger) const {
	if (!context || !provider || !audio_renderer_provider)
		return;

	auto const stats = provider->GetMemoryStats();
	auto const selection = GetLastAudioProviderSelectionReport();
	auto const attempts = aegisub::provider_selection_diagnostics::FormatAttempts(selection);
	auto const audio_cache_type = OPT_GET("Audio/Cache/Type")->GetInt();
	auto const render_backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
	auto const waveform_style = OPT_GET("Audio/Display/Waveform Style")->GetInt();
	auto spectrum_quality = OPT_GET("Audio/Renderer/Spectrum/Quality")->GetInt();
	auto const configured_spectrum_quality = spectrum_quality;
#ifdef WITH_FFTW3
	spectrum_quality += 2;
#endif
	spectrum_quality = mid<int64_t>(0, spectrum_quality, 5);
	auto const spectrum_mode = mid<int64_t>(0, OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt(), 1);

	std::ostringstream out;
	out << "trigger=" << (trigger ? trigger : "unknown")
		<< " renderer=" << RendererProviderName(audio_renderer_provider.get())
		<< " spectrum=" << (OPT_GET("Audio/Spectrum")->GetBool() ? 1 : 0);

	if (context->project) {
		AppendQuoted(out, "audio_path", PathForLog(context->project->AudioName()));
		AppendQuoted(out, "video_path", PathForLog(context->project->VideoName()));
	}

	AppendQuoted(out, "provider_option", OPT_GET("Audio/Provider")->GetString());
	if (!selection.preferred_provider.empty())
		AppendQuoted(out, "preferred_provider", selection.preferred_provider);
	if (!selection.selected_provider.empty())
		AppendQuoted(out, "selected_provider", selection.selected_provider);
	if (!attempts.empty())
		AppendQuoted(out, "provider_attempts", attempts);
	AppendQuoted(out, "provider_name", stats.provider_name.empty() ? std::string("<unknown>") : stats.provider_name);
	AppendQuoted(out, "provider_storage", stats.storage_kind.empty() ? std::string("<unknown>") : stats.storage_kind);

	out << " sample_rate=" << stats.sample_rate
		<< " channels=" << stats.channels
		<< " bytes_per_sample=" << stats.bytes_per_sample
		<< " float_samples=" << (stats.float_samples ? 1 : 0)
		<< " samples=" << stats.num_samples
		<< " decoded_samples=" << stats.decoded_samples
		<< " logical_bytes=" << stats.logical_bytes
		<< " decoded_bytes=" << stats.decoded_bytes
		<< " storage_bytes=" << stats.storage_bytes
		<< " zoom_level=" << zoom_level
		<< " ms_per_pixel=" << ms_per_pixel
		<< " amplitude_scale=" << scale_amplitude
		<< " audio_height=" << audio_height
		<< " audio_width_px=" << pixel_audio_width
		<< " scroll_left=" << scroll_left
		<< " cache_type=" << audio_cache_type;
	AppendQuoted(out, "cache_type_name", AudioCacheTypeName(audio_cache_type));
	AppendQuoted(out, "hd_cache_config", OPT_GET("Audio/Cache/HD/Location")->GetString());
	AppendQuoted(out, "hd_cache_path", ResolveAudioHdCachePathForLog(context));
	AppendQuoted(out, "ffms_index_cache_dir", ResolveTokenPathForLog(context, "?local/ffms2cache/"));

	out << " renderer_memory_max_mb=" << OPT_GET("Audio/Renderer/Spectrum/Memory Max")->GetInt()
		<< " renderer_cache_format=" << OPT_GET("Audio/Renderer/Spectrum/Cache Format")->GetInt()
		<< " renderer_cache_bitmap_width=" << ReadEnvInt("AEGISUB_AUDIO_RENDERER_CACHE_BITMAP_WIDTH", 32, 8, 512)
		<< " allow_placeholder=" << (audio_renderer_provider->AllowsPlaceholder() ? 1 : 0)
		<< " content_backing=" << (content_backing_enabled ? 1 : 0)
		<< " render_backend=" << render_backend;
	AppendQuoted(out, "render_backend_name", RenderBackendName(render_backend));
#ifdef WITH_SKIA
	AppendQuoted(out, "effective_backend", EffectiveSkiaBackendName(skia_waveform_content_enabled, skia_backend.get(), skia_gpu_auto_downgrade_reason));
	AppendQuoted(out, "skia_backend_type", skia_backend ? SkiaBackendTypeName(skia_backend->GetBackendType()) : "none");
	out << " skia_gpu_available=" << (skia_gpu_diagnostics.available ? 1 : 0)
		<< " skia_gpu_software_like=" << (skia_gpu_diagnostics.software_like ? 1 : 0);
	AppendQuoted(out, "skia_gl_vendor", skia_gpu_diagnostics.vendor.empty() ? std::string("<unknown>") : skia_gpu_diagnostics.vendor);
	AppendQuoted(out, "skia_gl_renderer", skia_gpu_diagnostics.renderer.empty() ? std::string("<unknown>") : skia_gpu_diagnostics.renderer);
	AppendQuoted(out, "skia_gl_version", skia_gpu_diagnostics.version.empty() ? std::string("<unknown>") : skia_gpu_diagnostics.version);
	AppendQuoted(out, "skia_glsl_version", skia_gpu_diagnostics.shading_language_version.empty() ? std::string("<unknown>") : skia_gpu_diagnostics.shading_language_version);
	if (!skia_gpu_auto_downgrade_reason.empty())
		AppendQuoted(out, "skia_auto_downgrade_reason", skia_gpu_auto_downgrade_reason);
	out << " skia_gpu=" << ((skia_backend && skia_backend->GetBackendType() == AudioDisplaySkiaBackendType::Gpu) ? 1 : 0);
#else
	AppendQuoted(out, "effective_backend", "wx_dc");
	out << " skia_gpu=0";
#endif

	out << " waveform_style=" << waveform_style;
	AppendQuoted(out, "waveform_style_name", WaveformStyleName(waveform_style));
	out << " spectrum_quality_config=" << configured_spectrum_quality
		<< " spectrum_quality_effective=" << spectrum_quality
		<< " spectrum_mode=" << spectrum_mode;
	AppendQuoted(out, "spectrum_mode_name", SpectrumComputationModeName(spectrum_mode));
	out << " spectrum_freq_curve=" << OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt()
		<< " spectrum_channel_mode=" << static_cast<int>(spectrum_channel_mode_runtime);
	AppendQuoted(out, "spectrum_channel_mode_name", SpectrumChannelModeName(spectrum_channel_mode_runtime));
	out << " spectrum_mono_mix_mode=" << static_cast<int>(spectrum_mono_mix_mode_runtime);
	AppendQuoted(out, "spectrum_mono_mix_mode_name", SpectrumMonoMixModeName(spectrum_mono_mix_mode_runtime));
	AppendQuoted(out, "spectrum_selected_channels", JoinSelectedChannels(spectrum_selected_channels_runtime));
	AppendQuoted(out, "ffms_decode_error_handling", OPT_GET("Provider/Audio/FFmpegSource/Decode Error Handling")->GetString());
	out << " ffms_downmix=" << (OPT_GET("Provider/Audio/FFmpegSource/Downmix")->GetBool() ? 1 : 0)
		<< " ffms_index_all_tracks=" << (OPT_GET("Provider/FFmpegSource/Index All Tracks")->GetBool() ? 1 : 0);
	AppendQuoted(out, "avisynth_runtime_path", OPT_GET("Provider/Avisynth/Runtime Path")->GetString());

	LOG_I("audio/render/config") << out.str();
}

void AudioDisplay::ReloadRenderingSettings()
{
	std::string colour_scheme_name;

	if (OPT_GET("Audio/Spectrum")->GetBool())
	{
		colour_scheme_name = OPT_GET("Colour/Audio Display/Spectrum")->GetString();
		auto audio_spectrum_renderer = agi::make_unique<AudioSpectrumRenderer>(colour_scheme_name);
		spectrum_mono_mix_mode_runtime = static_cast<AudioSpectrumMonoMixMode>(
			mid<int64_t>(0, OPT_GET("Audio/Renderer/Spectrum/Mono Mix Mode")->GetInt(), 2));

		int64_t spectrum_quality = OPT_GET("Audio/Renderer/Spectrum/Quality")->GetInt();
#ifdef WITH_FFTW3
		// FFTW is so fast we can afford to upgrade quality by two levels
		spectrum_quality += 2;
#endif
		spectrum_quality = mid<int64_t>(0, spectrum_quality, 5);

		// Quality indexes:        0  1  2  3   4   5
		int spectrum_width[]    = {8, 9, 9, 9, 10, 11};
		int spectrum_distance[] = {8, 8, 7, 6,  6,  5};

		audio_spectrum_renderer->SetResolution(
			spectrum_width[spectrum_quality],
			spectrum_distance[spectrum_quality]);

		int64_t spectrum_mode = OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt();
		spectrum_mode = mid<int64_t>(0, spectrum_mode, 1);
		audio_spectrum_renderer->SetComputationMode(static_cast<AudioSpectrumComputationMode>(spectrum_mode));

		int64_t spectrum_freq_curve = OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt();
		audio_spectrum_renderer->SetFrequencyCurvePreset(static_cast<int>(spectrum_freq_curve));
		audio_spectrum_renderer->SetChannelMode(spectrum_channel_mode_runtime);
		audio_spectrum_renderer->SetMonoMixMode(spectrum_mono_mix_mode_runtime);
		audio_spectrum_renderer->SetSelectedChannels(spectrum_selected_channels_runtime);

		// Restore analysis cache capacity: the Set* calls above invoke
		// AgeCache(0) which shrinks max_cache_bytes to a single block.
		// In the Skia/GPU path the legacy AudioRenderer::Render path
		// (which normally restores the size) is bypassed, so we must
		// restore it explicitly.
		{
			auto max_mb = OPT_GET("Audio/Renderer/Spectrum/Memory Max")->GetInt();
			size_t max_bytes = static_cast<size_t>(std::max<int64_t>(1, max_mb)) * 1024 * 1024;
			audio_spectrum_renderer->AgeCache(max_bytes);
		}

		audio_renderer_provider = std::move(audio_spectrum_renderer);
	}
	else
	{
		colour_scheme_name = OPT_GET("Colour/Audio Display/Waveform")->GetString();
		audio_renderer_provider = agi::make_unique<AudioWaveformRenderer>(colour_scheme_name);
	}

	if (audio_renderer_provider)
		audio_renderer_provider->SetAllowPlaceholder(ReadEnvFlagDefaultOn("AEGISUB_AUDIO_ANALYSIS_PLACEHOLDER"));

	auto ui_lifetime = ui_activation.GetLifetime();
	audio_renderer_provider->SetContentReadyCallback([this, ui_lifetime] {
		agi::ui::MainAsyncIfAlive(ui_lifetime, [this] {
			OnRenderContentReady();
		});
	});

	audio_renderer->SetRenderer(audio_renderer_provider.get());
	scrollbar->SetColourScheme(colour_scheme_name);
	timeline->SetColourScheme(colour_scheme_name);

	InvalidateContentBacking();
	RequestPaint();
	if (provider && pixel_audio_width > 1)
		LogRenderConfiguration("render_settings");
}

void AudioDisplay::OnLoadTimer(wxTimerEvent&)
{
	using namespace std::chrono;
	if (provider)
	{
		const auto now = steady_clock::now();
		const auto elapsed = duration_cast<milliseconds>(now - audio_load_start_time).count();
		if (elapsed == 0) return;

		const int64_t new_decoded_count = provider->GetDecodedSamples();
		if (new_decoded_count != last_sample_decoded)
			audio_load_speed = (audio_load_speed + (double)new_decoded_count / elapsed) / 2;
		if (audio_load_speed == 0) return;

		int new_pos = AbsoluteXFromTime(elapsed * audio_load_speed * 1000.0 / provider->GetSampleRate());
		if (new_pos > audio_load_position)
			audio_load_position = new_pos;

		const double left = last_sample_decoded * 1000.0 / provider->GetSampleRate() / ms_per_pixel;
		const double right = new_decoded_count * 1000.0 / provider->GetSampleRate() / ms_per_pixel;

		if (left < scroll_left + pixel_audio_width && right >= scroll_left) {
			InvalidateContentBacking();
			RequestPaint();
		}
		else {
			wxRect sb = scrollbar->GetBounds();
			RequestPaint(&sb);
		}
		last_sample_decoded = new_decoded_count;
	}

	if (!provider || last_sample_decoded == provider->GetNumSamples()) {
		load_timer.Stop();
		audio_load_position = -1;
	}
}

#ifdef WITH_SKIA
bool AudioDisplay::TryPaintWithSkia(wxDC &dc) {
	if (!audio_renderer_provider || !provider)
		return false;
	if (!skia_waveform_content_enabled || !skia_renderer || !skia_backend)
		return false;
	if (!skia_gpu_diagnostics.available && skia_backend->GetBackendType() != AudioDisplaySkiaBackendType::Bitmap) {
		AudioDisplaySkiaGpuDiagnostics diagnostics;
		if (skia_backend->QueryGpuDiagnostics(diagnostics)) {
			skia_gpu_diagnostics = diagnostics;
			LogSkiaGpuDiagnostics(skia_backend.get(), skia_gpu_diagnostics);

			auto const render_backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
			auto const downgrade_reason = GetSkiaGpuAutoDowngradeReason(skia_gpu_diagnostics);
			if (!downgrade_reason.empty() && render_backend == 0) {
				skia_gpu_auto_downgrade_reason = downgrade_reason;
				DisableSkiaBackend(std::string("auto backend downgraded to wx_dc reason=") + downgrade_reason, "skia_auto_downgrade");
				return false;
			}
		}
	}

	wxRect full_rect(wxPoint(0, 0), GetClientSize());
	if (full_rect.width <= 0 || full_rect.height <= 0)
		return false;
	if (IsGpuSkiaBackendActive()) {
		if (TryPaintWithSkiaGpu(dc, full_rect))
			return true;
		DisableSkiaBackend(
			std::string(skia_backend ? SkiaBackendTypeName(skia_backend->GetBackendType()) : "unknown")
				+ " backend failed; falling back to wx_dc",
			"skia_fallback");
		return false;
	}

	bool painted_any = false;
	auto &model = reusable_render_model;
	wxRect audio_rect(0, audio_top, full_rect.width, audio_height);
	for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
		wxRect rect = region.GetRect();
		if (rect.width <= 0 || rect.height <= 0)
			continue;

		rect.Intersect(full_rect);
		if (rect.width <= 0 || rect.height <= 0)
			continue;

		FillRenderModel(
			model,
			rect,
			scrollbar && scrollbar->GetBounds().Intersects(rect),
			timeline && timeline->GetBounds().Intersects(rect));
		if (audio_renderer_provider && audio_rect.Intersects(rect))
			audio_renderer_provider->PopulateRenderModel(model);

		if (!skia_renderer->CanDrawFrame(model))
			continue;

		auto frame_target = skia_backend->CreatePresentTarget(rect);
		auto *canvas = frame_target ? frame_target->GetCanvas() : nullptr;
		if (!(frame_target
			&& frame_target->IsValid()
			&& canvas
			&& skia_renderer->DrawFrameToCanvas(*canvas, frame_target->GetRect(), model)
			&& frame_target->PresentTo(dc, false))) {
			painted_any = false;
			goto skia_fallback;
		}

		painted_any = true;
	}

	if (painted_any)
		return true;

	// If the Skia path cannot create or present a valid surface, stop
	// retrying it for this control instance. The wxDC path below is the
	// compatibility fallback for low-end/virtualized GL.
	skia_fallback:
	DisableSkiaBackend(
		std::string(skia_backend ? SkiaBackendTypeName(skia_backend->GetBackendType()) : "unknown")
			+ " backend failed; falling back to wx_dc",
		"skia_fallback");
	return false;
}
#endif

void AudioDisplay::RequestPaint(const wxRect *rect, bool erase_background) {
	if (rect)
		RefreshRect(*rect, erase_background);
	else
		Refresh(erase_background);
}

void AudioDisplay::OnPaint(wxPaintEvent&)
{
	if (!audio_renderer_provider || !provider) return;

	auto paint_with_wxdc = [&](wxDC &dc) {
		wxRect update_box = GetUpdateRegion().GetBox();
		wxMemoryDC backing_dc;
		bool backing_checked = false;
		bool backing_selected = false;
		bool prepared_visible_audio = false;

		auto ensure_backing_dc = [&]() -> bool {
			if (!content_backing_enabled)
				return false;
			if (!backing_checked) {
				backing_checked = true;
				EnsureContentBackingBitmap();
			}
			if (!content_backing_valid || !content_backing_bitmap.IsOk())
				return false;
			if (!backing_selected) {
				backing_dc.SelectObject(content_backing_bitmap);
				backing_selected = true;
			}
			return true;
		};

		for (wxRegionIterator region(GetUpdateRegion()); region; ++region)
		{
			wxRect rect = region.GetRect();
			if (rect.width <= 0 || rect.height <= 0)
				continue;

			rect.Intersect(wxRect(wxPoint(0, 0), GetClientSize()));
			if (rect.width <= 0 || rect.height <= 0)
				continue;

			dc.SetClippingRegion(rect);
			auto model = BuildRenderModel(
				rect,
				scrollbar->GetBounds().Intersects(rect),
				timeline->GetBounds().Intersects(rect));

			LogAudioCursorDebug("paint_region.wxdc", [&](std::ostringstream& out) {
				AppendRect(out, "rect", rect);
				out << " cursor_visible=" << model.track_cursor.visible
					<< " cursor_abs=" << model.track_cursor_absolute_x
					<< " cursor_rel=" << model.track_cursor.x
					<< " marker_count=" << model.marker_geometry.size()
					<< " redraw_scrollbar=" << model.redraw_scrollbar
					<< " redraw_timeline=" << model.redraw_timeline;
			});

			wxRect audio_bounds(0, audio_top, GetClientSize().GetWidth(), audio_height);
			if (audio_bounds.Intersects(rect)) {
				wxRect audio_rect = rect;
				audio_rect.Intersect(audio_bounds);
				if (audio_rect.width > 0 && audio_rect.height > 0) {
					bool used_backing = ensure_backing_dc();
					if (!used_backing && !prepared_visible_audio) {
						HintVisibleAudioRange();
						WarmVisibleAudioCache();
						prepared_visible_audio = true;
					}
					if (used_backing) {
						dc.Blit(
							audio_rect.x,
							audio_rect.y,
							audio_rect.width,
							audio_rect.height,
							&backing_dc,
							audio_rect.x,
							audio_rect.y - audio_top);
					}
					else {
						PaintAudio(dc, model);
					}

					PaintStaticAudioOverlays(dc, model);
					PaintMarkers(dc, model);
					PaintLabels(dc, model);
				}
			}

			if (model.redraw_scrollbar)
				PaintScrollbar(dc, model);
			if (model.redraw_timeline)
				PaintTimeline(dc, model);

			dc.DestroyClippingRegion();
		}

		if (backing_selected)
			backing_dc.SelectObject(wxNullBitmap);

		wxRect audio_bounds(0, audio_top, GetClientSize().GetWidth(), audio_height);
		if (track_cursor_pos >= 0 && audio_bounds.Intersects(update_box)) {
			auto model = BuildRenderModel(update_box, false, false);
			LogAudioCursorDebug("paint_track_cursor.wxdc", [&](std::ostringstream& out) {
				AppendRect(out, "update_box", update_box);
				out << " cursor_abs=" << model.track_cursor_absolute_x
					<< " cursor_rel=" << model.track_cursor.x
					<< " marker_count=" << model.marker_geometry.size();
			});
			PaintTrackCursor(dc, model);
		}

		MaybeLogDebugInfo(audio_renderer_provider.get());
	};

#ifdef WITH_SKIA
	if (IsGpuSkiaBackendActive()) {
		wxPaintDC dc(GetWindow());
		if (TryPaintWithSkia(dc)) {
			MaybeLogDebugInfo(audio_renderer_provider.get());
			return;
		}

		paint_with_wxdc(dc);
		return;
	}
#endif

	wxSize const client_size = GetClientSize();
	if (!paint_bitmap.IsOk()
		|| paint_bitmap.GetWidth() != client_size.x
		|| paint_bitmap.GetHeight() != client_size.y) {
		paint_bitmap = wxBitmap(client_size.x, client_size.y, wxBITMAP_SCREEN_DEPTH);
	}

	wxBufferedPaintDC dc(GetWindow(), paint_bitmap);
	paint_with_wxdc(dc);
}

AudioDisplayRenderModel AudioDisplay::BuildRenderModel(
	const wxRect &update_rect,
	bool redraw_scrollbar,
	bool redraw_timeline) const {
	AudioDisplayRenderModel model;
	FillRenderModel(model, update_rect, redraw_scrollbar, redraw_timeline);
	return model;
}

void AudioDisplay::FillRenderModel(
	AudioDisplayRenderModel &model,
	const wxRect &update_rect,
	bool redraw_scrollbar,
	bool redraw_timeline) const {
	model.Reset();
	model.viewport = BuildViewportRequest(update_rect);
	model.audio_bounds = AudioDisplayRect{0, audio_top, GetClientSize().GetWidth(), audio_height};
	model.viewport_time = TimeRange(model.viewport.begin_ms, model.viewport.end_ms);
	model.style_ranges = style_ranges;
	model.redraw_scrollbar = redraw_scrollbar;
	model.redraw_timeline = redraw_timeline;
	model.track_cursor_visible = track_cursor_pos >= 0;
	model.track_cursor_absolute_x = track_cursor_pos;
	model.track_cursor_label = std::string(track_cursor_label.utf8_str());
	if (scrollbar)
		model.scrollbar = scrollbar->BuildRenderModel(HasFocus(), audio_load_position);
	if (timeline)
		model.timeline = timeline->BuildRenderModel();

	if (auto *timing = controller ? controller->GetTimingController() : nullptr) {
		timing->GetMarkers(model.viewport_time, model.markers);
		timing->GetLabels(model.viewport_time, model.labels);
	}

	bool const log_cursor_model = IsAudioCursorDebugLogEnabled()
		&& (model.track_cursor_visible || !model.markers.empty());
	int current_video_marker_pos = -1;
	uint32_t play_cursor_colour = 0;
	int markers_at_track_cursor = 0;
	int markers_at_video_marker = 0;
	int play_cursor_like_markers = 0;
	int candidate_marker_count = 0;
	std::ostringstream candidate_markers;
	if (log_cursor_model) {
		current_video_marker_pos = GetCurrentVideoMarkerPos();
		play_cursor_colour = PackWxColour(to_wx(OPT_GET("Colour/Audio Display/Play Cursor")->GetColor()));
	}

	model.marker_geometry.reserve(model.markers.size());
	for (auto const* marker : model.markers) {
		int const marker_absolute_x = AbsoluteXFromTime(marker->GetPosition());
		AudioDisplayMarkerRenderData marker_data;
		marker_data.x = RelativeXFromTime(marker->GetPosition());
		marker_data.top = audio_top;
		marker_data.bottom = audio_top + std::max(0, audio_height - 1);
		{
			wxPen pen = marker->GetStyle();
			marker_data.style.colour = PackWxColour(pen.GetColour());
			marker_data.style.width = pen.GetWidth();
		}
		marker_data.feet = marker->GetFeet();

		if (log_cursor_model) {
			if (marker_absolute_x == model.track_cursor_absolute_x)
				++markers_at_track_cursor;
			if (marker_absolute_x == current_video_marker_pos)
				++markers_at_video_marker;
			if (marker_data.style.colour == play_cursor_colour && marker_data.feet == AudioMarker::Feet_None)
				++play_cursor_like_markers;

			bool const near_track_cursor = model.track_cursor_visible
				&& std::abs(marker_absolute_x - model.track_cursor_absolute_x) <= 1;
			bool const near_video_marker = current_video_marker_pos >= 0
				&& std::abs(marker_absolute_x - current_video_marker_pos) <= 1;
			bool const marker_looks_like_play_cursor = marker_data.style.colour == play_cursor_colour
				&& marker_data.feet == AudioMarker::Feet_None;
			if ((near_track_cursor || near_video_marker || marker_looks_like_play_cursor)
				&& candidate_marker_count < 8) {
				if (candidate_marker_count++)
					candidate_markers << ';';
				candidate_markers
					<< "abs=" << marker_absolute_x
					<< "/rel=" << marker_data.x
					<< "/colour=0x" << std::hex << marker_data.style.colour << std::dec
					<< "/feet=" << marker_data.feet
					<< "/width=" << marker_data.style.width;
			}
		}

		model.marker_geometry.push_back(std::move(marker_data));
	}

	model.label_geometry.reserve(model.labels.size());
	for (auto const& label : model.labels) {
		AudioDisplayRangeLabelRenderData label_data;
		label_data.text = std::string(label.text.utf8_str());
		label_data.left = RelativeXFromTime(label.range.begin());
		label_data.width = AbsoluteXFromTime(label.range.length());
		label_data.top = audio_top + FromDIP(4);
		model.label_geometry.push_back(std::move(label_data));
	}

	model.track_cursor.visible = model.track_cursor_visible;
	model.track_cursor.x = model.track_cursor_absolute_x - scroll_left;
	model.track_cursor.top = audio_top;
	model.track_cursor.bottom = audio_top + std::max(0, audio_height - 1);
	model.track_cursor.label = model.track_cursor_label;

	if (log_cursor_model) {
		LogAudioCursorDebug("fill_render_model", [&](std::ostringstream& out) {
			AppendRect(out, "update_rect", update_rect);
			out << " scroll_left=" << scroll_left
				<< " cursor_visible=" << model.track_cursor_visible
				<< " cursor_abs=" << model.track_cursor_absolute_x
				<< " cursor_rel=" << model.track_cursor.x
				<< " follows_mouse=" << track_cursor_follows_mouse
				<< " video_marker_abs=" << current_video_marker_pos
				<< " marker_count=" << model.marker_geometry.size()
				<< " markers_at_track_cursor=" << markers_at_track_cursor
				<< " markers_at_video_marker=" << markers_at_video_marker
				<< " play_cursor_like_markers=" << play_cursor_like_markers;
			auto const candidates = candidate_markers.str();
			if (!candidates.empty())
				AppendQuoted(out, "candidates", candidates);
		});
	}

	{
		wxString face = ResolveAudioLabelFontFace();
		if (!face.empty())
			model.audio_label_font_face = std::string(face.utf8_str());
	}

	if (spectrum_channel_mode_runtime == AudioSpectrumChannelMode::ChannelSplit) {
		if (auto *spectrum = dynamic_cast<AudioSpectrumRenderer *>(audio_renderer_provider.get()))
			model.split_channel_labels = spectrum->GetActiveChannelLabels();
	}
}

AudioViewportRequest AudioDisplay::BuildViewportRequest(const wxRect &update_rect) const {
	const int request_foot_size = FromDIP(foot_size);
	const int begin_ms = std::max(0, TimeFromRelativeX(update_rect.x - request_foot_size));
	const int end_ms = std::max(0, TimeFromRelativeX(update_rect.x + update_rect.width + request_foot_size));

	AudioViewportRequest viewport;
	viewport.scroll_left = scroll_left;
	viewport.ms_per_pixel = ms_per_pixel;
	viewport.audio_top = audio_top;
	viewport.audio_height = audio_height;
	viewport.foot_size = request_foot_size;
	viewport.update_rect = ToDisplayRect(update_rect);
	viewport.begin_ms = begin_ms;
	viewport.end_ms = end_ms;
	return viewport;
}

void AudioDisplay::HintVisibleAudioRange() const {
	if (!provider || ms_per_pixel <= 0.0)
		return;

	auto const sample_rate = provider->GetSampleRate();
	if (sample_rate <= 0)
		return;

	auto const client_width = std::max(0, GetClientSize().GetWidth());
	if (client_width <= 0)
		return;

	auto const now = std::chrono::steady_clock::now();
	if (controller && controller->IsPlaying() && !dragged_object) {
		int const min_scroll_delta = std::max(16, client_width / 8);
		if (last_visible_audio_hint_client_width == client_width
			&& last_visible_audio_hint_scroll_left >= 0
			&& std::abs(scroll_left - last_visible_audio_hint_scroll_left) < min_scroll_delta
			&& now - last_visible_audio_hint_time < std::chrono::milliseconds(high_frequency_refresh_interval_ms)) {
			return;
		}
	}

	auto const begin_ms = std::max(0, TimeFromAbsoluteX(scroll_left));
	auto const end_ms = std::max(begin_ms, TimeFromAbsoluteX(scroll_left + client_width));
	auto const start_frame = static_cast<int64_t>(begin_ms) * sample_rate / 1000;
	auto const end_frame = static_cast<int64_t>(end_ms) * sample_rate / 1000;
	provider->HintVisibleRange(start_frame, end_frame - start_frame);
	last_visible_audio_hint_time = now;
	last_visible_audio_hint_scroll_left = scroll_left;
	last_visible_audio_hint_client_width = client_width;
}

void AudioDisplay::WarmVisibleAudioCache() const {
	if (!audio_renderer_provider)
		return;
	if (controller && controller->IsPlaying() && !dragged_object)
		return;

	auto const client_width = std::max(0, GetClientSize().GetWidth());
	if (client_width <= 0)
		return;

	audio_renderer_provider->WarmCacheRange(scroll_left, client_width);
}

void AudioDisplay::OnRenderContentReady() {
	if (!audio_renderer_provider)
		return;

	auto const client_width = std::max(0, GetClientSize().GetWidth());
	if (client_width <= 0)
		return;

	if (!audio_renderer_provider->AllowsPlaceholder()
		&& !audio_renderer_provider->IsCacheRangeReady(scroll_left, client_width))
		return;

	InvalidateContentBacking();
	// During drag, use throttled refresh to avoid flooding the paint queue.
	// Without this, async cache completions were silently dropped and the
	// spectrum stayed black until drag ended.
	bool const needs_update = dragged_object;
	QueueHighFrequencyRefresh(nullptr, needs_update);
}

void AudioDisplay::PaintAudio(wxDC &dc, AudioDisplayRenderModel const& model) {
	if (!audio_tile_compositor || !audio_renderer)
		return;
	audio_tile_compositor->Compose(dc, *audio_renderer, model.viewport, model.style_ranges);
}

void AudioDisplay::PaintMarkers(wxDC &dc, AudioDisplayRenderModel const& model)
{
	if (model.marker_geometry.empty()) return;

	wxDCPenChanger pen_retainer(dc, wxPen());
	wxDCBrushChanger brush_retainer(dc, wxBrush());
	for (auto const& marker : model.marker_geometry)
	{
		wxColour pen_colour = UnpackToWxColour(marker.style.colour);
		dc.SetPen(wxPen(pen_colour, marker.style.width));
		if (model.audio_bounds.height > 0)
			dc.DrawLine(marker.x, marker.top, marker.x, marker.bottom);

		if (marker.feet == AudioMarker::Feet_None) continue;

		dc.SetBrush(wxBrush(pen_colour));
		dc.SetPen(*wxTRANSPARENT_PEN);

		if (marker.feet & AudioMarker::Feet_Left)
			PaintFoot(dc, marker.x, -1);
		if (marker.feet & AudioMarker::Feet_Right)
			PaintFoot(dc, marker.x, 1);
	}
}

void AudioDisplay::PaintScrollbar(wxDC &dc, AudioDisplayRenderModel const& model) {
	auto const& scrollbar_model = model.scrollbar;
	if (!scrollbar_model.visible)
		return;

	dc.SetPen(wxPen(UnpackToWxColour(scrollbar_model.light_colour)));
	dc.SetBrush(wxBrush(UnpackToWxColour(scrollbar_model.dark_colour)));
	dc.DrawRectangle(ToWxRect(scrollbar_model.bounds));

	if (scrollbar_model.has_selection) {
		dc.SetPen(wxPen(UnpackToWxColour(scrollbar_model.selection_colour)));
		dc.SetBrush(wxBrush(UnpackToWxColour(scrollbar_model.selection_colour)));
		dc.DrawRectangle(ToWxRect(scrollbar_model.selection_rect));
	}

	dc.SetPen(wxPen(UnpackToWxColour(scrollbar_model.light_colour)));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	dc.DrawRectangle(ToWxRect(scrollbar_model.bounds));

	if (scrollbar_model.has_load_marker)
		dc.GradientFillLinear(ToWxRect(scrollbar_model.load_marker_rect), UnpackToWxColour(scrollbar_model.dark_colour), UnpackToWxColour(scrollbar_model.light_colour));

	dc.SetPen(wxPen(UnpackToWxColour(scrollbar_model.light_colour)));
	dc.SetBrush(wxBrush(UnpackToWxColour(scrollbar_model.light_colour)));
	dc.DrawRectangle(ToWxRect(scrollbar_model.thumb));
}

void AudioDisplay::PaintFoot(wxDC &dc, int marker_x, int dir)
{
	int foot_size = FromDIP(6);
	wxPoint foot_top[3] = { wxPoint(foot_size * dir, 0), wxPoint(0, 0), wxPoint(0, foot_size) };
	wxPoint foot_bot[3] = { wxPoint(foot_size * dir, 0), wxPoint(0, -foot_size), wxPoint(0, 0) };
	dc.DrawPolygon(3, foot_top, marker_x, audio_top);
	dc.DrawPolygon(3, foot_bot, marker_x, audio_top + std::max(0, audio_height - 1));
}

void AudioDisplay::PaintLabels(wxDC &dc, AudioDisplayRenderModel const& model)
{
	if (model.label_geometry.empty()) return;

	wxDCFontChanger fc(dc);
	wxFont font = MakeAudioLabelFont(dc);
	fc.Set(font);
	dc.SetTextForeground(*wxWHITE);
	for (auto const& label : model.label_geometry)
	{
		wxString const wx_text = wxString::FromUTF8(label.text);
		wxSize const extent = dc.GetTextExtent(wx_text);

		// If it doesn't fit, truncate
		if (label.width < extent.GetWidth())
		{
			dc.SetClippingRegion(label.left, label.top, label.width, extent.GetHeight());
			dc.DrawText(wx_text, label.left, label.top);
			dc.DestroyClippingRegion();
		}
		// Otherwise center in the range
		else
		{
			dc.DrawText(wx_text, label.left + (label.width - extent.GetWidth()) / 2, label.top);
		}
	}
}

void AudioDisplay::PaintTimeline(wxDC &dc, AudioDisplayRenderModel const& model) {
	auto const& timeline_model = model.timeline;
	if (!timeline_model.visible)
		return;

	int const bottom = timeline_model.bounds.y + timeline_model.bounds.height;
	int const major_tick_height = FromDIP(6);
	int const minor_tick_height = FromDIP(4);

	dc.SetPen(wxPen(UnpackToWxColour(timeline_model.dark_colour)));
	dc.SetBrush(wxBrush(UnpackToWxColour(timeline_model.dark_colour)));
	dc.DrawRectangle(ToWxRect(timeline_model.bounds));

	dc.SetPen(wxPen(UnpackToWxColour(timeline_model.light_colour)));
	dc.DrawLine(timeline_model.bounds.x, bottom - 1, timeline_model.bounds.x + timeline_model.bounds.width, bottom - 1);

	dc.SetTextBackground(UnpackToWxColour(timeline_model.dark_colour));
	dc.SetTextForeground(UnpackToWxColour(timeline_model.light_colour));

	for (auto const& tick : timeline_model.ticks) {
		if (tick.major)
			dc.DrawLine(tick.x, bottom - major_tick_height, tick.x, bottom - 1);
		else
			dc.DrawLine(tick.x, bottom - minor_tick_height, tick.x, bottom - 1);

		if (!tick.label.empty())
			dc.DrawText(wxString::FromUTF8(tick.label), tick.x, timeline_model.bounds.y);
	}
}

void AudioDisplay::PaintSplitChannelLabels(wxDC &dc, AudioDisplayRenderModel const& model) {
	if (model.split_channel_labels.empty() || model.audio_bounds.height <= 0)
		return;

	const int band_h = model.audio_bounds.height / static_cast<int>(model.split_channel_labels.size());
	const int label_x = FromDIP(4);
	wxFont label_font = MakeAudioLabelFont(dc, -1);
	label_font.SetPointSize(std::max(7, label_font.GetPointSize()));
	dc.SetFont(label_font);
	for (size_t i = 0; i < model.split_channel_labels.size(); ++i) {
		const wxString wx_label = wxString::FromUTF8(model.split_channel_labels[i].c_str());
		const int label_y = model.audio_bounds.y + static_cast<int>(i) * band_h + FromDIP(2);
		dc.SetTextForeground(wxColour(0, 0, 0));
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
				if (dx || dy)
					dc.DrawText(wx_label, label_x + dx, label_y + dy);
		dc.SetTextForeground(wxColour(230, 230, 230));
		dc.DrawText(wx_label, label_x, label_y);
	}
}

void AudioDisplay::PaintStaticAudioOverlays(wxDC &dc, AudioDisplayRenderModel const& model) {
	PaintSplitChannelLabels(dc, model);
}

void AudioDisplay::PaintTrackCursor(wxDC &dc, AudioDisplayRenderModel const& model) {
	wxDCPenChanger penchanger(dc, wxPen(*wxWHITE));
	if (model.track_cursor.visible)
		dc.DrawLine(model.track_cursor.x, model.track_cursor.top, model.track_cursor.x, model.track_cursor.bottom);

	if (model.track_cursor.label.empty()) return;

	wxString const wx_label = wxString::FromUTF8(model.track_cursor.label);

	wxDCFontChanger fc(dc);
	wxFont font = MakeAudioLabelFont(dc);
	fc.Set(font);

	wxSize label_size(dc.GetTextExtent(wx_label));
	int label_margin = FromDIP(2);
	wxPoint label_pos(model.track_cursor.x - label_size.x/2, model.track_cursor.top + label_margin);
	label_pos.x = mid(label_margin, label_pos.x, GetClientSize().GetWidth() - label_size.x - label_margin);

	int old_bg_mode = dc.GetBackgroundMode();
	dc.SetBackgroundMode(wxTRANSPARENT);

	// Draw border
	dc.SetTextForeground(wxColour(64, 64, 64));
	dc.DrawText(wx_label, label_pos.x+1, label_pos.y+1);
	dc.DrawText(wx_label, label_pos.x+1, label_pos.y-1);
	dc.DrawText(wx_label, label_pos.x-1, label_pos.y+1);
	dc.DrawText(wx_label, label_pos.x-1, label_pos.y-1);

	// Draw fill
	dc.SetTextForeground(*wxWHITE);
	dc.DrawText(wx_label, label_pos.x, label_pos.y);
	dc.SetBackgroundMode(old_bg_mode);

	label_pos.x -= label_margin;
	label_pos.y -= label_margin;
	label_size.IncBy(label_margin * 2, label_margin * 2);
	track_cursor_label_rect.SetPosition(label_pos);
	track_cursor_label_rect.SetSize(label_size);
}

void AudioDisplay::SetDraggedObject(AudioDisplayInteractionObject *new_obj)
{
	dragged_object = new_obj;
	// Keep interactive prefetch enabled during drag so that scrolled-into
	// regions are populated by the background thread and don't appear black.
	// SetInteractivePrefetchEnabled(!dragged_object);

	if (dragged_object && !HasCapture())
		CaptureMouse();
	else if (!dragged_object && HasCapture())
		ReleaseMouse();

	if (!dragged_object)
		audio_marker.reset();

	if (!dragged_object && audio_renderer_provider && audio_renderer_provider->AllowsPlaceholder()) {
		HintVisibleAudioRange();
		WarmVisibleAudioCache();
		InvalidateContentBacking();
		QueueHighFrequencyRefresh(nullptr, false);
	}
}

wxFont AudioDisplay::MakeAudioLabelFont(wxDC &dc, int point_size_delta, bool bold) const {
	wxFont font = dc.GetFont();
	wxString face_name = ResolveAudioLabelFontFace();
	if (!face_name.empty())
		font.SetFaceName(face_name);
	if (point_size_delta != 0)
		font.SetPointSize(std::max(1, font.GetPointSize() + point_size_delta));
	font.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
	return font;
}

wxString AudioDisplay::FormatTrackCursorLabel(int absolute_pos) const {
	if (absolute_pos < 0)
		return wxString();
	return to_wx(FormatTimeForDisplay(TimeFromAbsoluteX(absolute_pos), ReadAudioCursorTimeDisplayMode()));
}

void AudioDisplay::SetTrackCursor(int new_pos, bool show_time, bool follows_mouse)
{
	const int old_pos = track_cursor_pos;
	const wxRect old_label_rect = track_cursor_label_rect;
	const wxString old_label = track_cursor_label;
	const bool old_label_visible = old_pos >= 0 && !old_label.empty();
	const bool new_label_visible = show_time && new_pos >= 0;
	wxString new_label;
	int current_video_marker_pos = -1;
	if (IsAudioCursorDebugLogEnabled())
		current_video_marker_pos = GetCurrentVideoMarkerPos();

	LogAudioCursorDebug("set_track_cursor.begin", [&](std::ostringstream& out) {
		out << " old_pos=" << old_pos
			<< " new_pos=" << new_pos
			<< " show_time=" << show_time
			<< " follows_mouse_new=" << follows_mouse
			<< " follows_mouse_old=" << track_cursor_follows_mouse
			<< " old_label_visible=" << old_label_visible
			<< " new_label_visible=" << new_label_visible
			<< " scroll_left=" << scroll_left
			<< " video_marker_abs=" << current_video_marker_pos
			<< " playing=" << (controller && controller->IsPlaying());
	});

	if (old_pos == new_pos) {
		if (!old_label_visible && !new_label_visible) {
			LogAudioCursorDebug("set_track_cursor.skip", [&](std::ostringstream& out) {
				out << " reason=no_visible_change same_pos=" << new_pos;
			});
			return;
		}
		if (new_label_visible)
			new_label = FormatTrackCursorLabel(new_pos);
		if (old_label_visible == new_label_visible && (!new_label_visible || new_label == old_label)) {
			track_cursor_follows_mouse = follows_mouse;
			LogAudioCursorDebug("set_track_cursor.skip", [&](std::ostringstream& out) {
				out << " reason=same_label_state same_pos=" << new_pos
					<< " follows_mouse=" << follows_mouse;
			});
			return;
		}
	}
	else if (!AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(old_pos, new_pos)) {
		LogAudioCursorDebug("set_track_cursor.skip", [&](std::ostringstream& out) {
			out << " reason=planner_skip old_pos=" << old_pos
				<< " new_pos=" << new_pos;
		});
		return;
	}

	if (new_label_visible && new_label.empty())
		new_label = FormatTrackCursorLabel(new_pos);

	track_cursor_pos = new_pos;
	track_cursor_follows_mouse = follows_mouse;

	if (new_label_visible)
	{
		track_cursor_label = new_label;
	}
	else
	{
		track_cursor_label_rect.SetSize(wxSize(0,0));
		track_cursor_label.Clear();
	}

	auto calc_label_rect = [this]() {
		if (track_cursor_pos < 0 || track_cursor_label.empty())
			return wxRect();

		wxClientDC dc(GetWindow());
		wxFont font = MakeAudioLabelFont(dc);
		dc.SetFont(font);

		wxSize label_size(dc.GetTextExtent(track_cursor_label));
		int label_margin = FromDIP(2);
		wxPoint label_pos(track_cursor_pos - scroll_left - label_size.x/2, audio_top + label_margin);
		label_pos.x = mid(label_margin, label_pos.x, GetClientSize().GetWidth() - label_size.x - label_margin);

		label_pos.x -= label_margin;
		label_pos.y -= label_margin;
		label_size.IncBy(label_margin * 2, label_margin * 2);
		return wxRect(label_pos, label_size);
	};

	const wxRect new_label_rect = calc_label_rect();
	track_cursor_label_rect = new_label_rect;

	// Queue a narrow repaint around old/new cursor and label regions to keep
	// cursor updates smooth without repainting the entire audio display.
	auto const dirty = AudioDisplayInvalidationPlanner::PlanTrackCursorDirtyRect(
		old_pos,
		track_cursor_pos,
		ToPlannerRect(old_label_rect),
		ToPlannerRect(new_label_rect),
		scroll_left,
		audio_top,
		audio_height);
	if (!dirty.IsEmpty()) {
		wxRect rect = ToWxRect(dirty);
#ifdef WITH_SKIA
		if (IsGpuSkiaBackendActive())
			rect = wxRect(0, audio_top, GetClientSize().GetWidth(), audio_height);
#endif
		char const* refresh_mode = "immediate";
		if (dragged_object)
		{
			refresh_mode = "dragged_update";
			QueueHighFrequencyRefresh(&rect, true);
		}
		else if (controller && controller->IsPlaying())
		{
			refresh_mode = "playing_deferred";
			QueueHighFrequencyRefresh(&rect, false);
		}
		else {
			bool prefer_deferred_refresh = false;
#ifdef WITH_SKIA
			prefer_deferred_refresh = defer_immediate_track_cursor_refresh && IsGpuSkiaBackendActive();
#endif
			if (prefer_deferred_refresh) {
				refresh_mode = "deferred";
				QueueHighFrequencyRefresh(&rect, false);
			}
			else {
				RequestPaint(&rect);
				Update();
			}
		}
		LogAudioCursorDebug("set_track_cursor.refresh", [&](std::ostringstream& out) {
			AppendRect(out, "dirty", rect);
			out << " refresh_mode=" << refresh_mode
				<< " cursor_abs=" << track_cursor_pos
				<< " cursor_rel=" << (track_cursor_pos - scroll_left)
				<< " label_visible=" << !track_cursor_label.empty();
		});
	}
	else {
		LogAudioCursorDebug("set_track_cursor.no_dirty", [&](std::ostringstream& out) {
			out << " cursor_abs=" << track_cursor_pos
				<< " cursor_rel=" << (track_cursor_pos - scroll_left);
		});
	}
}

void AudioDisplay::OnPlaybackStop()
{
	defer_immediate_track_cursor_refresh = true;
	RemoveTrackCursor();
	defer_immediate_track_cursor_refresh = false;
}

void AudioDisplay::RemoveTrackCursor()
{
	SetTrackCursor(-1, false, false);
}

void AudioDisplay::SyncTrackCursorToMouseAfterScroll() {
	if (!controller || controller->IsPlaying() || track_cursor_pos < 0)
		return;
	if (!track_cursor_follows_mouse)
		return;

	wxWindow *window = GetWindow();
	if (!window || !window->IsShownOnScreen())
		return;

	wxPoint const mouse_pos = window->ScreenToClient(wxGetMousePosition());
	if (!GetClientRect().Contains(mouse_pos))
		return;

	SetTrackCursor(
		scroll_left + mouse_pos.x,
		OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool(),
		true);
}

bool AudioDisplay::TryGetCurrentVideoMarker(int &out_pos, int &out_frame) const {
	out_pos = -1;
	out_frame = -1;
	if (!provider || !context || ms_per_pixel <= 0.0)
		return false;
	if (!OPT_GET("Audio/Display/Draw/Video Position")->GetBool())
		return false;

	auto core = context->GetCore();
	if (!core.videoController)
		return false;

	const int frame = core.videoController->GetFrameN();
	if (frame < 0)
		return false;

	out_frame = frame;
	out_pos = AbsoluteXFromTime(core.videoController->TimeAtFrame(frame));
	return out_pos >= 0;
}

int AudioDisplay::GetCurrentVideoMarkerPos() const {
	int pos = -1;
	int frame = -1;
	if (!TryGetCurrentVideoMarker(pos, frame))
		return -1;
	return pos;
}

wxRect AudioDisplay::GetMarkerRefreshRect(int absolute_x) const {
	if (absolute_x < 0 || audio_height <= 0)
		return wxRect();

	const int padding = FromDIP(foot_size) + FromDIP(4);
	return wxRect(absolute_x - scroll_left - padding, audio_top, padding * 2 + 1, audio_height);
}

bool AudioDisplay::QueueDynamicVideoMarkerRefresh() {
	if (!provider || !context || audio_marker)
		return false;

	int new_pos = -1;
	int new_frame = -1;
	if (!TryGetCurrentVideoMarker(new_pos, new_frame)) {
		LogAudioCursorDebug("video_marker.refresh", [&](std::ostringstream& out) {
			out << " reason=no_current_video_marker"
				<< " last_pos=" << last_video_marker_pos
				<< " last_frame=" << last_video_marker_frame;
		});
		last_video_marker_pos = -1;
		last_video_marker_frame = -1;
		return false;
	}

	// If the video marker didn't move, don't swallow this marker update. It may
	// have come from a different marker provider (e.g. toggling keyframes).
	if (new_frame == last_video_marker_frame && new_pos == last_video_marker_pos) {
		LogAudioCursorDebug("video_marker.refresh", [&](std::ostringstream& out) {
			out << " reason=unchanged"
				<< " pos=" << new_pos
				<< " frame=" << new_frame;
		});
		return false;
	}

	wxRect dirty;
	int const old_pos = last_video_marker_pos;
	int const old_frame = last_video_marker_frame;
	if (new_pos != last_video_marker_pos) {
		auto const planned = AudioDisplayInvalidationPlanner::PlanMarkerMoveDirtyRect(
			ToPlannerRect(GetMarkerRefreshRect(last_video_marker_pos)),
			ToPlannerRect(GetMarkerRefreshRect(new_pos)));
		if (!planned.IsEmpty())
			dirty = ToWxRect(planned);
	}

	last_video_marker_pos = new_pos;
	last_video_marker_frame = new_frame;
	if (!dirty.IsEmpty())
		QueueHighFrequencyRefresh(&dirty, true);
	LogAudioCursorDebug("video_marker.refresh", [&](std::ostringstream& out) {
		out << " reason=moved"
			<< " old_pos=" << old_pos
			<< " new_pos=" << new_pos
			<< " old_frame=" << old_frame
			<< " new_frame=" << new_frame;
		if (!dirty.IsEmpty())
			AppendRect(out, "dirty", dirty);
	});
	return true;
}

void AudioDisplay::OnMouseEnter(wxMouseEvent& event)
{
	if (OPT_GET("Audio/Auto/Focus")->GetBool())
		SetFocus();

	// Restore the track cursor at the current mouse position so that it
	// reappears immediately after an alt-tab / minimize-restore cycle
	// (OnMouseLeave removes it when the window loses focus).
	if (!controller->IsPlaying())
		SetTrackCursor(
			scroll_left + event.GetPosition().x,
			OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool(),
			true);
}

void AudioDisplay::OnMouseLeave(wxMouseEvent&)
{
	if (!controller->IsPlaying())
		RemoveTrackCursor();
}

void AudioDisplay::ScheduleMiddleScrubSeek(int target_ms, bool force) {
	if (!context || !context->videoController)
		return;

	constexpr auto min_interval = std::chrono::milliseconds(33);
	const int target_frame = context->videoController->FrameAtTime(target_ms, agi::vfr::EXACT);
	const int current_frame = context->videoController->GetFrameN();

	if (force) {
		if (middle_scrub_seek_timer.IsRunning())
			middle_scrub_seek_timer.Stop();
		middle_scrub_pending_seek_frame = -1;

		if (target_frame != current_frame) {
			context->videoController->JumpToFrame(target_frame);
		}
		middle_scrub_last_seek_time = std::chrono::steady_clock::now();
		return;
	}

	if (target_frame == current_frame) {
		if (middle_scrub_pending_seek_frame != -1) {
			middle_scrub_pending_seek_frame = -1;
			if (middle_scrub_seek_timer.IsRunning())
				middle_scrub_seek_timer.Stop();
		}
		return;
	}

	auto const now = std::chrono::steady_clock::now();
	auto const elapsed = now - middle_scrub_last_seek_time;
	if (elapsed >= min_interval) {
		if (middle_scrub_seek_timer.IsRunning())
			middle_scrub_seek_timer.Stop();
		middle_scrub_pending_seek_frame = -1;
		context->videoController->PreviewToFrame(target_frame);
		middle_scrub_last_seek_time = now;
		return;
	}

	middle_scrub_pending_seek_frame = target_frame;
	if (!middle_scrub_seek_timer.IsRunning()) {
		auto const remaining = min_interval - elapsed;
		const int delay_ms = static_cast<int>(std::max<int64_t>(
			1, std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()));
		middle_scrub_seek_timer.Start(delay_ms, true);
	}
}

void AudioDisplay::OnMiddleScrubSeekTimer(wxTimerEvent &) {
	if (!context || !context->videoController)
		return;
	if (middle_scrub_pending_seek_frame < 0)
		return;
	if (middle_scrub_pending_seek_frame == context->videoController->GetFrameN()) {
		middle_scrub_pending_seek_frame = -1;
		return;
	}

	auto const now = std::chrono::steady_clock::now();
	context->videoController->PreviewToFrame(middle_scrub_pending_seek_frame);
	middle_scrub_last_seek_time = now;
	middle_scrub_pending_seek_frame = -1;
}

void AudioDisplay::OnMouseEvent(wxMouseEvent& event)
{
	// If we have focus, we get mouse move events on Mac even when the mouse is
	// outside our client rectangle, we don't want those.
	if (event.Moving() && !GetClientRect().Contains(event.GetPosition()))
	{
		event.Skip();
		return;
	}

	if (event.IsButton())
		SetFocus();

	const bool scrub_end_event = event.MiddleUp() || (middle_scrub_seek_active && !event.MiddleIsDown());
	if (event.MiddleDown())
		middle_scrub_seek_active = true;
	else if (scrub_end_event)
		middle_scrub_seek_active = false;

	const int mouse_x = event.GetPosition().x;
	if (scrub_end_event) {
		ScheduleMiddleScrubSeek(TimeFromRelativeX(mouse_x), true);
		return;
	}

	// Scroll the display after a mouse-up near one of the edges
	if ((event.LeftUp() || event.RightUp()) && OPT_GET("Audio/Auto/Scroll")->GetBool())
	{
		const int width = GetClientSize().GetWidth();
		if (mouse_x < width / 20) {
			ScrollBy(-width / 3);
		}
		else if (width - mouse_x < width / 20) {
			ScrollBy(width / 3);
		}
	}

	if (ForwardMouseEvent(event))
		return;

	if (event.MiddleIsDown())
	{
		SetTrackCursor(
			scroll_left + mouse_x,
			OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool(),
			true);
		ScheduleMiddleScrubSeek(TimeFromRelativeX(mouse_x), event.MiddleDown());
		return;
	}

	if (event.Moving() && !controller->IsPlaying())
	{
		SetTrackCursor(
			scroll_left + mouse_x,
			OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool(),
			true);
	}

	AudioTimingController *timing = controller->GetTimingController();
	if (!timing) return;
	const int drag_sensitivity = int(OPT_GET("Audio/Start Drag Sensitivity")->GetInt() * ms_per_pixel);
	const int snap_sensitivity = OPT_GET("Audio/Snap/Enable")->GetBool() != event.ShiftDown() ? int(OPT_GET("Audio/Snap/Distance")->GetInt() * ms_per_pixel) : 0;

	// Not scrollbar, not timeline, no button action
	if (event.Moving())
	{
		const int timepos = TimeFromRelativeX(mouse_x);

		if (timing->IsNearbyMarker(timepos, drag_sensitivity, event.AltDown()))
			SetCursor(wxCursor(wxCURSOR_SIZEWE));
		else
			SetCursor(wxNullCursor);
		return;
	}

	const int old_scroll_pos = scroll_left;
	if (event.LeftDown() || event.RightDown())
	{
		const int timepos = TimeFromRelativeX(mouse_x);
		std::vector<AudioMarker*> markers = event.LeftDown()
			? timing->OnLeftClick(timepos, event.CmdDown(), event.AltDown(), drag_sensitivity, snap_sensitivity)
			: timing->OnRightClick(timepos, event.CmdDown(), drag_sensitivity, snap_sensitivity);

		// Clicking should never result in the audio display scrolling
		ScrollPixelToLeft(old_scroll_pos);

		if (markers.size())
		{
			RemoveTrackCursor();
			audio_marker = agi::make_unique<AudioMarkerInteractionObject>(markers, timing, this, (wxMouseButton)event.GetButton());
			SetDraggedObject(audio_marker.get());
			return;
		}
	}
}

bool AudioDisplay::ForwardMouseEvent(wxMouseEvent &event) {
	// Handle any ongoing drag
	if (dragged_object)
	{
		if (HasCapture())
		{
			if (!dragged_object->OnMouseEvent(event))
			{
				scroll_timer.Stop();
				SetDraggedObject(nullptr);
				SetCursor(wxNullCursor);
			}
			return true;
		}
		else
		{
			// Something is wrong, we might have lost capture somehow.
			// Fix state and pretend it didn't happen.
			SetDraggedObject(nullptr);
			SetCursor(wxNullCursor);
		}
	}

	const wxPoint mousepos = event.GetPosition();
	AudioDisplayInteractionObject *new_obj = nullptr;
	// Check for scrollbar action
	if (scrollbar->GetBounds().Contains(mousepos))
	{
		new_obj = scrollbar.get();
	}
	// Check for timeline action
	else if (timeline->GetBounds().Contains(mousepos))
	{
		SetCursor(wxCursor(wxCURSOR_SIZEWE));
		new_obj = timeline.get();
	}
	else
	{
		return false;
	}

	if (!controller->IsPlaying())
		RemoveTrackCursor();
	if (new_obj->OnMouseEvent(event))
		SetDraggedObject(new_obj);
	return true;
}

void AudioDisplay::OnKeyDown(wxKeyEvent& event)
{
	hotkey::check("Audio", context, event);
}

void AudioDisplay::OnSize(wxSizeEvent &)
{
	if (high_frequency_refresh_timer.IsRunning())
		high_frequency_refresh_timer.Stop();
	pending_high_frequency_full_refresh = false;
	pending_high_frequency_refresh = false;
	pending_high_frequency_update = false;
	pending_high_frequency_rect = wxRect();
	InvalidateContentBacking();

	// We changed size, update the sub-controls' internal data and redraw
	wxSize size = GetClientSize();

	timeline->SetDisplaySize(wxSize(size.x, scrollbar->GetBounds().y));
	scrollbar->SetDisplaySize(size);

	if (controller->GetTimingController())
	{
		TimeRange sel(controller->GetTimingController()->GetPrimaryPlaybackRange());
		scrollbar->SetSelection(AbsoluteXFromTime(sel.begin()), AbsoluteXFromTime(sel.length()));
	}

	audio_height = size.GetHeight();
	audio_height -= scrollbar->GetBounds().GetHeight();
	audio_height -= timeline->GetHeight();
	audio_renderer->SetHeight(audio_height);

	audio_top = timeline->GetHeight();

	HintVisibleAudioRange();
	WarmVisibleAudioCache();
	RequestPaint();
}

void AudioDisplay::OnFocus(wxFocusEvent &)
{
	// The scrollbar indicates focus so repaint that
	wxRect sb = scrollbar->GetBounds();
	RequestPaint(&sb);
}

int AudioDisplay::GetDuration() const
{
	if (!provider) return 0;
	return (provider->GetNumSamples() * 1000 + provider->GetSampleRate() - 1) / provider->GetSampleRate();
}

void AudioDisplay::ApplyAudioProvider(agi::AudioProvider *provider)
{
	this->provider = provider;
	InvalidateContentBacking();

	if (!audio_renderer_provider)
		ReloadRenderingSettings();

	audio_renderer->SetAudioProvider(provider);
	audio_renderer->SetCacheMaxSize(OPT_GET("Audio/Renderer/Spectrum/Memory Max")->GetInt() * 1024 * 1024);

	timeline->ChangeAudio(GetDuration());

	ms_per_pixel = 0;
	SetZoomLevel(zoom_level);
	{
		int pos = -1;
		int frame = -1;
		if (TryGetCurrentVideoMarker(pos, frame)) {
			last_video_marker_pos = pos;
			last_video_marker_frame = frame;
		}
		else {
			last_video_marker_pos = -1;
			last_video_marker_frame = -1;
		}
	}

	LogRenderConfiguration("audio_provider");
	RequestPaint();

	if (provider)
	{
		if (connections.empty())
		{
			auto core = context->GetCore();
			connections = agi::signal::make_vector({
				controller->AddPlaybackPositionListener(&AudioDisplay::OnPlaybackPosition, this),
				controller->AddPlaybackStopListener(&AudioDisplay::OnPlaybackStop, this),
				core.videoController->AddFramePresentedListener(&AudioDisplay::OnVideoSeek, this),
				controller->AddTimingControllerListener(&AudioDisplay::OnTimingController, this),
				OPT_SUB("Audio/Display/Draw/Cursor Time", &AudioDisplay::OnTrackCursorTimeOptionChanged, this),
				OPT_SUB("Audio/Display/Draw/Cursor Time Format", &AudioDisplay::OnTrackCursorTimeOptionChanged, this),
				OPT_SUB("Audio/Spectrum", &AudioDisplay::ReloadRenderingSettings, this),
				OPT_SUB("Audio/Display/Waveform Style", &AudioDisplay::ReloadRenderingSettings, this),
				OPT_SUB("Colour/Audio Display/Spectrum", &AudioDisplay::ReloadRenderingSettings, this),
				OPT_SUB("Colour/Audio Display/Waveform", &AudioDisplay::ReloadRenderingSettings, this),
				OPT_SUB("Audio/Renderer/Spectrum/Quality", &AudioDisplay::ReloadRenderingSettings, this),
				OPT_SUB("Audio/Renderer/Spectrum/Computation Mode", &AudioDisplay::OnSpectrumComputationModeChanged, this),
				OPT_SUB("Audio/Renderer/Spectrum/FreqCurve", &AudioDisplay::OnSpectrumFrequencyCurveChanged, this),
				OPT_SUB("Audio/Renderer/Spectrum/Mono Mix Mode", &AudioDisplay::OnSpectrumMonoMixModeChanged, this),
			});
			OnTimingController();
		}
		last_sample_decoded = provider->GetDecodedSamples();
		audio_load_position = -1;
		audio_load_speed = 0;
		audio_load_start_time = std::chrono::steady_clock::now();
		if (last_sample_decoded != provider->GetNumSamples())
			load_timer.Start(100);
	}
	else
	{
		connections.clear();
	}
}

void AudioDisplay::OnAudioOpen(agi::AudioProvider *provider)
{
	ApplyAudioProvider(provider);
}

void AudioDisplay::OnTimingController()
{
	AudioTimingController *timing_controller = controller->GetTimingController();
	if (timing_controller)
	{
		timing_controller->AddMarkerMovedListener(&AudioDisplay::OnMarkerMoved, this);
		timing_controller->AddUpdatedPrimaryRangeListener(&AudioDisplay::OnSelectionChanged, this);
		timing_controller->AddUpdatedStyleRangesListener(&AudioDisplay::OnStyleRangesChanged, this);

		OnStyleRangesChanged();
		OnMarkerMoved();
		OnSelectionChanged();
	}
}

void AudioDisplay::OnPlaybackPosition(int ms)
{
	if (middle_scrub_seek_active)
		return;

	int pixel_position = AbsoluteXFromTime(ms);
	LogAudioCursorDebug("playback_position", [&](std::ostringstream& out) {
		out << " ms=" << ms
			<< " pixel_position=" << pixel_position
			<< " scroll_left=" << scroll_left
			<< " lock_scroll=" << OPT_GET("Audio/Lock Scroll on Cursor")->GetBool();
	});
	SetTrackCursor(pixel_position, false, false);

	if (OPT_GET("Audio/Lock Scroll on Cursor")->GetBool())
	{
		int client_width = GetClientSize().GetWidth();
		int edge_size = client_width / 20;
		if (scroll_left > 0 && pixel_position < scroll_left + edge_size)
		{
			ScrollPixelToLeft(std::max(pixel_position - edge_size, 0));
		}
		else if (scroll_left + client_width < std::min(pixel_audio_width - 1, pixel_position + edge_size))
		{
			ScrollPixelToLeft(std::min(pixel_position - client_width + edge_size, pixel_audio_width - client_width - 1));
		}
	}
}

void AudioDisplay::OnVideoSeek(int frame)
{
	if (!provider || !context || controller->IsPlaying())
		return;

	auto core = context->GetCore();
	if (!core.project->VideoProvider())
		return;

	// During continuous playback, the audio transport owns the main track cursor.
	// Video seek only drives paused-state navigation feedback.
	const int ms = core.videoController->TimeAtFrame(frame, agi::vfr::EXACT);
	const bool show_time = middle_scrub_seek_active && OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool();
	LogAudioCursorDebug("video_seek", [&](std::ostringstream& out) {
		out << " frame=" << frame
			<< " ms=" << ms
			<< " show_time=" << show_time
			<< " scroll_left=" << scroll_left
			<< " middle_scrub_seek_active=" << middle_scrub_seek_active;
	});
	SetTrackCursor(AbsoluteXFromTime(ms), show_time, false);
}

void AudioDisplay::OnTrackCursorTimeOptionChanged(agi::OptionValue const& opt) {
	(void)opt;
	if (!controller || controller->IsPlaying())
		return;
	if (track_cursor_pos < 0)
		return;
	SetTrackCursor(
		track_cursor_pos,
		OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool(),
		track_cursor_follows_mouse);
}

void AudioDisplay::OnSelectionChanged()
{
	TimeRange sel(controller->GetPrimaryPlaybackRange());
	scrollbar->SetSelection(AbsoluteXFromTime(sel.begin()), AbsoluteXFromTime(sel.length()));

	if (audio_marker)
	{
		if (!scroll_timer.IsRunning())
		{
			// If the dragged object is outside the visible area, start the
			// scroll timer to shift it back into view
			int rel_x = RelativeXFromTime(audio_marker->GetPosition());
			if (rel_x < 0 || rel_x >= GetClientSize().GetWidth())
			{
				// 50ms is the default for this on Windows (hardcoded since
				// wxSystemSettings doesn't expose DragScrollDelay etc.)
				scroll_timer.Start(50, true);
			}
		}
	}
	else if (OPT_GET("Audio/Auto/Scroll")->GetBool() && sel.end() != 0)
	{
		ScrollTimeRangeInView(sel);
	}

	if (audio_marker)
		QueueHighFrequencyRefresh(&scrollbar->GetBounds(), true);
	else {
		wxRect sb = scrollbar->GetBounds();
		RequestPaint(&sb);
	}
}

void AudioDisplay::OnScrollTimer(wxTimerEvent &event)
{
	if (!audio_marker) return;

	int rel_x = RelativeXFromTime(audio_marker->GetPosition());
	int width = GetClientSize().GetWidth();

	// If the dragged object is outside the visible area, scroll it into
	// view with a 5% margin
	if (rel_x < 0)
	{
		ScrollBy(rel_x - width / 20);
	}
	else if (rel_x >= width)
	{
		ScrollBy(rel_x - width + width / 20);
	}
}

void AudioDisplay::OnStyleRangesChanged()
{
	if (!controller->GetTimingController()) return;

	AudioStyleRangeMerger asrm;
	controller->GetTimingController()->GetRenderingStyles(asrm);

	style_ranges.clear();
	for (auto pair : asrm) style_ranges.push_back(pair);

	InvalidateContentBacking();
	const wxRect audio_rect(0, audio_top, GetClientSize().GetWidth(), audio_height);
	if (audio_marker)
		QueueHighFrequencyRefresh(&audio_rect, true);
	else
		RequestPaint(&audio_rect);
}

void AudioDisplay::OnMarkerMoved()
{
	bool const handled_dynamic_video = QueueDynamicVideoMarkerRefresh();
	LogAudioCursorDebug("markers_changed", [&](std::ostringstream& out) {
		out << " handled_dynamic_video=" << handled_dynamic_video
			<< " track_cursor_pos=" << track_cursor_pos
			<< " scroll_left=" << scroll_left;
	});
	if (handled_dynamic_video)
		return;

	{
		int pos = -1;
		int frame = -1;
		if (TryGetCurrentVideoMarker(pos, frame)) {
			last_video_marker_pos = pos;
			last_video_marker_frame = frame;
		}
		else {
			last_video_marker_pos = -1;
			last_video_marker_frame = -1;
		}
	}
	const wxRect audio_rect(0, audio_top, GetClientSize().GetWidth(), audio_height);
	if (audio_marker)
		QueueHighFrequencyRefresh(&audio_rect, true);
	else
		RequestPaint(&audio_rect);
}
