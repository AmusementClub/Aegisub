#pragma once

#include <cstddef>
#include <vector>

#include <wx/bitmap.h>

class AudioColorScheme;

void RenderSpectrumColumnsToBitmap(
	wxBitmap &bmp,
	const std::vector<const float *> &power_columns,
	size_t derivation_size,
	const int *band_a,
	const int *band_b,
	const float *band_frac,
	bool interpolated,
	float amplitude_scale,
	const AudioColorScheme &palette);

void RenderSpectrumBlockToBitmap(
	wxBitmap &bmp,
	const float *power,
	size_t derivation_size,
	float amplitude_scale,
	const AudioColorScheme &palette);
