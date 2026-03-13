// Copyright (c) 2005-2006, Rodrigo Braz Monteiro
// Copyright (c) 2006-2010, Niels Martin Hansen
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

/// @file audio_renderer_spectrum.cpp
/// @brief Caching frequency-power spectrum renderer for audio display
/// @ingroup audio_ui

#include "audio_renderer_spectrum.h"

#include "audio_colorscheme.h"
#include "audio_display_source.h"
#include "audio_mix_policy.h"
#ifndef WITH_FFTW3
#include "fft.h"
#endif

#include <libaegisub/audio/provider.h>
#include <libaegisub/make_unique.h>

#include <algorithm>

#include <wx/image.h>
#include <wx/dcmemory.h>

namespace {
inline float max_inclusive_range(const float *values, int first, int last) {
	float max_value = values[first];
	for (int i = first + 1; i <= last; ++i) {
		if (values[i] > max_value)
			max_value = values[i];
	}
	return max_value;
}
}

/// Allocates blocks of derived data for the audio spectrum
struct AudioSpectrumCacheBlockFactory {
	typedef std::unique_ptr<float, std::default_delete<float[]>> BlockType;

	/// Pointer back to the owning spectrum renderer
	AudioSpectrumRenderer *spectrum;

	/// @brief Allocate and fill a data block
	/// @param i Index of the block to produce data for
	/// @return Newly allocated and filled block
	///
	/// The filling is delegated to the spectrum renderer
	BlockType ProduceBlock(size_t i)
	{
		auto res = new float[((size_t)1)<<spectrum->derivation_size];
		spectrum->FillBlock(i, res);
		return BlockType(res);
	}

	/// @brief Calculate the in-memory size of a spec
	/// @return The size in bytes of a spectrum cache block
	size_t GetBlockSize() const
	{
		return sizeof(float) << spectrum->derivation_size;
	}
};

/// @brief Cache for audio spectrum frequency-power data
class AudioSpectrumCache
: public DataBlockCache<float, 10, AudioSpectrumCacheBlockFactory> {
public:
	AudioSpectrumCache(size_t block_count, AudioSpectrumRenderer *renderer)
	: DataBlockCache(block_count, AudioSpectrumCacheBlockFactory{renderer})
	{
	}
};

AudioSpectrumRenderer::AudioSpectrumRenderer(std::string const& color_scheme_name)
{
	colors.reserve(AudioStyle_MAX);
	for (int i = 0; i < AudioStyle_MAX; ++i)
		colors.emplace_back(12, color_scheme_name, i);
}

void AudioSpectrumRenderer::EnsureRenderScaleCache(int imgheight) {
	bool interpolated = imgheight > 1 << derivation_size;
	if (render_scale_cache_height == imgheight
		&& render_scale_cache_derivation_size == derivation_size
		&& render_scale_cache_interpolated == interpolated)
		return;

	render_scale_cache_height = imgheight;
	render_scale_cache_derivation_size = derivation_size;
	render_scale_cache_interpolated = interpolated;

	render_band_a.resize(imgheight);
	render_band_b.resize(imgheight);
	render_band_frac.resize(interpolated ? imgheight : 0);

	const int maxband = 1 << derivation_size;
	if (interpolated) {
		for (int y = 0; y < imgheight; ++y) {
			double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
			int lower = std::max(0, std::min(maxband - 1, static_cast<int>(std::floor(ideal))));
			int upper = std::max(0, std::min(maxband - 1, static_cast<int>(std::ceil(ideal))));
			render_band_a[y] = lower;
			render_band_b[y] = upper;
			render_band_frac[y] = static_cast<float>(ideal - std::floor(ideal));
		}
	}
	else {
		for (int y = 0; y < imgheight; ++y) {
			int sample1 = std::max(0, maxband * y / imgheight);
			int sample2 = std::min(maxband - 1, maxband * (y + 1) / imgheight);
			render_band_a[y] = sample1;
			render_band_b[y] = sample2;
		}
	}
}

