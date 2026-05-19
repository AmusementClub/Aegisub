#pragma once

#include <cstddef>
#include <memory>
#include <string>

#ifdef WITH_FFTW3

namespace audio::spectrum {

class Fftw3SpectrumTransform;
std::unique_ptr<Fftw3SpectrumTransform> TryCreateFftw3SpectrumTransform(size_t sample_count);
std::string GetFftw3LoadError();

class Fftw3SpectrumTransform {
	struct Impl;
	std::unique_ptr<Impl> impl;

	explicit Fftw3SpectrumTransform(std::unique_ptr<Impl> impl);
	friend std::unique_ptr<Fftw3SpectrumTransform> TryCreateFftw3SpectrumTransform(size_t sample_count);

public:
	~Fftw3SpectrumTransform();

	Fftw3SpectrumTransform(Fftw3SpectrumTransform const&) = delete;
	Fftw3SpectrumTransform& operator=(Fftw3SpectrumTransform const&) = delete;
	Fftw3SpectrumTransform(Fftw3SpectrumTransform&&) noexcept;
	Fftw3SpectrumTransform& operator=(Fftw3SpectrumTransform&&) noexcept;

	bool IsReady() const noexcept;
	bool Execute(float const *samples, float *block, size_t bin_count, double scale_factor);
};

}

#endif
