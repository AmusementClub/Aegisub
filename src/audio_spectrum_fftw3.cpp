#include "audio_spectrum_fftw3.h"

#ifdef WITH_FFTW3

#include <libaegisub/native_library.h>

#include <cmath>
#include <utility>

namespace audio::spectrum {
namespace {
using Plan = void *;
using Complex = double[2];

struct Api {
	double *(*alloc_real)(size_t) = nullptr;
	Complex *(*alloc_complex)(size_t) = nullptr;
	Plan (*plan_dft_r2c_1d)(int, double *, Complex *, unsigned) = nullptr;
	void (*execute)(Plan) = nullptr;
	void (*destroy_plan)(Plan) = nullptr;
	void (*free)(void *) = nullptr;
};

constexpr unsigned estimate = 1U << 6;
Api api;

template <typename T>
void LoadSymbol(agi::native::Library& library, T& target, const char *name) {
	target = library.ResolveSymbol<T>(name);
}

void InitializeRuntime(agi::native::Library& library) {
	LoadSymbol(library, api.alloc_real, "fftw_alloc_real");
	LoadSymbol(library, api.alloc_complex, "fftw_alloc_complex");
	LoadSymbol(library, api.plan_dft_r2c_1d, "fftw_plan_dft_r2c_1d");
	LoadSymbol(library, api.execute, "fftw_execute");
	LoadSymbol(library, api.destroy_plan, "fftw_destroy_plan");
	LoadSymbol(library, api.free, "fftw_free");
}

agi::native::CachedLibrary& RuntimeLibrary() {
	static agi::native::CachedLibrary runtime_library(
		"fftw3",
		"FFTW3",
		"audio/spectrum/fftw3",
		InitializeRuntime,
		agi::native::CachedLibrary::DetailFunction(),
		agi::native::DefaultAppLocalLoadOptions(false));
	return runtime_library;
}

Api *TryGetApi() noexcept {
	return RuntimeLibrary().IsAvailable() ? &api : nullptr;
}

}

struct Fftw3SpectrumTransform::Impl {
	Api *api = nullptr;
	Plan plan = nullptr;
	double *input = nullptr;
	Complex *output = nullptr;
	size_t sample_count = 0;

	Impl(Api *api, size_t sample_count)
		: api(api)
		, sample_count(sample_count) {
	}

	~Impl() {
		if (!api)
			return;
		if (plan)
			api->destroy_plan(plan);
		if (input)
			api->free(input);
		if (output)
			api->free(output);
	}
};

Fftw3SpectrumTransform::Fftw3SpectrumTransform(std::unique_ptr<Impl> impl)
	: impl(std::move(impl)) {
}

Fftw3SpectrumTransform::~Fftw3SpectrumTransform() = default;
Fftw3SpectrumTransform::Fftw3SpectrumTransform(Fftw3SpectrumTransform&&) noexcept = default;
Fftw3SpectrumTransform& Fftw3SpectrumTransform::operator=(Fftw3SpectrumTransform&&) noexcept = default;

bool Fftw3SpectrumTransform::IsReady() const noexcept {
	return impl && impl->plan;
}

bool Fftw3SpectrumTransform::Execute(float const *samples, float *block, size_t bin_count, double scale_factor) {
	if (!IsReady())
		return false;

	for (size_t i = 0; i < impl->sample_count; ++i)
		impl->input[i] = samples[i];

	impl->api->execute(impl->plan);

	auto *out = impl->output;
	for (size_t i = 0; i < bin_count; ++i, ++out)
		block[i] = std::log10(std::sqrt(static_cast<float>((*out)[0] * (*out)[0] + (*out)[1] * (*out)[1])) * static_cast<float>(scale_factor) + 1.f);

	return true;
}

std::unique_ptr<Fftw3SpectrumTransform> TryCreateFftw3SpectrumTransform(size_t sample_count) {
	auto *fftw = TryGetApi();
	if (!fftw)
		return nullptr;

	auto impl = std::unique_ptr<Fftw3SpectrumTransform::Impl>(new Fftw3SpectrumTransform::Impl(fftw, sample_count));
	impl->input = fftw->alloc_real(sample_count);
	impl->output = fftw->alloc_complex(sample_count);
	if (!impl->input || !impl->output)
		return nullptr;

	impl->plan = fftw->plan_dft_r2c_1d(
		static_cast<int>(sample_count),
		impl->input,
		impl->output,
		estimate);
	if (!impl->plan)
		return nullptr;

	return std::unique_ptr<Fftw3SpectrumTransform>(new Fftw3SpectrumTransform(std::move(impl)));
}

}

#endif