AudioSpectrumRenderer::~AudioSpectrumRenderer()
{
	// This sequence will clean up
	provider = nullptr;
	RecreateCache();
}

void AudioSpectrumRenderer::RecreateCache()
{
	rolling_window_valid = false;
#ifdef WITH_FFTW3
	if (dft_plan)
	{
		fftw_destroy_plan(dft_plan);
		fftw_free(dft_input);
		fftw_free(dft_output);
		dft_plan = nullptr;
		dft_input = nullptr;
		dft_output = nullptr;
	}
#endif

	if (provider)
	{
		size_t block_count = (size_t)((provider->GetNumSamples() + ((size_t)1<<derivation_dist) - 1) >> derivation_dist);
		cache = agi::make_unique<AudioSpectrumCache>(block_count, this);

#ifdef WITH_FFTW3
		dft_input = fftw_alloc_real(2<<derivation_size);
		dft_output = fftw_alloc_complex(2<<derivation_size);
		dft_plan = fftw_plan_dft_r2c_1d(
			2<<derivation_size,
			dft_input,
			dft_output,
			FFTW_MEASURE);
#else
		// Allocate scratch for 6x the derivation size:
		// 2x for the input sample data
		// 2x for the real part of the output
		// 2x for the imaginary part of the output
		fft_scratch.resize(6 << derivation_size);
#endif
		audio_scratch.resize(2 << derivation_size);
	}
}

void AudioSpectrumRenderer::OnSetProvider()
{
	RecreateCache();
}

void AudioSpectrumRenderer::SetResolution(size_t _derivation_size, size_t _derivation_dist)
{
	if (derivation_dist != _derivation_dist)
	{
		derivation_dist = _derivation_dist;
		if (cache)
			cache->Age(0);
	}

	if (derivation_size != _derivation_size)
	{
		derivation_size = _derivation_size;
		RecreateCache();
	}
}

void AudioSpectrumRenderer::FillBlock(size_t block_index, float *block)
{
	assert(cache);
	assert(block);
	assert(display_source);

	const int channels = std::max(1, display_source->GetChannels());
	const size_t sample_count = static_cast<size_t>(2) << derivation_size;
	const size_t hop_samples = static_cast<size_t>(1) << derivation_dist;
	int64_t first_sample = (((int64_t)block_index) << derivation_dist) - ((int64_t)1 << derivation_size);
	mono_scratch.resize(sample_count);

	bool reused_window = rolling_window_valid
		&& block_index == rolling_window_block_index + 1
		&& hop_samples <= sample_count;

	if (reused_window) {
		const size_t overlap_samples = sample_count - hop_samples;
		std::move(mono_scratch.begin() + hop_samples, mono_scratch.end(), mono_scratch.begin());

		audio_scratch.resize(hop_samples * channels);
		display_source->GetFloatAudio(audio_scratch.data(), first_sample + overlap_samples, hop_samples);
		float *tail = mono_scratch.data() + overlap_samples;
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + hop_samples, tail);
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(hop_samples), channels, tail);
	}
	else {
		audio_scratch.resize(sample_count * channels);
		display_source->GetFloatAudio(audio_scratch.data(), first_sample, sample_count);
		if (channels == 1)
			std::copy(audio_scratch.begin(), audio_scratch.begin() + sample_count, mono_scratch.begin());
		else
			MixAudioToMono(mix_policy, audio_scratch.data(), static_cast<int>(sample_count), channels, mono_scratch.data());
	}

	rolling_window_valid = true;
	rolling_window_block_index = block_index;

#ifdef WITH_FFTW3
	for (size_t i = 0; i < sample_count; ++i)
		dft_input[i] = mono_scratch[i];

	fftw_execute(dft_plan);

	double scale_factor = 9 / sqrt(2 << (derivation_size + 1));

	fftw_complex *o = dft_output;
	for (size_t si = (size_t)1<<derivation_size; si > 0; --si)
	{
		*block++ = log10( sqrt(o[0][0] * o[0][0] + o[0][1] * o[0][1]) * scale_factor + 1 );
		o++;
	}
