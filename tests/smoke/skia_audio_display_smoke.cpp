#include "../../src/skia/audio/skia_audio_presenter.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class HiddenGlWindow final {
	static char const *ClassName() noexcept { return "AegisubSkiaAudioDisplaySmoke"; }

	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

public:
	HiddenGlWindow(int width, int height)
	: width(width)
	, height(height) {
		WNDCLASSA cls = {};
		cls.style = CS_OWNDC;
		cls.lpfnWndProc = DefWindowProcA;
		cls.hInstance = GetModuleHandleA(nullptr);
		cls.lpszClassName = ClassName();
		if (!RegisterClassA(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			throw std::runtime_error("RegisterClassA failed");

		window = CreateWindowExA(
			0,
			ClassName(),
			"Aegisub Skia Audio Display smoke",
			WS_POPUP,
			0,
			0,
			width,
			height,
			nullptr,
			nullptr,
			GetModuleHandleA(nullptr),
			nullptr);
		if (!window)
			throw std::runtime_error("CreateWindowExA failed");

		dc = GetDC(window);
		if (!dc)
			throw std::runtime_error("GetDC failed");

		PIXELFORMATDESCRIPTOR descriptor = {};
		descriptor.nSize = sizeof(descriptor);
		descriptor.nVersion = 1;
		descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		descriptor.iPixelType = PFD_TYPE_RGBA;
		descriptor.cColorBits = 32;
		descriptor.cAlphaBits = 8;
		descriptor.cStencilBits = 8;
		descriptor.iLayerType = PFD_MAIN_PLANE;
		auto const format = ChoosePixelFormat(dc, &descriptor);
		if (!format || !SetPixelFormat(dc, format, &descriptor))
			throw std::runtime_error("setting the WGL pixel format failed");

		context = wglCreateContext(dc);
		if (!context || !wglMakeCurrent(dc, context))
			throw std::runtime_error("creating the WGL context failed");
	}

	~HiddenGlWindow() {
		wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc && window)
			ReleaseDC(window, dc);
		if (window)
			DestroyWindow(window);
	}

	void const *ContextIdentity() const noexcept { return context; }
	int Width() const noexcept { return width; }
	int Height() const noexcept { return height; }
};

bool Near(unsigned char actual, unsigned char expected) {
	return actual >= static_cast<unsigned char>(std::max(0, expected - 4))
		&& actual <= static_cast<unsigned char>(std::min(255, expected + 4));
}

bool ContainsColor(
	std::vector<unsigned char> const& pixels,
	unsigned char red,
	unsigned char green,
	unsigned char blue) {
	for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
		if (Near(pixels[i], red)
			&& Near(pixels[i + 1], green)
			&& Near(pixels[i + 2], blue)
			&& pixels[i + 3] >= 250) {
			return true;
		}
	}
	return false;
}

std::vector<unsigned char> ReadBack(aegisub::skia::audio::FrameTarget const& target) {
	std::vector<unsigned char> pixels(static_cast<std::size_t>(target.width) * target.height * 4);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	if (glGetError() != GL_NO_ERROR)
		throw std::runtime_error("glReadPixels failed");
	return pixels;
}

std::shared_ptr<aegisub::skia::audio::ContentTile const> MakeWaveformTile(
	aegisub::skia::audio::ContentGeneration generation,
	std::uint64_t tile_index,
	std::uint32_t columns) {
	using namespace aegisub::skia::audio;
	auto tile = std::make_shared<ContentTile>();
	tile->key = { generation, ContentKind::Waveform, tile_index, columns, 0 };
	tile->waveform.resize(columns);
	for (std::uint32_t x = 0; x < columns; ++x) {
		auto const phase = static_cast<float>(x % 16) / 15.f;
		auto const peak = 0.25f + phase * 0.65f;
		tile->waveform[x] = { -peak, peak, -peak * 0.38f, peak * 0.38f };
	}
	return tile;
}

