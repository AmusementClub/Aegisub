#include "skia_audio_display.h"

#include "skia_audio_content_worker.h"
#include "skia_audio_presenter.h"

#include "../../include/aegisub/context.h"
#include "../../project.h"
#include "../../audio_controller.h"
#include "../../audio_colorscheme.h"
#include "../../audio_renderer_spectrum.h"
#include "../../audio_timing.h"
#include "../../include/aegisub/hotkey.h"
#include "../../navigation_preview_policy.h"
#include "../../options.h"
#include "../../video_controller.h"

#include <libaegisub/signal.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/ass/time.h>
#include <libaegisub/color.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <wx/dcclient.h>
#include <wx/mousestate.h>
#include <wx/thread.h>
#include <wx/timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace aegisub::skia::audio {
namespace {

wxDEFINE_EVENT(EVT_SKIA_AUDIO_CONTENT_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_SKIA_AUDIO_CONTENT_FAILURE, wxThreadEvent);

std::uint32_t ToArgb(wxColour const& color) {
	return 0xFF000000u
		| (static_cast<std::uint32_t>(color.Red()) << 16)
		| (static_cast<std::uint32_t>(color.Green()) << 8)
		| static_cast<std::uint32_t>(color.Blue());
}

std::uint32_t ToArgb(agi::Color const& color) {
	return 0xFF000000u
		| (static_cast<std::uint32_t>(color.r) << 16)
		| (static_cast<std::uint32_t>(color.g) << 8)
		| static_cast<std::uint32_t>(color.b);
}

class StyleRangeCollector final : public AudioRenderingStyleRanges {
public:
	std::vector<TimeStyleRange> ranges;

	void AddRange(int start, int end, AudioRenderingStyle style) override {
		if (end <= start || style < AudioStyle_Normal || style >= AudioStyle_MAX)
			return;
		ranges.push_back({ std::max(0, start), std::max(0, end), static_cast<FrameStyle>(style) });
	}
};

int ProviderDurationMs(agi::AudioProvider const *provider) {
	if (!provider || provider->GetSampleRate() <= 0)
		return 0;
	auto const duration = static_cast<long double>(provider->GetNumSamples())
		* 1000.0L / provider->GetSampleRate();
	if (!std::isfinite(static_cast<double>(duration)))
		return 0;
	return static_cast<int>(std::clamp<long double>(duration, 0, std::numeric_limits<int>::max()));
}

int SpectrumQuality() {
	int quality = OPT_GET("Audio/Renderer/Spectrum/Quality")->GetInt();
#if defined(WITH_PFFFT) || defined(WITH_FFTW3)
	quality += 2;
#endif
	return std::clamp(quality, 0, 5);
}

std::pair<std::size_t, std::size_t> SpectrumResolution() {
	constexpr std::size_t widths[] = { 8, 9, 9, 9, 10, 11 };
	constexpr std::size_t distances[] = { 8, 8, 7, 6, 6, 5 };
	auto const quality = SpectrumQuality();
	return { widths[quality], distances[quality] };
}

#if wxCHECK_VERSION(3, 1, 1)
int gl_attributes[] = {
	WX_GL_RGBA,
	WX_GL_DOUBLEBUFFER,
	WX_GL_STENCIL_SIZE, 8,
	WX_GL_BUFFER_SIZE, 24,
	WX_GL_MIN_ALPHA, 8,
	0,
};
#else
int gl_attributes[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

}

struct SkiaAudioDisplay::Impl {
	Impl(
		SkiaAudioDisplay *owner,
		AudioController *audio_controller,
		agi::Context *project_context,
		FailureInjection failure_injection,
		FailureCallback failure_callback)
	: presenter(std::make_unique<Presenter>(failure_injection))
	, content_worker([owner](ContentGeneration) {
		wxQueueEvent(owner, new wxThreadEvent(EVT_SKIA_AUDIO_CONTENT_READY));
	}, [owner](std::string message) {
		auto *event = new wxThreadEvent(EVT_SKIA_AUDIO_CONTENT_FAILURE);
		event->SetString(wxString::FromUTF8(message));
		wxQueueEvent(owner, event);
	})
	, failure_callback(std::move(failure_callback)) {
		this->audio_controller = audio_controller;
		this->project_context = project_context;
		load_timer.SetOwner(owner);
		middle_seek_timer.SetOwner(owner);
	}

	std::unique_ptr<wxGLContext> context;
	std::unique_ptr<Presenter> presenter;
	ContentWorker content_worker;
	agi::signal::Connection audio_open_connection;
	agi::signal::Connection playback_position_connection;
	agi::signal::Connection playback_stop_connection;
	agi::signal::Connection timing_controller_connection;
	std::vector<agi::signal::Connection> timing_connections;
	std::vector<agi::signal::Connection> option_connections;
	agi::Context *project_context = nullptr;
	agi::AudioProvider *provider = nullptr;
	AudioController *audio_controller = nullptr;
	ContentAnalysisConfig content_analysis;
	ContentGeneration content_generation;
	ContentViewportRequest last_content_request;
	bool has_last_content_request = false;
	FrameViewport viewport;
	int zoom_level = 0;
	int scroll_left = 0;
	float amplitude_scale = 1.f;
	std::uint64_t presentation_revision = 1;
	std::array<std::array<std::uint32_t, 4>, AudioStyle_MAX> waveform_style_colors {};
	bool presentation_colors_ready = false;
	std::array<std::shared_ptr<SpectrumPalette const>, AudioStyle_MAX> spectrum_style_palettes;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	FailureCallback failure_callback;
	wxTimer load_timer;
	wxTimer middle_seek_timer;
	NavigationPreviewPolicy middle_seek_policy;
	std::int64_t last_decoded_samples = 0;
	std::uint64_t context_generation = 0;
	bool fallback_requested = false;
	int playback_position_ms = -1;
	int mouse_position_ms = -1;
	std::vector<AudioMarker *> dragged_markers;
	wxMouseButton dragged_button = wxMOUSE_BTN_NONE;
	bool timeline_dragging = false;
	bool scrollbar_dragging = false;
	bool middle_seek_active = false;
	int drag_last_x = 0;