#else
	std::copy(mono_scratch.begin(), mono_scratch.begin() + sample_count, &fft_scratch[0]);

	float *fft_input = &fft_scratch[0];
	float *fft_real = &fft_scratch[0] + (2 << derivation_size);
	float *fft_imag = &fft_scratch[0] + (4 << derivation_size);

	FFT fft;
	fft.Transform(2<<derivation_size, fft_input, fft_real, fft_imag);

	float scale_factor = 9 / sqrt(2 * (float)(2<<derivation_size));

	for (size_t si = 1<<derivation_size; si > 0; --si)
	{
		// With x in range [0;1], log10(x*9+1) will also be in range [0;1],
		// although the FFT output can apparently get greater magnitudes than 1
		// despite the input being limited to [-1;+1).
		*block++ = log10( sqrt(*fft_real * *fft_real + *fft_imag * *fft_imag) * scale_factor + 1 );
		fft_real++; fft_imag++;
	}
#endif
}

void AudioSpectrumRenderer::Render(wxBitmap &bmp, int start, AudioRenderingStyle style)
{
	if (!cache)
		return;

	assert(bmp.IsOk());

	int end = start + bmp.GetWidth();

	assert(start >= 0);
	assert(end >= start);

	// Prepare an image buffer to write
	wxImage img(bmp.GetSize());
	unsigned char *imgdata = img.GetData();
	ptrdiff_t stride = img.GetWidth()*3;
	int imgheight = img.GetHeight();

	const AudioColorScheme *pal = &colors[style];

	/// @todo Make minband and maxband configurable
	EnsureRenderScaleCache(imgheight);
	const bool interpolated = imgheight > 1 << derivation_size;
	const double samples_per_pixel = pixel_ms * display_source->GetSampleRate() / 1000.0;
	size_t last_block_index = static_cast<size_t>(-1);
	float *last_power = nullptr;
	const int *band_a = render_band_a.data();
	const int *band_b = render_band_b.data();
	const float *band_frac = interpolated ? render_band_frac.data() : nullptr;

	// ax = absolute x, absolute to the virtual spectrum bitmap
	for (int ax = start; ax < end; ++ax)
	{
		// Derived audio data
		size_t block_index = static_cast<size_t>(ax * samples_per_pixel) >> derivation_dist;
		float *power;
		if (block_index == last_block_index)
			power = last_power;
		else {
			power = &cache->Get(block_index);
			last_block_index = block_index;
			last_power = power;
		}

		// Prepare bitmap writing
		unsigned char *px = imgdata + (imgheight-1) * stride + (ax - start) * 3;

		// Scale up or down vertically?
		if (interpolated)
		{
			// Interpolate
			for (int y = 0; y < imgheight; ++y)
			{
				assert(px >= imgdata);
				assert(px < imgdata + imgheight*stride);
				float sample1 = power[band_a[y]];
				float sample2 = power[band_b[y]];
				float frac = band_frac[y];
				float val = (1-frac)*sample1 + frac*sample2;
				pal->map(val*amplitude_scale, px);
				px -= stride;
			}
		}
		else
		{
			// Pick greatest
			for (int y = 0; y < imgheight; ++y)
			{
				assert(px >= imgdata);
				assert(px < imgdata + imgheight*stride);
				float maxval = max_inclusive_range(power, band_a[y], band_b[y]);
				pal->map(maxval*amplitude_scale, px);
				px -= stride;
			}
		}
	}

	wxBitmap tmpbmp(img);
	wxMemoryDC targetdc(bmp);
	targetdc.DrawBitmap(tmpbmp, 0, 0);
}

void AudioSpectrumRenderer::RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style)
{
	// Get the colour of silence
	wxColour col = colors[style].get(0.0f);
	dc.SetBrush(wxBrush(col));
	dc.SetPen(wxPen(col));
	dc.DrawRectangle(rect);
}

void AudioSpectrumRenderer::AgeCache(size_t max_size)
{
	if (cache)
		cache->Age(max_size);
}