std::shared_ptr<aegisub::skia::audio::ContentTile const> MakeSpectrumTile(
	aegisub::skia::audio::ContentGeneration generation,
	std::uint64_t tile_index,
	std::uint32_t columns,
	std::uint32_t bins) {
	using namespace aegisub::skia::audio;
	auto tile = std::make_shared<ContentTile>();
	tile->key = { generation, ContentKind::Spectrum, tile_index, columns, bins };
	tile->spectrum_power.resize(static_cast<std::size_t>(columns) * bins);
	for (std::uint32_t x = 0; x < columns; ++x) {
		auto const power = x < columns / 2 ? 0.f : 1.f;
		for (std::uint32_t bin = 0; bin < bins; ++bin)
			tile->spectrum_power[static_cast<std::size_t>(x) * bins + bin] = power;
	}
	return tile;
}

std::shared_ptr<aegisub::skia::audio::SpectrumPalette const> MakePalette(
	std::uint64_t revision,
	bool green) {
	using namespace aegisub::skia::audio;
	auto palette = std::make_shared<SpectrumPalette>();
	palette->revision = revision;
	for (std::size_t i = 0; i < palette->colors.size(); ++i) {
		auto const value = static_cast<std::uint32_t>(i);
		palette->colors[i] = green
			? 0xFF000000u | (value << 8)
			: 0xFF000000u | (value << 16) | (255u - value);
	}
	return palette;
}

std::shared_ptr<aegisub::skia::audio::SpectrumBandPlan const> MakeBandPlan(
	std::uint32_t bins,
	int height,
	aegisub::skia::audio::SpectrumScaleMode mode) {
	using namespace aegisub::skia::audio;
	SpectrumBandPlanRequest request;
	request.bin_count = bins;
	request.output_height = height;
	request.sample_rate = 48000;
	request.mode = mode;
	request.frequency_reference_position = SpectrumFrequencyReferenceForPreset(2);
	auto plan = std::make_shared<SpectrumBandPlan>(BuildSpectrumBandPlan(request));
	return plan->IsValid() ? plan : nullptr;
}

bool ValidateWaveformContent(
	aegisub::skia::audio::FrameTarget const& target,
	SkiaGlContextToken context,
	aegisub::skia::audio::PresenterMetrics& final_metrics) {
	using namespace aegisub::skia::audio;
	ContentGeneration const generation { 3, 5 };
	auto first = MakeWaveformTile(generation, 0, 64);
	auto second = MakeWaveformTile(generation, 1, 64);
	Presenter presenter(FailureInjection::None);
	ContentFrame frame;
	frame.generation = generation;
	frame.kind = ContentKind::Waveform;
	frame.width = target.width;
	frame.height = target.height;
	frame.tiles = { first };
	frame.waveform_zero_color = 0xFFE76F51;

	if (!presenter.RenderContentFrame(context, target, frame)) {
		std::cerr << presenter.TakeFailureLogMessage() << '\n';
		return false;
	}
	auto const pixels = ReadBack(target);
	if (!ContainsColor(pixels, 24, 34, 48)
		|| !ContainsColor(pixels, 42, 157, 143)
		|| !ContainsColor(pixels, 233, 196, 106)
		|| !ContainsColor(pixels, 231, 111, 81)) {
		std::cerr << "waveform retained masks did not contain all expected colors\n";
		return false;
	}

	frame.amplitude = 0.5f;
	if (!presenter.RenderContentFrame(context, target, frame))
		return false;

	frame.tiles.clear();
	if (!presenter.RenderContentFrame(context, target, frame)) {
		std::cerr << "missing waveform tile blocked or failed presentation\n";
		return false;
	}

	frame.tiles = { MakeWaveformTile({ 99, 1 }, 0, 64) };
	if (!presenter.RenderContentFrame(context, target, frame)) {
		std::cerr << "stale waveform tile failed presentation instead of being skipped\n";
		return false;
	}

	auto one_entry = presenter.Metrics().content_cache_bytes;
	if (!one_entry)
		return false;
	presenter.SetContentCacheBudget(one_entry);
	frame.amplitude = 1.f;
	frame.tiles = { second };
	if (!presenter.RenderContentFrame(context, target, frame))
		return false;

	final_metrics = presenter.Metrics();
	bool const passed = final_metrics.surface_acquisitions == 1
		&& final_metrics.submits == 5
		&& final_metrics.content_tiles_drawn == 3
		&& final_metrics.content_tiles_skipped == 1
		&& final_metrics.content_cache_hits == 1
		&& final_metrics.content_cache_misses == 2
		&& final_metrics.content_uploads == 2
		&& final_metrics.content_evictions == 1
		&& final_metrics.content_cache_entries == 1;
	if (!passed)
		std::cerr << "waveform retained cache metrics were unexpected\n";
	presenter.Release(context);
	return passed;
}

