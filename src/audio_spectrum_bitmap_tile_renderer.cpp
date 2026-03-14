#include "audio_spectrum_bitmap_tile_renderer.h"

#include "audio_colorscheme.h"

#include <algorithm>
#include <cmath>

#include <wx/dcmemory.h>
#include <wx/image.h>

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

void RenderSpectrumColumnsToBitmap(
	wxBitmap &bmp,
	const std::vector<const float *> &power_columns,
	size_t derivation_size,
	const int *band_a,
	const int *band_b,
	const float *band_frac,
	bool interpolated,
	float amplitude_scale,
	const AudioColorScheme &palette)
{
	if (power_columns.empty())
		return;

	wxImage img(bmp.GetSize());
	unsigned char *imgdata = img.GetData();
	ptrdiff_t stride = img.GetWidth() * 3;
	int imgheight = img.GetHeight();
	const int maxband = 1 << derivation_size;
	std::vector<int> local_band_a;
	std::vector<int> local_band_b;
	std::vector<float> local_band_frac;

	if (!band_a || !band_b || (interpolated && !band_frac)) {
		local_band_a.resize(imgheight);
		local_band_b.resize(imgheight);
		if (interpolated)
			local_band_frac.resize(imgheight);

		if (interpolated) {
			for (int y = 0; y < imgheight; ++y) {
				double ideal = static_cast<double>(y + 1.) / imgheight * maxband;
				local_band_a[y] = std::max(0, std::min(maxband - 1, static_cast<int>(std::floor(ideal))));
				local_band_b[y] = std::max(0, std::min(maxband - 1, static_cast<int>(std::ceil(ideal))));
				local_band_frac[y] = static_cast<float>(ideal - std::floor(ideal));
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				local_band_a[y] = std::max(0, maxband * y / imgheight);
				local_band_b[y] = std::min(maxband - 1, maxband * (y + 1) / imgheight);
			}
		}

		band_a = local_band_a.data();
		band_b = local_band_b.data();
		band_frac = interpolated ? local_band_frac.data() : nullptr;
	}

	for (int x = 0; x < img.GetWidth(); ++x) {
		const float *power = (x < static_cast<int>(power_columns.size())) ? power_columns[x] : nullptr;
		if (!power)
			continue;

		unsigned char *px = imgdata + (imgheight - 1) * stride + x * 3;
		if (interpolated) {
			for (int y = 0; y < imgheight; ++y) {
				float frac = band_frac[y];
				float val = (1 - frac) * power[band_a[y]] + frac * power[band_b[y]];
				palette.map(val * amplitude_scale, px);
				px -= stride;
			}
		}
		else {
			for (int y = 0; y < imgheight; ++y) {
				palette.map(max_inclusive_range(power, band_a[y], band_b[y]) * amplitude_scale, px);
				px -= stride;
			}
		}
	}

	wxBitmap tmpbmp(img);
	wxMemoryDC targetdc(bmp);
	targetdc.DrawBitmap(tmpbmp, 0, 0);
}

void RenderSpectrumBlockToBitmap(
	wxBitmap &bmp,
	const float *power,
	size_t derivation_size,
	float amplitude_scale,
	const AudioColorScheme &palette)
{
	if (!power)
		return;
	std::vector<const float *> columns(static_cast<size_t>(bmp.GetWidth()), power);
	const bool interpolated = bmp.GetHeight() > 1 << derivation_size;
	RenderSpectrumColumnsToBitmap(bmp, columns, derivation_size, nullptr, nullptr, nullptr, interpolated, amplitude_scale, palette);
}
