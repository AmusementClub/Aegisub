#include "skia_audio_display.h"

#include "skia_audio_content_worker.h"
#include "skia_audio_presenter.h"

#include "../../include/aegisub/context.h"
#include "../../project.h"
#include "../../audio_colorscheme.h"
#include "../../audio_renderer_spectrum.h"
#include "../../options.h"
#include "../../time_range.h"

#include <libaegisub/signal.h>
#include <libaegisub/audio/provider.h>

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
#include <wx/thread.h>

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
		this->project_context = project_context;
	}

	std::unique_ptr<wxGLContext> context;
	std::unique_ptr<Presenter> presenter;
	ContentWorker content_worker;
	agi::signal::Connection audio_open_connection;
	std::vector<agi::signal::Connection> option_connections;
	agi::Context *project_context = nullptr;
	agi::AudioProvider *provider = nullptr;
	ContentAnalysisConfig content_analysis;
	ContentGeneration content_generation;
	ContentViewportRequest last_content_request;
	bool has_last_content_request = false;
	FrameViewport viewport;
	int zoom_level = 0;
	int scroll_left = 0;
	float amplitude_scale = 1.f;
	std::uint64_t presentation_revision = 1;
	std::array<std::uint32_t, 4> waveform_colors {};
	bool presentation_colors_ready = false;
	std::shared_ptr<SpectrumPalette const> spectrum_palette;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	FailureCallback failure_callback;
	std::uint64_t context_generation = 0;
	bool fallback_requested = false;

	SkiaGlContextToken ContextToken() const noexcept {
		return { context.get(), context_generation };
	}

	void InvalidatePresentation() {
		++presentation_revision;
		if (!presentation_revision)
			++presentation_revision;
		spectrum_palette.reset();
		spectrum_band_plan.reset();
		presentation_colors_ready = false;
	}
};

SkiaAudioDisplay::SkiaAudioDisplay(
	wxWindow *parent,
	agi::Context *context,
	FailureInjection failure_injection,
	FailureCallback failure_callback)
: wxGLCanvas(parent, wxID_ANY, gl_attributes, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
, impl(std::make_unique<Impl>(this, context, failure_injection, std::move(failure_callback)))
{
	impl->project_context = context;
	impl->audio_open_connection = context->GetCore().project->AddAudioProviderListener(
		&SkiaAudioDisplay::OnAudioOpen,
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
	impl->spectrum_palette.reset();
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

		if (!impl->presentation_colors_ready) {
			AudioColorScheme waveform_scheme(
				8,
				OPT_GET("Colour/Audio Display/Waveform")->GetString(),
				AudioStyle_Normal);
			impl->waveform_colors = {
				ToArgb(waveform_scheme.get(0.f)),
				ToArgb(waveform_scheme.get(0.4f)),
				ToArgb(waveform_scheme.get(0.7f)),
				ToArgb(waveform_scheme.get(1.f)),
			};
			impl->presentation_colors_ready = true;
		}
		frame.background_color = impl->waveform_colors[0];
		frame.waveform_peak_color = impl->waveform_colors[1];
		frame.waveform_average_color = impl->waveform_colors[2];
		frame.waveform_zero_color = impl->waveform_colors[3];
		frame.draw_waveform_average = OPT_GET("Audio/Display/Waveform Style")->GetInt() != 0;

		if (frame.kind == ContentKind::Spectrum) {
			if (!impl->spectrum_palette) {
				auto palette = std::make_shared<SpectrumPalette>();
				palette->revision = impl->presentation_revision;
				AudioColorScheme spectrum_scheme(
					8,
					OPT_GET("Colour/Audio Display/Spectrum")->GetString(),
					AudioStyle_Normal);
				for (std::size_t i = 0; i < palette->colors.size(); ++i)
					palette->colors[i] = ToArgb(spectrum_scheme.get(static_cast<float>(i) / 255.f));
				impl->spectrum_palette = std::move(palette);
			}
			frame.spectrum_palette = impl->spectrum_palette;
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

void SkiaAudioDisplay::OnAudioOpen(agi::AudioProvider *provider) {
	try {
		impl->provider = provider;
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
