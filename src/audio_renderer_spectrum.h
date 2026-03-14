// Copyright (c) 2009, Niels Martin Hansen
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

/// @file audio_renderer_spectrum.h
/// @see audio_renderer_spectrum.cpp
/// @ingroup audio_ui
///
/// Calculate and render a frequency-power spectrum for PCM audio data.

#include <cstdint>
#include <memory>
#include <vector>

#include "audio_renderer.h"
#include "audio_mix_policy.h"

class AudioColorScheme;
class AudioSpectrumAnalysisCache;

enum class AudioSpectrumComputationMode {
	LegacyLinear = 0,
	FrequencyCurve = 1,
};

/// @class AudioSpectrumRenderer
/// @brief Render frequency-power spectrum graphs for audio data.
///
/// Renders frequency-power spectrum graphs of PCM audio data using a derivation function
/// such as the fast fourier transform.
class AudioSpectrumRenderer final : public AudioRendererBitmapProvider {
	std::unique_ptr<AudioSpectrumAnalysisCache> analysis_cache;

	/// Colour tables used for rendering
	std::vector<AudioColorScheme> colors;

	/// Binary logarithm of number of samples to use in deriving frequency-power data
	size_t derivation_size = 0;

	/// Binary logarithm of number of samples between the start of derivations
	size_t derivation_dist = 0;

	/// @brief Reset in response to changing audio provider
	///
	/// Overrides the OnSetProvider event handler in the base class, to reset things
	/// when the audio provider is changed.
	void OnSetProvider() override;

	/// @brief Recreates the cache
	///
	/// To be called when the number of blocks in cache might have changed,
	/// e.g. new audio provider or new resolution.
	void RecreateCache();

	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	AudioSpectrumComputationMode computation_mode = AudioSpectrumComputationMode::LegacyLinear;
	float frequency_reference_position = 1.0f / 3.0f;
	int frequency_curve_preset = 2;
	std::vector<int> render_band_a;
	std::vector<int> render_band_b;
	std::vector<float> render_band_frac;
	int render_scale_cache_height = 0;
	size_t render_scale_cache_derivation_size = 0;
	bool render_scale_cache_interpolated = false;
	int render_scale_cache_sample_rate = 0;
	int render_scale_cache_mode = -1;
	float render_scale_cache_reference_position = 0.0f;
	void EnsureRenderScaleCache(int imgheight);
	void SetFrequencyReferencePosition(float position);

public:
	/// @brief Constructor
	/// @param color_scheme_name Name of the color scheme to use
	AudioSpectrumRenderer(std::string const& color_scheme_name);

	/// @brief Destructor
	~AudioSpectrumRenderer();

	/// @brief Render a range of audio spectrum
	/// @param bmp   [in,out] Bitmap to render into, also carries length information
	/// @param start First column of pixel data in display to render
	/// @param style Style to render audio in
	void Render(wxBitmap &bmp, int start, AudioRenderingStyle style) override;

	/// @brief Render blank area
	void RenderBlank(wxDC &dc, const wxRect &rect, AudioRenderingStyle style) override;

	/// @brief Set the derivation resolution
	/// @param derivation_size Binary logarithm of number of samples to use in deriving frequency-power data
	/// @param derivation_dist Binary logarithm of number of samples between the start of derivations
	///
	/// The derivations done will each use 2^derivation_size audio samples and at a distance
	/// of 2^derivation_dist samples.
	///
	/// The derivation distance must be smaller than or equal to the size. If the distance
	/// is specified too large, it will be clamped to the size.
	void SetResolution(size_t derivation_size, size_t derivation_dist);
	void SetComputationMode(AudioSpectrumComputationMode mode);
	void SetFrequencyCurvePreset(int preset);

	/// @brief Cleans up the cache
	/// @param max_size Maximum size in bytes for the cache
	void AgeCache(size_t max_size) override;
	void SetInteractivePrefetchEnabled(bool enabled) override;
	std::vector<std::string> GetDebugInfo() const override;
};