	SkiaGlContextToken ContextToken() const noexcept {
		return { context.get(), context_generation };
	}

	void InvalidatePresentation() {
		++presentation_revision;
		if (!presentation_revision)
			++presentation_revision;
		for (auto& palette : spectrum_style_palettes)
			palette.reset();
		spectrum_band_plan.reset();
		presentation_colors_ready = false;
	}
};

SkiaAudioDisplay::SkiaAudioDisplay(
	wxWindow *parent,
	AudioController *controller,
	agi::Context *context,
	FailureInjection failure_injection,
	FailureCallback failure_callback)
: wxGLCanvas(parent, wxID_ANY, gl_attributes, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
, impl(std::make_unique<Impl>(this, controller, context, failure_injection, std::move(failure_callback)))
{
	impl->project_context = context;
	impl->audio_open_connection = context->GetCore().project->AddAudioProviderListener(
		&SkiaAudioDisplay::OnAudioOpen,
		this);
	impl->playback_position_connection = controller->AddPlaybackPositionListener(
		&SkiaAudioDisplay::OnPlaybackPosition,
		this);
	impl->playback_stop_connection = controller->AddPlaybackStopListener(
		&SkiaAudioDisplay::OnPlaybackStop,
		this);
	impl->timing_controller_connection = controller->AddTimingControllerListener(
		&SkiaAudioDisplay::OnTimingControllerChanged,
		this);
	impl->option_connections = agi::signal::make_vector({
		OPT_SUB("Audio/Spectrum", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Quality", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Input Format", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Computation Mode", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/FreqCurve", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Mono Mix Mode", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Display/Waveform Style", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Colour/Audio Display/Spectrum", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Colour/Audio Display/Waveform", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
	});
	SetMinClientSize(wxSize(-1, 70));
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetThemeEnabled(false);
	Bind(wxEVT_PAINT, &SkiaAudioDisplay::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &SkiaAudioDisplay::OnEraseBackground, this);
	Bind(wxEVT_SIZE, &SkiaAudioDisplay::OnSize, this);
	Bind(EVT_SKIA_AUDIO_CONTENT_READY, &SkiaAudioDisplay::OnContentReady, this);
	Bind(EVT_SKIA_AUDIO_CONTENT_FAILURE, &SkiaAudioDisplay::OnContentFailure, this);
	Bind(wxEVT_TIMER, &SkiaAudioDisplay::OnLoadTimer, this, impl->load_timer.GetId());
	Bind(wxEVT_TIMER, &SkiaAudioDisplay::OnMiddleSeekTimer, this, impl->middle_seek_timer.GetId());
	Bind(wxEVT_LEFT_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_RIGHT_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_RIGHT_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_ENTER_WINDOW, &SkiaAudioDisplay::OnMouseEnter, this);
	Bind(wxEVT_LEAVE_WINDOW, &SkiaAudioDisplay::OnMouseLeave, this);
	Bind(wxEVT_MOUSE_CAPTURE_LOST, &SkiaAudioDisplay::OnMouseCaptureLost, this);
	Bind(wxEVT_SET_FOCUS, &SkiaAudioDisplay::OnFocus, this);
	Bind(wxEVT_KILL_FOCUS, &SkiaAudioDisplay::OnFocus, this);
	Bind(wxEVT_KEY_DOWN, &SkiaAudioDisplay::OnKeyDown, this);
	OnTimingControllerChanged();
}

SkiaAudioDisplay::~SkiaAudioDisplay() {
	if (!impl || !impl->presenter)
		return;
	if (impl->context && impl->context->IsOK() && SetCurrent(*impl->context))
		impl->presenter->Release(impl->ContextToken());
	else
		impl->presenter->Abandon();
	impl->presenter.reset();
	impl->context.reset();
}

void SkiaAudioDisplay::RebuildViewport() {
	if (!impl)
		return;
	auto const size = GetClientSize();
	int text_width = 0;
	int text_height = 0;
	GetTextExtent(wxS("0123456789:."), &text_width, &text_height);
	FrameViewportRequest request;
	request.logical_width = size.GetWidth();
	request.logical_height = size.GetHeight();
	request.content_scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	request.timeline_height = text_height + 4;
	request.scrollbar_height = 15;
	request.scroll_left = impl->scroll_left;
	request.duration_ms = ProviderDurationMs(impl->provider);
	request.milliseconds_per_logical_pixel =
		AudioMillisecondsPerLogicalPixel(impl->zoom_level);
	impl->viewport = BuildFrameViewport(request);
	if (impl->viewport.IsValid())
		impl->scroll_left = impl->viewport.scroll_left;
}

void SkiaAudioDisplay::ReconfigureAnalysis() {
	if (!impl)
		return;
	ContentAnalysisConfig config;
	if (OPT_GET("Audio/Spectrum")->GetBool()) {
		config.kind = ContentKind::Spectrum;
		auto const input_format = std::clamp<int>(
			static_cast<int>(OPT_GET("Audio/Renderer/Spectrum/Input Format")->GetInt()),
			0,
			1);
		config.source_mode = input_format == 0
			? ContentSourceMode::Int16Mono
			: ContentSourceMode::FloatInterleaved;
		auto const [derivation_size, derivation_distance] = SpectrumResolution();
		config.spectrum_derivation_size = derivation_size;
		config.spectrum_derivation_distance = derivation_distance;
		config.spectrum_channel_mode = SpectrumChannelMode::MixedMono;
		if (input_format == 1) {
			auto const mono_mode = std::clamp<int>(
				static_cast<int>(OPT_GET("Audio/Renderer/Spectrum/Mono Mix Mode")->GetInt()),
				0,
				2);
			config.spectrum_channel_mode = static_cast<SpectrumChannelMode>(mono_mode);
		}
	}
	else {
		config.kind = ContentKind::Waveform;
		config.source_mode = ContentSourceMode::Int16Mono;
	}
	auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	config.milliseconds_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level) / scale;
	config.mix_policy = AudioMixPolicy::MonoAverage;
	impl->content_analysis = config;
	auto const old_generation = impl->content_generation;
	impl->content_generation = impl->content_worker.SetAnalysis(config);
	if (impl->content_generation != old_generation)
		impl->has_last_content_request = false;
	for (auto& palette : impl->spectrum_style_palettes)
		palette.reset();
	impl->spectrum_band_plan.reset();
	RebuildViewport();
	RequestVisibleContent();
}

void SkiaAudioDisplay::RequestVisibleContent() {
	if (!impl || !impl->provider || !impl->viewport.IsValid() || !impl->content_analysis.IsValid())
		return;
	ContentViewportRequest request;
	request.generation = impl->content_generation;
	request.kind = impl->content_analysis.kind;
	request.first_column = impl->viewport.first_column;
	request.column_count = impl->viewport.visible_column_count;
	request.tile_column_count = 256;
	if (request.kind == ContentKind::Spectrum)
		request.spectrum_bin_count = static_cast<std::uint32_t>(
			std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
	if (impl->has_last_content_request && impl->last_content_request == request)
		return;
	impl->last_content_request = request;
	impl->has_last_content_request = true;
	impl->content_worker.Request(request);
}

void SkiaAudioDisplay::OnRenderingSettingsChanged() {
	impl->InvalidatePresentation();
	ReconfigureAnalysis();
	Refresh(false);
}

void SkiaAudioDisplay::OnPaint(wxPaintEvent&) try {
	wxPaintDC paint_dc(this);
	if (impl->fallback_requested)
		return;

	auto const logical_size = GetClientSize();
	if (logical_size.GetWidth() <= 0 || logical_size.GetHeight() <= 0)
		return;
	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;

	if (!impl->context) {
		impl->context = std::make_unique<wxGLContext>(this);
		++impl->context_generation;
		if (!impl->context_generation)
			++impl->context_generation;
	}

	auto const context = impl->ContextToken();
	if (!impl->context->IsOK() || !SetCurrent(*impl->context)) {
		impl->presenter->Fail(
			context,
			SkiaGlDeviceFailure::ContextActivationFailed,
			"wxGLCanvas could not activate the Audio Display context");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
		return;
	}

	FrameTarget target;
	target.context_generation = context.generation;
	target.width = impl->viewport.target_width;
	target.height = impl->viewport.target_height;
	target.sample_count = 0;
	GLint stencil_bits = 0;
	glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
	target.stencil_bits = std::max(0, static_cast<int>(stencil_bits));
	target.framebuffer_id = 0;
	target.bottom_left_origin = true;

	if (!impl->provider) {
		if (!impl->presenter->RenderDiagnosticFrame(context, target)) {
			RequestFallback(impl->presenter->TakeFailureLogMessage());
			return;
		}
	}
	else {
		RequestVisibleContent();
		ContentFrame frame;
		frame.generation = impl->content_generation;
		frame.kind = impl->content_analysis.kind;
		frame.first_column = impl->viewport.first_column;
		frame.x = static_cast<float>(impl->viewport.content.x);
		frame.y = static_cast<float>(impl->viewport.content.y);
		frame.width = static_cast<float>(impl->viewport.content.width);
		frame.height = static_cast<float>(impl->viewport.content.height);
		frame.first_column_offset = static_cast<float>(impl->viewport.first_column_offset);
		frame.amplitude = impl->amplitude_scale;
		auto timeline = std::make_shared<TimelineFrame>();
		timeline->y = impl->viewport.timeline.y;
		timeline->height = impl->viewport.timeline.height;
		timeline->scroll_left = impl->viewport.scroll_left;
		timeline->scroll_left_exact = impl->viewport.first_column_exact;
		timeline->duration_ms = ProviderDurationMs(impl->provider);
		timeline->milliseconds_per_pixel = impl->viewport.milliseconds_per_column;
		auto const scheme_name = OPT_GET(frame.kind == ContentKind::Spectrum
		? "Colour/Audio Display/Spectrum" : "Colour/Audio Display/Waveform")->GetString();
		auto const ui_prefix = std::string("Colour/Schemes/") + scheme_name
		+ (HasFocus() ? "/UI Focused/" : "/UI/");
		timeline->background_color = ToArgb(OPT_GET(ui_prefix + "Dark")->GetColor());
		timeline->foreground_color = ToArgb(OPT_GET(ui_prefix + "Light")->GetColor());
		frame.timeline = std::move(timeline);
		auto scrollbar_frame = std::make_shared<ScrollbarFrame>();
		scrollbar_frame->y = impl->viewport.scrollbar.y;
		scrollbar_frame->height = impl->viewport.scrollbar.height;
		scrollbar_frame->total = static_cast<int>(std::lround(
		impl->viewport.logical_audio_width * std::max(1.0, static_cast<double>(GetContentScaleFactor()))));
		scrollbar_frame->page = impl->viewport.target_width;
		scrollbar_frame->position = static_cast<int>(std::lround(
			impl->viewport.scroll_left * std::max(1.0, static_cast<double>(GetContentScaleFactor()))));
		if (impl->provider && impl->provider->GetNumSamples() > 0) {
			auto const decoded = impl->provider->GetDecodedSamples();
			if (decoded >= impl->provider->GetNumSamples())
				scrollbar_frame->load_position = -1;
			else
				scrollbar_frame->load_position = static_cast<int>(std::clamp<std::int64_t>(
					decoded * scrollbar_frame->total / impl->provider->GetNumSamples(), 0, scrollbar_frame->total));
		}
		scrollbar_frame->background_color = ToArgb(OPT_GET(ui_prefix + "Dark")->GetColor());
		scrollbar_frame->thumb_color = ToArgb(OPT_GET(ui_prefix + "Light")->GetColor());
		scrollbar_frame->selection_color = ToArgb(OPT_GET(ui_prefix + "Selection")->GetColor());
		frame.scrollbar = scrollbar_frame;

		if (!impl->presentation_colors_ready) {
			for (int style = AudioStyle_Normal; style < AudioStyle_MAX; ++style) {
				AudioColorScheme waveform_scheme(
					8,
					OPT_GET("Colour/Audio Display/Waveform")->GetString(),
					static_cast<AudioRenderingStyle>(style));
				impl->waveform_style_colors[style] = {
					ToArgb(waveform_scheme.get(0.f)),
					ToArgb(waveform_scheme.get(0.4f)),
					ToArgb(waveform_scheme.get(0.7f)),
					ToArgb(waveform_scheme.get(1.f)),
				};
			}
			impl->presentation_colors_ready = true;
		}
		frame.background_color = impl->waveform_style_colors[AudioStyle_Normal][0];
		frame.waveform_peak_color = impl->waveform_style_colors[AudioStyle_Normal][1];
		frame.waveform_average_color = impl->waveform_style_colors[AudioStyle_Normal][2];
		frame.waveform_zero_color = impl->waveform_style_colors[AudioStyle_Normal][3];
		frame.draw_waveform_average = OPT_GET("Audio/Display/Waveform Style")->GetInt() != 0;

		if (frame.kind == ContentKind::Spectrum) {
			for (int style = AudioStyle_Normal; style < AudioStyle_MAX; ++style) {
				if (impl->spectrum_style_palettes[style])
					continue;
				auto palette = std::make_shared<SpectrumPalette>();
				palette->revision = (impl->presentation_revision << 3) | static_cast<std::uint64_t>(style + 1);
				AudioColorScheme spectrum_scheme(
					8,
					OPT_GET("Colour/Audio Display/Spectrum")->GetString(),
					static_cast<AudioRenderingStyle>(style));
				for (std::size_t i = 0; i < palette->colors.size(); ++i)
					palette->colors[i] = ToArgb(spectrum_scheme.get(static_cast<float>(i) / 255.f));
				impl->spectrum_style_palettes[style] = std::move(palette);
			}
			frame.spectrum_palette = impl->spectrum_style_palettes[AudioStyle_Normal];
			frame.background_color = frame.spectrum_palette->colors.front();
			auto const bin_count = static_cast<std::uint32_t>(
				std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
			if (!impl->spectrum_band_plan
				|| impl->spectrum_band_plan->bin_count != bin_count
				|| impl->spectrum_band_plan->output_height != impl->viewport.content.height) {
				SpectrumBandPlanRequest plan_request;
				plan_request.bin_count = bin_count;
				plan_request.output_height = impl->viewport.content.height;
				plan_request.sample_rate = impl->provider->GetSampleRate();
				plan_request.mode = OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt() == 0
					? SpectrumScaleMode::LegacyLinear
					: SpectrumScaleMode::FrequencyCurve;
				plan_request.frequency_reference_position = SpectrumFrequencyReferenceForPreset(
					OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt());
				auto band_plan = std::make_shared<SpectrumBandPlan>(BuildSpectrumBandPlan(plan_request));
				if (!band_plan->IsValid()) {
					RequestFallback("Skia Audio Display could not build a valid spectrum band plan");
					return;
				}
				impl->spectrum_band_plan = std::move(band_plan);
			}
			frame.spectrum_band_plan = impl->spectrum_band_plan;
		}

		StyleRangeCollector style_collector;
		auto *timing = impl->audio_controller ? impl->audio_controller->GetTimingController() : nullptr;
		if (timing)
			timing->GetRenderingStyles(style_collector);
		for (auto const& span : BuildDeviceStyleSpans(style_collector.ranges, impl->viewport)) {
			auto const style_index = std::clamp(
				static_cast<int>(span.style),
				static_cast<int>(AudioStyle_Normal),
				static_cast<int>(AudioStyle_MAX - 1));
			StyleFrame style;
			style.x = span.x;
			style.width = span.width;
			style.background_color = impl->waveform_style_colors[style_index][0];
			style.waveform_peak_color = impl->waveform_style_colors[style_index][1];
			style.waveform_average_color = impl->waveform_style_colors[style_index][2];
			style.waveform_zero_color = impl->waveform_style_colors[style_index][3];
			if (frame.kind == ContentKind::Spectrum) {
				style.spectrum_palette = impl->spectrum_style_palettes[style_index];
				style.background_color = style.spectrum_palette->colors.front();
			}
			frame.styles.push_back(std::move(style));
		}

		if (timing) {
			auto const first_visible_ms = std::max(0, static_cast<int>(std::floor(
				impl->viewport.first_column_exact * impl->viewport.milliseconds_per_column)));
			auto const last_visible_ms = std::max(first_visible_ms, static_cast<int>(std::ceil(
				(impl->viewport.first_column_exact + impl->viewport.content.width)
					* impl->viewport.milliseconds_per_column)));
			TimeRange const visible_range(first_visible_ms, last_visible_ms);
			auto const x_from_ms = [this](int time_ms) {
				return static_cast<float>(impl->viewport.content.x
					+ time_ms / impl->viewport.milliseconds_per_column
					- impl->viewport.first_column_exact);
			};

			AudioMarkerVector markers;
			timing->GetMarkers(visible_range, markers);
			frame.markers.reserve(markers.size());
			for (auto const *marker : markers) {
				auto const pen = marker->GetStyle();
				frame.markers.push_back({
					x_from_ms(marker->GetPosition()),
					ToArgb(pen.GetColour()),
					std::max(1, static_cast<int>(std::lround(pen.GetWidth() * GetContentScaleFactor()))),
					static_cast<std::uint8_t>(marker->GetFeet()),
				});
			}

			std::vector<AudioLabelProvider::AudioLabel> labels;
			timing->GetLabels(visible_range, labels);
			frame.labels.reserve(labels.size());
			for (auto const& label : labels) {
				frame.labels.push_back({
					x_from_ms(label.range.begin()),
					static_cast<float>(label.range.length() / impl->viewport.milliseconds_per_column),
					label.text.utf8_string(),
				});
			}

			auto const selection = timing->GetPrimaryPlaybackRange();
			 scrollbar_frame->selection_start = std::max(0, static_cast<int>(std::floor(
				selection.begin() / impl->viewport.milliseconds_per_column)));
			 scrollbar_frame->selection_length = std::max(0, static_cast<int>(std::ceil(
				selection.length() / impl->viewport.milliseconds_per_column)));
		}
		auto const cursor_position_ms = impl->playback_position_ms >= 0
			? impl->playback_position_ms : impl->mouse_position_ms;
		if (cursor_position_ms >= 0) {
			auto cursor = std::make_shared<CursorFrame>();
			cursor->x = static_cast<float>(impl->viewport.content.x
				+ cursor_position_ms / impl->viewport.milliseconds_per_column
				- impl->viewport.first_column_exact);
			cursor->color = impl->playback_position_ms >= 0
				? ToArgb(OPT_GET("Colour/Audio Display/Play Cursor")->GetColor())
				: 0xFFFFFFFFu;
			if (impl->playback_position_ms < 0
				&& OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool())
				cursor->label = agi::Time(cursor_position_ms).GetAssFormatted();
			frame.cursor = std::move(cursor);
		}

		ContentViewportRequest request;
		request.generation = impl->content_generation;
		request.kind = impl->content_analysis.kind;
		request.first_column = impl->viewport.first_column;
		request.column_count = impl->viewport.visible_column_count;
		request.tile_column_count = 256;
		if (request.kind == ContentKind::Spectrum)
			request.spectrum_bin_count = static_cast<std::uint32_t>(
				std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
		for (auto const& key : PlanVisibleContentTiles(request))
			if (auto tile = impl->content_worker.Find(key))
				frame.tiles.push_back(std::move(tile));

		if (!impl->presenter->RenderContentFrame(context, target, frame)) {
			RequestFallback(impl->presenter->TakeFailureLogMessage());
			return;
		}
	}
	if (!SwapBuffers()) {
		impl->presenter->Fail(context, SkiaGlDeviceFailure::SwapBuffersFailed, "wxGLCanvas::SwapBuffers failed");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
}
catch (std::exception const& err) {
	if (impl->presenter && impl->context) {
		impl->presenter->Fail(
			impl->ContextToken(),
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			err.what());
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
	else {
		RequestFallback(std::string("Skia Audio Display initialization failed: ") + err.what());
	}
}
catch (...) {
	RequestFallback("an unknown exception escaped Skia Audio Display paint");
}

void SkiaAudioDisplay::OnEraseBackground(wxEraseEvent&) {
}

void SkiaAudioDisplay::OnSize(wxSizeEvent& event) {
	ReconfigureAnalysis();
	Refresh(false);
	event.Skip();
}

void SkiaAudioDisplay::OnContentReady(wxThreadEvent&) {
	Refresh(false);
}

void SkiaAudioDisplay::OnContentFailure(wxThreadEvent& event) {
	RequestFallback(event.GetString().utf8_string());
}

void SkiaAudioDisplay::OnLoadTimer(wxTimerEvent&) {
	if (!impl || !impl->provider)
		return;
	auto const decoded = impl->provider->GetDecodedSamples();
	if (decoded != impl->last_decoded_samples) {
		impl->last_decoded_samples = decoded;
		Refresh(false);
	}
	if (decoded >= impl->provider->GetNumSamples())
		impl->load_timer.Stop();
}

void SkiaAudioDisplay::EmitMiddleSeekOutput(int time_ms, bool commit) {
	if (!impl || !impl->project_context)
		return;
	auto core = impl->project_context->GetCore();
	if (!core.videoController || !core.project->VideoProvider())
		return;
	if (commit)
		core.videoController->CommitInteractiveSeekPreviewToTime(time_ms, agi::vfr::EXACT);
	else
		core.videoController->PreviewToFrameLatest(core.videoController->FrameAtTime(time_ms, agi::vfr::EXACT));
}

void SkiaAudioDisplay::ScheduleMiddleSeekTimer() {
	if (!impl || !impl->middle_seek_active) {
		if (impl)
			impl->middle_seek_timer.Stop();
		return;
	}
	if (!wxGetMouseState().MiddleIsDown()) {
		FinishMiddleSeek(impl->mouse_position_ms >= 0 ? impl->mouse_position_ms : 0);
		return;
	}
	if (impl->viewport.IsValid()) {
		auto const point = ScreenToClient(wxGetMousePosition());
		impl->mouse_position_ms = std::max(0, static_cast<int>(
			(impl->scroll_left + point.x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level)));
	}
	auto next = impl->middle_seek_policy.NextPreviewTime();
	if (!next) {
		impl->middle_seek_timer.Start(33, true);
		return;
	}
	auto const now = NavigationPreviewPolicy::Clock::now();
	auto const delay = *next > now
		? std::chrono::duration_cast<std::chrono::milliseconds>(*next - now).count() : 1;
	impl->middle_seek_timer.Start(std::max(1, static_cast<int>(delay)), true);
}

void SkiaAudioDisplay::FinishMiddleSeek(int time_ms) {
	if (!impl || !impl->middle_seek_active)
		return;
	impl->middle_seek_timer.Stop();
	impl->middle_seek_active = false;
	impl->middle_seek_policy.OnRelease(time_ms, NavigationPreviewPolicy::Clock::now());
	EmitMiddleSeekOutput(time_ms, true);
	impl->mouse_position_ms = -1;
	Refresh(false);
}

void SkiaAudioDisplay::OnMiddleSeekTimer(wxTimerEvent&) {
	if (!impl || !impl->middle_seek_active)
		return;
	if (!wxGetMouseState().MiddleIsDown()) {
		FinishMiddleSeek(impl->mouse_position_ms >= 0 ? impl->mouse_position_ms : 0);
		return;
	}
	if (impl->viewport.IsValid()) {
		auto const point = ScreenToClient(wxGetMousePosition());
		impl->mouse_position_ms = std::max(0, static_cast<int>(
			(impl->scroll_left + point.x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level)));
	}
	if (auto output = impl->middle_seek_policy.OnTimer(NavigationPreviewPolicy::Clock::now()))
		EmitMiddleSeekOutput(output->target, false);
	ScheduleMiddleSeekTimer();
}

void SkiaAudioDisplay::OnPlaybackPosition(int position_ms) {
	if (!impl)
		return;
	impl->playback_position_ms = std::max(0, position_ms);
	if (OPT_GET("Audio/Lock Scroll on Cursor")->GetBool() && impl->viewport.IsValid()) {
		auto const logical_ms_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level);
		auto const pixel_position = static_cast<int>(std::floor(position_ms / logical_ms_per_pixel));
		auto const client_width = std::max(1, GetClientSize().GetWidth());
		auto const edge = std::max(1, client_width / 20);
		if (impl->scroll_left > 0 && pixel_position < impl->scroll_left + edge)
			impl->scroll_left = std::max(0, pixel_position - edge);
		else if (pixel_position >= impl->scroll_left + client_width - edge)
			impl->scroll_left = pixel_position - client_width + edge;
		RebuildViewport();
		RequestVisibleContent();
	}
	Refresh(false);
}

void SkiaAudioDisplay::OnPlaybackStop() {
	if (!impl)
		return;
	impl->playback_position_ms = -1;
	Refresh(false);
}

void SkiaAudioDisplay::OnTimingControllerChanged() {
	if (!impl || !impl->audio_controller)
		return;
	impl->timing_connections.clear();
	if (auto *timing = impl->audio_controller->GetTimingController()) {
		impl->timing_connections = agi::signal::make_vector({
			timing->AddMarkerMovedListener(&SkiaAudioDisplay::OnTimingDataChanged, this),
			timing->AddLabelChangedListener(&SkiaAudioDisplay::OnTimingDataChanged, this),
			timing->AddUpdatedPrimaryRangeListener(&SkiaAudioDisplay::OnTimingDataChanged, this),
			timing->AddUpdatedStyleRangesListener(&SkiaAudioDisplay::OnTimingDataChanged, this),
		});
	}
	OnTimingDataChanged();
}

void SkiaAudioDisplay::OnTimingDataChanged() {
	if (impl)
		Refresh(false);
}

void SkiaAudioDisplay::OnMouseEvent(wxMouseEvent& event) {
	if (!impl || impl->fallback_requested)
		return;
	if (hotkey::check("Audio", impl->project_context, event))
		return;

	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;
	auto const mouse = event.GetPosition();
	auto const client_width = std::max(1, GetClientSize().GetWidth());
	auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	auto const timeline_bottom = static_cast<int>(std::ceil(
		(impl->viewport.timeline.y + impl->viewport.timeline.height) / scale));
	auto const scrollbar_top = static_cast<int>(std::floor(impl->viewport.scrollbar.y / scale));
	auto const time_from_x = [this](int x) {
		auto const value = (impl->scroll_left + x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level);
		return static_cast<int>(std::clamp<double>(value, 0.0, std::numeric_limits<int>::max()));
	};
	auto *timing = impl->audio_controller ? impl->audio_controller->GetTimingController() : nullptr;

	if (event.MiddleDown() || (impl->middle_seek_active && event.MiddleIsDown())) {
		auto core = impl->project_context->GetCore();
		if (core.videoController && core.project->VideoProvider()) {
			auto const time_ms = time_from_x(mouse.x);
			impl->mouse_position_ms = time_ms;
			if (!impl->middle_seek_active)
				core.videoController->BeginInteractiveSeekPreview();
			impl->middle_seek_active = true;
			if (auto output = impl->middle_seek_policy.OnMotion(
				time_ms, NavigationPreviewPolicy::Clock::now(), event.MiddleDown()))
				EmitMiddleSeekOutput(output->target, false);
			ScheduleMiddleSeekTimer();
			Refresh(false);
		}
		return;
	}
	if (event.MiddleUp() && impl->middle_seek_active) {
		FinishMiddleSeek(time_from_x(mouse.x));
		return;
	}

	if (impl->timeline_dragging) {
		if (event.LeftIsDown()) {
			ScrollBy(impl->drag_last_x - mouse.x);
			impl->drag_last_x = mouse.x;
		}
		else {
			impl->timeline_dragging = false;
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}
	if (impl->scrollbar_dragging) {
		if (event.LeftIsDown()) {
			auto const total = std::max(1, impl->viewport.logical_audio_width);
			auto const page = std::clamp(GetClientSize().GetWidth(), 1, total);
			auto const thumb_width = std::max(10.0, client_width * static_cast<double>(page) / total);
			auto const shaft = std::max(1.0, client_width - thumb_width);
			auto const maximum = std::max(0, total - page);
			impl->scroll_left = static_cast<int>(std::lround(maximum
				* std::clamp(mouse.x - thumb_width * 0.5, 0.0, shaft) / shaft));
			RebuildViewport();
			RequestVisibleContent();
			Refresh(false);
		}
		else {
			impl->scrollbar_dragging = false;
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}
	if (!impl->dragged_markers.empty()) {
		bool const button_down = impl->dragged_button == wxMOUSE_BTN_LEFT
			? event.LeftIsDown() : event.RightIsDown();
		if (button_down && timing) {
			if (mouse.x < 0)
				ScrollBy(mouse.x - client_width / 20);
			else if (mouse.x >= client_width)
				ScrollBy(mouse.x - client_width + client_width / 20);
			auto const snap = OPT_GET("Audio/Snap/Enable")->GetBool() != event.ShiftDown()
				? static_cast<int>(OPT_GET("Audio/Snap/Distance")->GetInt()
					* AudioMillisecondsPerLogicalPixel(impl->zoom_level))
				: 0;
			timing->OnMarkerDrag(impl->dragged_markers, time_from_x(mouse.x), snap);
			Refresh(false);
		}
		else {
			impl->dragged_markers.clear();
			impl->dragged_button = wxMOUSE_BTN_NONE;
			SetCursor(wxNullCursor);
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}

	if (event.IsButton())
		SetFocus();
	if (event.LeftDown() && mouse.y < timeline_bottom) {
		impl->timeline_dragging = true;
		impl->drag_last_x = mouse.x;
		if (!HasCapture()) CaptureMouse();
		return;
	}
	if (event.LeftDown() && mouse.y >= scrollbar_top) {
		impl->scrollbar_dragging = true;
		if (!HasCapture()) CaptureMouse();
		wxMouseEvent motion(event);
		motion.SetEventType(wxEVT_MOTION);
		OnMouseEvent(motion);
		return;
	}

	if (event.Moving()) {
		if (!impl->audio_controller->IsPlaying()) {
			impl->mouse_position_ms = mouse.y >= timeline_bottom && mouse.y < scrollbar_top
				? time_from_x(mouse.x) : -1;
			Refresh(false);
		}
		if (timing && mouse.y >= timeline_bottom && mouse.y < scrollbar_top) {
			auto const sensitivity = static_cast<int>(
				OPT_GET("Audio/Start Drag Sensitivity")->GetInt()
				* AudioMillisecondsPerLogicalPixel(impl->zoom_level));
			SetCursor(timing->IsNearbyMarker(impl->mouse_position_ms, sensitivity, event.AltDown())
				? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
		}
		return;
	}

	if (timing && (event.LeftDown() || event.RightDown())
		&& mouse.y >= timeline_bottom && mouse.y < scrollbar_top) {
		auto const sensitivity = static_cast<int>(
			OPT_GET("Audio/Start Drag Sensitivity")->GetInt()
			* AudioMillisecondsPerLogicalPixel(impl->zoom_level));
		auto const snap = OPT_GET("Audio/Snap/Enable")->GetBool() != event.ShiftDown()
			? static_cast<int>(OPT_GET("Audio/Snap/Distance")->GetInt()
				* AudioMillisecondsPerLogicalPixel(impl->zoom_level))
			: 0;
		impl->dragged_markers = event.LeftDown()
			? timing->OnLeftClick(time_from_x(mouse.x), event.CmdDown(), event.AltDown(), sensitivity, snap)
			: timing->OnRightClick(time_from_x(mouse.x), event.CmdDown(), sensitivity, snap);
		if (!impl->dragged_markers.empty()) {
			impl->dragged_button = event.LeftDown() ? wxMOUSE_BTN_LEFT : wxMOUSE_BTN_RIGHT;
			impl->mouse_position_ms = -1;
			if (!HasCapture()) CaptureMouse();
		}
		Refresh(false);
	}
}

void SkiaAudioDisplay::OnMouseEnter(wxMouseEvent& event) {
	if (OPT_GET("Audio/Auto/Focus")->GetBool())
		SetFocus();
	event.Skip();
}

void SkiaAudioDisplay::OnMouseLeave(wxMouseEvent& event) {
	if (impl && !impl->middle_seek_active && impl->audio_controller && !impl->audio_controller->IsPlaying()) {
		impl->mouse_position_ms = -1;
		Refresh(false);
	}
	event.Skip();
}

void SkiaAudioDisplay::OnMouseCaptureLost(wxMouseCaptureLostEvent&) {
	if (!impl)
		return;
	impl->dragged_markers.clear();
	impl->dragged_button = wxMOUSE_BTN_NONE;
	impl->timeline_dragging = false;
	impl->scrollbar_dragging = false;
	if (impl->middle_seek_active) {
		if (auto core = impl->project_context->GetCore(); core.videoController)
			core.videoController->CancelInteractiveSeekPreview();
		impl->middle_seek_active = false;
		impl->middle_seek_policy.Cancel();
		impl->middle_seek_timer.Stop();
	}
	SetCursor(wxNullCursor);
}

void SkiaAudioDisplay::OnFocus(wxFocusEvent& event) {
	Refresh(false);
	event.Skip();
}

void SkiaAudioDisplay::OnKeyDown(wxKeyEvent& event) {
	if (!hotkey::check("Audio", impl->project_context, event))
		event.Skip();
}

void SkiaAudioDisplay::OnAudioOpen(agi::AudioProvider *provider) {
	try {
		impl->provider = provider;
		impl->last_decoded_samples = provider ? provider->GetDecodedSamples() : 0;
		if (provider && provider->GetDecodedSamples() < provider->GetNumSamples())
			impl->load_timer.Start(100);
		else
			impl->load_timer.Stop();
		auto const generation = impl->content_worker.SetProvider(provider);
		if (generation != impl->content_generation)
			impl->has_last_content_request = false;
		impl->content_generation = generation;
		ReconfigureAnalysis();
		Refresh(false);
	}
	catch (std::exception const& err) {
		RequestFallback(std::string("Skia Audio content worker initialization failed: ") + err.what());
	}
	catch (...) {
		RequestFallback("an unknown exception escaped Skia Audio content worker initialization");
	}
}

void SkiaAudioDisplay::RequestFallback(std::string message) {
	if (impl->fallback_requested)
		return;
	impl->fallback_requested = true;
	if (message.empty())
		message = "Skia Audio Display failed without a device diagnostic";
	if (impl->failure_callback)
		impl->failure_callback(std::move(message));
}

void SkiaAudioDisplay::ClearFailureCallback() {
	impl->failure_callback = {};
}

void SkiaAudioDisplay::SyncToCurrentAudioProvider() {
	OnAudioOpen(impl->project_context->GetCore().project->AudioProvider());
}

void SkiaAudioDisplay::ScrollBy(int pixel_amount) {
	impl->scroll_left += pixel_amount;
	RebuildViewport();
	Refresh(false);
}

void SkiaAudioDisplay::ScrollBy(int pixel_amount, int) {
	ScrollBy(pixel_amount);
}

void SkiaAudioDisplay::ScrollTimeRangeInView(TimeRange const& range) {
	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;
	auto const ms_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level);
	auto const begin = static_cast<int>(range.begin() / ms_per_pixel);
	auto const end = static_cast<int>(range.end() / ms_per_pixel);
	auto const margin = GetClientSize().GetWidth() / 20;
	auto const page = GetClientSize().GetWidth() - 2 * margin;
	if (begin >= impl->scroll_left + margin && end <= impl->scroll_left + margin + page)
		return;
	if (end - begin < page)
		impl->scroll_left = begin - (page - (end - begin)) / 2 - margin;
	else if (end >= impl->scroll_left + margin && end < impl->scroll_left + margin + page)
		impl->scroll_left = end - page - margin;
	else
		impl->scroll_left = begin - margin;
	RebuildViewport();
	Refresh(false);
}

void SkiaAudioDisplay::SetZoomLevel(int zoom_level) {
	impl->zoom_level = zoom_level;
	ReconfigureAnalysis();
	Refresh(false);
}

int SkiaAudioDisplay::GetZoomLevel() const {
	return impl->zoom_level;
}

void SkiaAudioDisplay::SetAmplitudeScale(float scale) {
	impl->amplitude_scale = std::max(0.f, scale);
	Refresh(false);
}

}
