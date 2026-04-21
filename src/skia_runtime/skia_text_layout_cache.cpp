// Copyright (c) 2026
// All rights reserved.

#include "skia_runtime/skia_text_layout_cache.h"

#ifdef WITH_SKIA
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkTypeface.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#endif
#endif

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
#ifdef WITH_SKIA
SkColor ToSkColor(wxColour const& colour) {
	return SkColorSetARGB(colour.Alpha(), colour.Red(), colour.Green(), colour.Blue());
}

SkFontStyle ToSkFontStyle(VideoOverlayTextStyle const& style) {
	return SkFontStyle(
		style.bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
		SkFontStyle::kNormal_Width,
		style.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
}

sk_sp<SkFontMgr> CreateFontManager() {
#ifdef _WIN32
	if (auto font_mgr = SkFontMgr_New_DirectWrite())
		return font_mgr;

	if (auto font_mgr = SkFontMgr_New_GDI())
		return font_mgr;
#endif

	return SkFontMgr::RefEmpty();
}

SkFontMgr *GetFontManager() {
	static sk_sp<SkFontMgr> const font_mgr = CreateFontManager();
	return font_mgr.get();
}
#endif
}

bool SkiaTextLayoutCache::FontKey::operator==(FontKey const& other) const {
	return face == other.face
		&& size == other.size
		&& bold == other.bold
		&& italic == other.italic;
}

size_t SkiaTextLayoutCache::FontKeyHash::operator()(FontKey const& key) const noexcept {
	size_t hash = std::hash<std::string>()(key.face);
	hash ^= std::hash<int>()(key.size) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
	hash ^= std::hash<bool>()(key.bold) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
	hash ^= std::hash<bool>()(key.italic) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
	return hash;
}

sk_sp<SkTypeface> SkiaTextLayoutCache::ResolveTypeface(VideoOverlayTextStyle const& style) {
#ifdef WITH_SKIA
	FontKey const key { style.face, style.size, style.bold, style.italic };
	auto const existing = typefaces.find(key);
	if (existing != typefaces.end())
		return existing->second;

	auto *font_mgr = GetFontManager();
	auto typeface = font_mgr
		? font_mgr->matchFamilyStyle(
			style.face.empty() ? nullptr : style.face.c_str(),
			ToSkFontStyle(style))
		: nullptr;
	if (!typeface && font_mgr)
		typeface = font_mgr->matchFamilyStyle(nullptr, ToSkFontStyle(style));
	if (!typeface && font_mgr)
		typeface = font_mgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
	if (!typeface)
		return nullptr;
	typefaces.emplace(key, typeface);
	return typeface;
#else
	(void)style;
	return nullptr;
#endif
}

wxSize SkiaTextLayoutCache::MeasureText(std::string const& text, VideoOverlayTextStyle const& style) {
#ifdef WITH_SKIA
	auto typeface = ResolveTypeface(style);
	if (!typeface)
		return wxSize(0, 0);
	SkFont font(typeface, style.size);
	font.setEdging(SkFont::Edging::kAntiAlias);

	SkRect bounds;
	float const advance = font.measureText(
		text.data(),
		text.size(),
		SkTextEncoding::kUTF8,
		&bounds,
		nullptr);

	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return wxSize(
		static_cast<int>(std::ceil(std::max(advance, bounds.width()))),
		static_cast<int>(std::ceil(metrics.fDescent - metrics.fAscent)));
#else
	(void)text;
	(void)style;
	return wxSize();
#endif
}

void SkiaTextLayoutCache::DrawText(SkCanvas &canvas, std::string const& text, int x, int y, VideoOverlayTextStyle const& style) {
#ifdef WITH_SKIA
	auto typeface = ResolveTypeface(style);
	if (!typeface)
		return;
	SkFont font(typeface, style.size);
	font.setEdging(SkFont::Edging::kAntiAlias);

	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	float const baseline = static_cast<float>(y) - metrics.fAscent;

	SkPaint paint;
	paint.setAntiAlias(true);
	paint.setColor(ToSkColor(style.colour));

	if (style.outline) {
		SkPaint outline = paint;
		outline.setStyle(SkPaint::kStroke_Style);
		outline.setStrokeWidth(2.0f);
		outline.setColor(SK_ColorBLACK);
		canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, outline);
	}

	canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, paint);
#else
	(void)canvas;
	(void)text;
	(void)x;
	(void)y;
	(void)style;
#endif
}
