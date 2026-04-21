// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "audio_display_analysis.h"
#include "audio_display_types.h"
#include "audio_marker.h"
#include "audio_rendering_style.h"
#include "audio_tile_compositor.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class AudioDisplayContentKind {
	None = 0,
	Waveform,
	Spectrum,
};

struct AudioDisplayWaveformPalette {
	uint32_t background = 0xFF000000u;
	uint32_t peak_line = 0xFF00FFFFu;
	uint32_t average_line = 0xFF00FFFFu;
	uint32_t baseline = 0xFF00FFFFu;
};

struct AudioDisplayWaveformColumn {
	AudioWaveformSummary summary;
	bool ready = false;
};

struct AudioDisplayWaveformRenderData {
	int pixel_origin = 0;
	bool render_averages = false;
	float amplitude_scale = 1.0f;
	std::array<AudioDisplayWaveformPalette, AudioStyle_MAX> palettes;
	std::vector<AudioDisplayWaveformColumn> columns;

	void Reset() {
		pixel_origin = 0;
		render_averages = false;
		amplitude_scale = 1.0f;
		columns.clear();  // preserves capacity
	}
};

constexpr size_t AudioDisplaySpectrumPaletteSize = 256;

struct AudioDisplaySpectrumPalette {
	std::array<uint32_t, AudioDisplaySpectrumPaletteSize> colours;
};

struct AudioDisplaySpectrumRenderData {
	int pixel_origin = 0;
	int channel_count = 1;
	int channel_band_height = 0;
	int bins_per_column = 0;
	bool interpolated = false;
	float amplitude_scale = 1.0f;
	uint32_t channel_divider = AudioDisplayPackColour(80, 80, 80);
	std::array<AudioDisplaySpectrumPalette, AudioStyle_MAX> palettes;
	std::vector<int> band_a;
	std::vector<int> band_b;
	std::vector<float> band_frac;
	std::vector<uint8_t> ready;
	std::vector<float> power;

	void Reset() {
		pixel_origin = 0;
		channel_count = 1;
		channel_band_height = 0;
		bins_per_column = 0;
		interpolated = false;
		amplitude_scale = 1.0f;
		channel_divider = AudioDisplayPackColour(80, 80, 80);
		band_a.clear();
		band_b.clear();
		band_frac.clear();
		ready.clear();
		power.clear();
	}
};

struct AudioDisplayScrollbarRenderData {
	bool visible = false;
	AudioDisplayRect bounds;
	AudioDisplayRect thumb;
	AudioDisplayRect selection_rect;
	AudioDisplayRect load_marker_rect;
	bool has_selection = false;
	bool has_load_marker = false;
	uint32_t light_colour = 0xFFFFFFFFu;
	uint32_t dark_colour = 0xFF000000u;
	uint32_t selection_colour = 0xFF000000u;
};

struct AudioDisplayTimelineTick {
	int x = 0;
	bool major = false;
	std::string label;
};

struct AudioDisplayTimelineRenderData {
	bool visible = false;
	AudioDisplayRect bounds;
	uint32_t light_colour = 0xFFFFFFFFu;
	uint32_t dark_colour = 0xFF000000u;
	std::vector<AudioDisplayTimelineTick> ticks;
};

struct AudioDisplayMarkerRenderData {
	int x = 0;
	int top = 0;
	int bottom = 0;
	AudioDisplayPenStyle style;
	int feet = 0;
};

struct AudioDisplayRangeLabelRenderData {
	std::string text;
	int left = 0;
	int width = 0;
	int top = 0;
};

struct AudioDisplayTrackCursorRenderData {
	bool visible = false;
	int x = 0;
	int top = 0;
	int bottom = 0;
	std::string label;
};

struct AudioDisplayRenderModel {
	AudioViewportRequest viewport;
	AudioDisplayRect audio_bounds;
	TimeRange viewport_time = TimeRange(0, 0);
	std::vector<std::pair<int, int>> style_ranges;
	AudioDisplayContentKind content_kind = AudioDisplayContentKind::None;
	AudioDisplayWaveformRenderData waveform;
	AudioDisplaySpectrumRenderData spectrum;
	std::vector<const AudioMarker*> markers;
	std::vector<AudioLabelProvider::AudioLabel> labels;
	std::vector<AudioDisplayMarkerRenderData> marker_geometry;
	std::vector<AudioDisplayRangeLabelRenderData> label_geometry;
	std::vector<std::string> split_channel_labels;
	AudioDisplayScrollbarRenderData scrollbar;
	AudioDisplayTimelineRenderData timeline;
	AudioDisplayTrackCursorRenderData track_cursor;
	std::string audio_label_font_face;
	bool redraw_scrollbar = false;
	bool redraw_timeline = false;
	bool track_cursor_visible = false;
	int track_cursor_absolute_x = -1;
	std::string track_cursor_label;

	/// Reset all fields for reuse, preserving vector capacities.
	void Reset() {
		viewport = AudioViewportRequest();
		audio_bounds = AudioDisplayRect();
		viewport_time = TimeRange(0, 0);
		style_ranges.clear();
		content_kind = AudioDisplayContentKind::None;
		waveform.Reset();
		spectrum.Reset();
		markers.clear();
		labels.clear();
		marker_geometry.clear();
		label_geometry.clear();
		split_channel_labels.clear();
		scrollbar = AudioDisplayScrollbarRenderData();
		timeline.ticks.clear();
		timeline.visible = false;
		track_cursor = AudioDisplayTrackCursorRenderData();
		audio_label_font_face.clear();
		redraw_scrollbar = false;
		redraw_timeline = false;
		track_cursor_visible = false;
		track_cursor_absolute_x = -1;
		track_cursor_label.clear();
	}
};