bool ValidateSpectrumContent(
	aegisub::skia::audio::FrameTarget const& target,
	SkiaGlContextToken context,
	aegisub::skia::audio::PresenterMetrics& final_metrics) {
	using namespace aegisub::skia::audio;
	ContentGeneration const generation { 7, 11 };
	auto tile = MakeSpectrumTile(generation, 0, 96, 32);
	Presenter presenter(FailureInjection::None);
	ContentFrame frame;
	frame.generation = generation;
	frame.kind = ContentKind::Spectrum;
	frame.width = target.width;
	frame.height = target.height;
	frame.background_color = 0xFF101010;
	frame.spectrum_palette = MakePalette(1, false);
	frame.spectrum_band_plan = MakeBandPlan(32, target.height, SpectrumScaleMode::LegacyLinear);
	frame.tiles = { tile };

	if (!presenter.RenderContentFrame(context, target, frame)) {
		std::cerr << presenter.TakeFailureLogMessage() << '\n';
		return false;
	}
	auto const pixels = ReadBack(target);
	if (!ContainsColor(pixels, 0, 0, 255) || !ContainsColor(pixels, 255, 0, 0)) {
		std::cerr << "spectrum power/palette shader did not produce expected endpoints\n";
		return false;
	}

	frame.amplitude = 0.5f;
	if (!presenter.RenderContentFrame(context, target, frame))
		return false;
	frame.spectrum_palette = MakePalette(2, true);
	if (!presenter.RenderContentFrame(context, target, frame))
		return false;
	if (!ContainsColor(ReadBack(target), 0, 127, 0)) {
		std::cerr << "spectrum palette/amplitude update did not apply without content upload\n";
		return false;
	}
	frame.spectrum_band_plan = MakeBandPlan(32, target.height, SpectrumScaleMode::FrequencyCurve);
	if (!presenter.RenderContentFrame(context, target, frame)) {
		std::cerr << "spectrum band-plan change failed to remap retained power\n";
		return false;
	}

	final_metrics = presenter.Metrics();
	bool const passed = final_metrics.surface_acquisitions == 1
		&& final_metrics.submits == 4
		&& final_metrics.content_tiles_drawn == 4
		&& final_metrics.content_cache_hits == 2
		&& final_metrics.content_cache_misses == 2
		&& final_metrics.content_uploads == 2
		&& final_metrics.palette_uploads == 2;
	if (!passed)
		std::cerr << "spectrum retained cache metrics were unexpected\n";
	presenter.Release(context);
	return passed;
}

bool ValidateInjectedFailure(
	aegisub::skia::audio::FrameTarget const& target,
	SkiaGlContextToken context,
	aegisub::skia::audio::FailureInjection injection,
	SkiaGlDeviceFailure expected) {
	aegisub::skia::audio::Presenter presenter(injection);
	if (presenter.RenderDiagnosticFrame(context, target)) {
		std::cerr << "failure injection unexpectedly rendered a frame\n";
		return false;
	}
	if (presenter.LastFailure() != expected) {
		std::cerr
			<< "failure injection reported " << ToString(presenter.LastFailure())
			<< ", expected " << ToString(expected) << '\n';
		return false;
	}
	return true;
}

}

