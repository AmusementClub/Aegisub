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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio_renderer.h"

class AudioColorScheme;
class AudioDisplaySource;
class AudioSpectrumAnalysisCache;

/// How the FFT bins are mapped to display pixels.
enum class AudioSpectrumComputationMode {
	LegacyLinear = 0,  ///< Linear bin mapping (original behaviour)
	FrequencyCurve = 1, ///< Frequency-aware curve mapping with configurable presets
};

/// How multi-channel audio is collapsed to a single power spectrum.
enum class AudioSpectrumMonoMixMode {
	MonoAverage = 0,        ///< Time-domain downmix (average all channels, single FFT)
	PerBinMaxPower = 1,     ///< Per-bin strongest channel (one FFT per channel, take max per bin)
	PerBinAveragePower = 2, ///< Per-bin average energy (one FFT per channel, average per bin)
};

/// Where the renderer gets its mono float audio from.
///
/// Int16Mono uses the provider's built-in mono downmix path (GetInt16MonoAudio)
/// and decodes to float via SIMD. This is equivalent to the legacy behaviour.
///
/// Float32 fetches full per-channel float audio and mixes to mono in software.
/// This preserves original float precision and enables per-channel operations
/// such as PerBinMaxPower and PerBinAveragePower mono mix modes.
enum class AudioSpectrumInputFormat {
	Int16Mono = 0, ///< Provider s16 mono, decoded to float by the renderer
	Float32 = 1,   ///< Float32 display source, software downmix
};

/// @class AudioSpectrumRenderer
/// @brief Render frequency-power spectrum graphs for audio data.
///
/// Renders frequency-power spectrum graphs of PCM audio data using a derivation function
/// such as the fast fourier transform.
class AudioSpectrumRenderer final : public AudioRendererBitmapProvider {
	/// Display source providing float audio (mono for Int16Mono, per-channel for Float32)
	std::unique_ptr<AudioDisplaySource> display_source;

	/// Primary FFT cache for the mono/aggregated spectrum
	std::unique_ptr<AudioSpectrumAnalysisCache> analysis_cache;

	/// Per-channel float audio sources, created only for per-bin aggregation paths
	std::vector<std::unique_ptr<AudioDisplaySource>> per_channel_sources;

	/// Per-channel FFT caches, created only for per-bin aggregation paths
	std::vector<std::unique_ptr<AudioSpectrumAnalysisCache>> per_channel_caches;

	/// Colour tables used for rendering
	std::vector<AudioColorScheme> colors;

	/// Binary logarithm of number of samples to use in deriving frequency-power data
	size_t derivation_size = 0;

	/// Binary logarithm of number of samples between the start of derivations
	size_t derivation_dist = 0;

	/// How FFT bins are mapped to display pixels
	AudioSpectrumComputationMode computation_mode = AudioSpectrumComputationMode::LegacyLinear;

	/// Multi-channel collapse strategy
	AudioSpectrumMonoMixMode mono_mix_mode = AudioSpectrumMonoMixMode::MonoAverage;

	/// Audio input format (int16 mono or float32 per-channel)
	AudioSpectrumInputFormat input_format = AudioSpectrumInputFormat::Int16Mono;

	/// Reference position within the frequency range for curve blending (0..1)
	/// 0 = fully linear, 1 = fully logarithmic. Set from frequency_curve_preset.
	float frequency_reference_position = 1.0f / 3.0f;

	/// Preset index for the frequency curve mapping (0=Linear .. 4=Logarithmic)
	int frequency_curve_preset = 2;

	// ---- Render scale cache ----
	// The band mapping arrays (render_band_a/b/frac) are expensive to compute;
	// they are cached and only recomputed when a relevant parameter changes.

	std::vector<int> render_band_a;       ///< Lower bin index per screen row
	std::vector<int> render_band_b;       ///< Upper bin index per screen row
	std::vector<float> render_band_frac;  ///< Interpolation fraction per screen row (only when interpolated)

	int render_scale_cache_height = 0;
	size_t render_scale_cache_derivation_size = 0;
	bool render_scale_cache_interpolated = false;
	int render_scale_cache_sample_rate = 0;
	int render_scale_cache_mode = -1;
	float render_scale_cache_reference_position = 0.0f;

	// ---- Per-frame scratch buffers ----
	// Reused across Render() calls to avoid per-frame allocations.

	/// Pointers to FFT power data for each pixel column (mono/aggregated path)
	std::vector<const float *> power_columns;

	/// Pointers to per-channel FFT power data for the current pixel column
	std::vector<const float *> channel_power_inputs;

	/// Pointers to merged per-bin power data per pixel column (per-bin aggregation path)
	std::vector<const float *> combined_power_columns;

	/// Pre-allocated buffer for merged per-bin power data
	std::vector<float> combined_power_scratch;

	/// @brief Reset in response to changing audio provider
	///
	/// Overrides the OnSetProvider event handler in the base class, to reset things
	/// when the audio provider is changed.
	void OnSetProvider() override;

	/// Recreate the display source from the current provider and input format
	void RecreateDisplaySource();

	/// Recreate the primary and per-channel FFT caches
	void RecreateCache();

	/// Ensure per-channel caches exist when per-bin aggregation is active.
	/// Must be called after RecreateDisplaySource() so display_source is valid.
	void EnsurePerChannelCaches();

	/// Whether the current configuration uses per-channel per-bin aggregation
	bool UsesPerChannelMonoAggregation() const;

	/// Recompute the render scale band mapping cache for the given image height
	void EnsureRenderScaleCache(int imgheight);

	/// Set the frequency reference position (clamped to [0.001, 0.999])
	void SetFrequencyReferencePosition(float position);

public:
	/// @brief Constructor
	/// @param color_scheme_name Name of the color scheme to use
	AudioSpectrumRenderer(std::string const& color_scheme_name);

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

	/// @brief Set the computation mode (linear or frequency curve)
	void SetComputationMode(AudioSpectrumComputationMode mode);

	/// @brief Set the frequency curve preset (0=Linear .. 4=Logarithmic)
	void SetFrequencyCurvePreset(int preset);

	/// @brief Set the mono mix mode (time-domain downmix or per-bin aggregation)
	void SetMonoMixMode(AudioSpectrumMonoMixMode mode);

	/// @brief Set the audio input format (int16 mono or float32 per-channel)
	void SetInputFormat(AudioSpectrumInputFormat format);

	/// @brief Cleans up the cache
	/// @param max_size Maximum size in bytes for the cache
	void AgeCache(size_t max_size) override;
};