int main() try {
	using namespace aegisub::skia::audio;

	HiddenGlWindow window(160, 96);
	SkiaGlContextToken const context { window.ContextIdentity(), 1 };
	GLint stencil_bits = 0;
	glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);

	FrameTarget target;
	target.context_generation = context.generation;
	target.width = window.Width();
	target.height = window.Height();
	target.stencil_bits = std::max(0, static_cast<int>(stencil_bits));
	target.framebuffer_id = 0;
	target.bottom_left_origin = true;

	Presenter presenter(FailureInjection::None);
	if (!presenter.RenderDiagnosticFrame(context, target)) {
		if (presenter.LastFailure() == SkiaGlDeviceFailure::GlVersionUnsupported
			|| presenter.LastFailure() == SkiaGlDeviceFailure::SoftwareRendererUnsupported) {
			std::cout << "Skia Audio Display correctly selected fallback: "
				<< presenter.TakeFailureLogMessage() << '\n';
			return 0;
		}
		throw std::runtime_error(presenter.TakeFailureLogMessage());
	}

	auto const pixels = ReadBack(target);
	if (!ContainsColor(pixels, 24, 34, 48)
		|| !ContainsColor(pixels, 42, 157, 143)
		|| !ContainsColor(pixels, 233, 196, 106)) {
		throw std::runtime_error("the diagnostic frame did not contain all expected colors");
	}

	if (!presenter.RenderDiagnosticFrame(context, target))
		throw std::runtime_error("the warm diagnostic frame failed");
	auto const metrics = presenter.Metrics();
	if (metrics.frame_attempts != 2 || metrics.surface_acquisitions != 1 || metrics.submits != 2)
		throw std::runtime_error("warm rendering recreated the surface or used an unexpected submit count");
	presenter.Release(context);

	PresenterMetrics waveform_metrics;
	if (!ValidateWaveformContent(target, context, waveform_metrics))
		throw std::runtime_error("waveform retained content smoke failed");
	PresenterMetrics spectrum_metrics;
	if (!ValidateSpectrumContent(target, context, spectrum_metrics))
		throw std::runtime_error("spectrum retained content smoke failed");

	bool passed = true;
	passed = ValidateInjectedFailure(
		target,
		context,
		FailureInjection::ContextInitialization,
		SkiaGlDeviceFailure::ContextInitializationInjected) && passed;
	passed = ValidateInjectedFailure(
		target,
		context,
		FailureInjection::FrameBegin,
		SkiaGlDeviceFailure::FrameBeginInjected) && passed;
	passed = ValidateInjectedFailure(
		target,
		context,
		FailureInjection::FlushSubmit,
		SkiaGlDeviceFailure::FlushInjected) && passed;
	passed = ValidateInjectedFailure(
		target,
		context,
		FailureInjection::Unsupported,
		SkiaGlDeviceFailure::UnsupportedFailureInjection) && passed;

	if (passed) {
		std::cout
			<< "Skia Audio Display WGL/Ganesh smoke passed: surfaces="
			<< metrics.surface_acquisitions
			<< ", submits=" << metrics.submits
			<< ", waveform-uploads=" << waveform_metrics.content_uploads
			<< ", waveform-evictions=" << waveform_metrics.content_evictions
			<< ", spectrum-uploads=" << spectrum_metrics.content_uploads
			<< ", palette-uploads=" << spectrum_metrics.palette_uploads << '\n';
	}
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "skia-audio-display-smoke failed: " << err.what() << '\n';
	return 2;
}

#else

#include <iostream>

int main() {
	std::cout << "skia-audio-display-smoke is currently only implemented on Windows builds.\n";
	return 0;
}

#endif
