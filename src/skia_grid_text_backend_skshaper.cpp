#if !defined(_WIN32)

#include "skia_grid_text_backend.h"

#include "grid_core/subtitle_grid_renderer_contract.h"
#include "skia_runtime/platform_font_runtime.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkFont.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRegion.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTextBlob.h>
#include <modules/skshaper/include/SkShaper.h>

#include <libaegisub/color.h>
#include <libaegisub/log.h>

#include <wx/font.h>
#include <wx/string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace aegisub::grid {
namespace {

constexpr std::size_t LayoutEntryLimit = 512;
constexpr std::size_t LayoutByteLimit = 8u * 1024u * 1024u;
constexpr std::size_t MaxLayoutTextUnits = 1024u * 1024u;

class AccumulateDuration final {
	std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
	double& total_;

public:
	explicit AccumulateDuration(double& total) noexcept : total_(total) { }
	~AccumulateDuration() {
		total_ += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started_).count();
	}
};

class MeasuringTextBlobRunHandler final : public SkShaper::RunHandler {
	SkTextBlobBuilderRunHandler blob_builder_;
	SkScalar line_advance_ = 0.f;
	SkScalar max_advance_ = 0.f;

public:
	MeasuringTextBlobRunHandler(char const *utf8, SkPoint origin)
	: blob_builder_(utf8, origin) {
	}

	void beginLine() override {
		line_advance_ = 0.f;
		blob_builder_.beginLine();
	}

	void runInfo(RunInfo const& info) override {
		// Horizontal bidi runs are positioned by the shaper, but every run's
		// magnitude still contributes to the line advance used for centering.
		line_advance_ += std::abs(info.fAdvance.fX);
		blob_builder_.runInfo(info);
	}

	void commitRunInfo() override { blob_builder_.commitRunInfo(); }
	Buffer runBuffer(RunInfo const& info) override {
		return blob_builder_.runBuffer(info);
	}
	void commitRunBuffer(RunInfo const& info) override {
		blob_builder_.commitRunBuffer(info);
	}
	void commitLine() override {
		max_advance_ = std::max(max_advance_, line_advance_);
		blob_builder_.commitLine();
	}

	sk_sp<SkTextBlob> makeBlob() { return blob_builder_.makeBlob(); }
	SkScalar width() const noexcept { return max_advance_; }
	SkScalar height() noexcept { return blob_builder_.endPoint().y(); }
};

class NativeSkShaperGridTextBackend final : public SkiaGridTextBackend {
	struct Entry {
		sk_sp<SkTextBlob> blob;
		int width = 0;
		int height = 0;
		std::size_t bytes = 0;
		std::list<std::wstring>::iterator lru;
	};

	std::unique_ptr<SkShaper> shaper_;
	SkFont font_;
	std::string family_;
	int weight_ = 0;
	bool italic_ = false;
	float size_ = 0.f;
	std::unordered_map<std::wstring, Entry> layouts_;
	std::list<std::wstring> lru_;
	std::size_t layout_bytes_ = 0;
	GridTextBackendStats stats_;

	void ClearLayouts() noexcept {
		layouts_.clear();
		lru_.clear();
		layout_bytes_ = 0;
	}

	Entry *GetLayout(std::wstring_view text, std::string& error) {
		if (text.size() > MaxLayoutTextUnits) {
			error = "native subtitle grid text is too long";
			return nullptr;
		}
		std::wstring key(text);
		if (auto found = layouts_.find(key); found != layouts_.end()) {
			lru_.splice(lru_.begin(), lru_, found->second.lru);
			++stats_.layout_hits;
			return &found->second;
		}
		++stats_.layout_misses;
		AccumulateDuration layout_timing(stats_.layout_ms);
		if (!shaper_ || !font_.getTypeface()) {
			error = "native SkShaper grid font is unavailable";
			return nullptr;
		}

		wxString const converted(text.data(), text.size());
		auto const utf8 = converted.utf8_string();
		MeasuringTextBlobRunHandler handler(utf8.data(), SkPoint::Make(0.f, 0.f));
		// SkShaper's convenience overload still performs Unicode bidi, script
		// segmentation, and font-manager fallback. The boolean is the paragraph
		// base direction, so derive it from the first strong character instead of
		// forcing mixed Arabic/Hebrew paragraphs to use an LTR base.
		auto const direction = ResolveSubtitleGridParagraphDirection(
			std::string_view(utf8.data(), utf8.size()));
		shaper_->shape(
			utf8.data(), utf8.size(), font_,
			direction == SubtitleGridParagraphDirection::LeftToRight,
			std::numeric_limits<SkScalar>::max() / 4.f, &handler);
		auto blob = handler.makeBlob();
		if (!blob) {
			error = "native SkShaper produced no grid text blob";
			return nullptr;
		}
		auto const width = std::max(0, static_cast<int>(std::ceil(handler.width())));
		auto const height = std::max(0, static_cast<int>(std::ceil(handler.height())));
		auto const bounds_width = blob->bounds().width();
		if (!std::isfinite(bounds_width) || bounds_width < 0.f
			|| bounds_width > static_cast<float>(LayoutByteLimit / 4u)) {
			error = "native subtitle grid layout has excessive bounds";
			return nullptr;
		}
		// Both the unordered-map key and the LRU list own a string copy. The
		// SkTextBlob's internal allocation remains an estimate; the byte and
		// entry caps are safety bounds, not allocator accounting.
		auto const key_storage_bytes = key.size() * sizeof(wchar_t);
		auto const bytes = sizeof(Entry) + 2u * sizeof(std::wstring)
			+ 2u * key_storage_bytes + utf8.size()
			+ static_cast<std::size_t>(bounds_width * 4.f);
		if (bytes > LayoutByteLimit) {
			error = "native subtitle grid layout exceeds the cache budget";
			return nullptr;
		}

		lru_.push_front(key);
		auto inserted = layouts_.emplace(std::move(key), Entry{
			std::move(blob), width, height, bytes, lru_.begin()});
		layout_bytes_ += bytes;
		while (layouts_.size() > LayoutEntryLimit || layout_bytes_ > LayoutByteLimit) {
			auto const victim = lru_.back();
			auto found = layouts_.find(victim);
			if (found != layouts_.end()) {
				layout_bytes_ -= found->second.bytes;
				layouts_.erase(found);
				++stats_.layout_evictions;
			}
			lru_.pop_back();
		}
		return &inserted.first->second;
	}

public:
	NativeSkShaperGridTextBackend() {
		auto manager = aegisub::skia::PlatformFontRuntime::Get().FontManager();
#if defined(__APPLE__)
		shaper_ = SkShaper::MakeCoreText();
#else
		shaper_ = SkShaper::MakeShaperDrivenWrapper(std::move(manager));
#endif
	}

	bool Available() const noexcept { return shaper_ != nullptr; }

	bool SetRenderTarget(
		SkiaGridTextRenderTarget const&, std::string&) override {
		return true;
	}

	bool SetFont(wxFont const& font, std::string& error) override {
		auto family = font.GetFaceName().utf8_string();
		auto const weight = std::clamp(font.GetNumericWeight(), 1, 1000);
		auto const italic = font.GetStyle() != wxFONTSTYLE_NORMAL;
		auto pixels = font.GetPixelSize().GetHeight();
		if (pixels <= 0)
			pixels = std::max(1, static_cast<int>(std::lround(font.GetFractionalPointSize() * 96.0 / 72.0)));
		auto const size = static_cast<float>(pixels);
		if (font_.getTypeface() && family == family_ && weight == weight_
			&& italic == italic_ && size == size_)
			return true;

		auto typeface = aegisub::skia::PlatformFontRuntime::Get().ResolveTypeface({
			family,
			weight,
			SkFontStyle::kNormal_Width,
			italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant,
		});
		if (!typeface) {
			error = "native platform font manager could not resolve the grid font";
			return false;
		}
		font_ = SkFont(std::move(typeface), size);
		font_.setEdging(SkFont::Edging::kAntiAlias);
		font_.setSubpixel(false);
		family_ = std::move(family);
		weight_ = weight;
		italic_ = italic;
		size_ = size;
		ClearLayouts();
		return true;
	}

	bool Measure(
		std::wstring_view text, int& width, int& height, std::string& error) override {
		if (text.empty()) return true;
		auto *entry = GetLayout(text, error);
		if (!entry) return false;
		width = entry->width;
		height = entry->height;
		return true;
	}

	bool Draw(
		SkSurface& surface,
		SkRegion const&,
		std::wstring_view text,
		int x,
		int top,
		agi::Color const& color,
		std::string& error) override {
		if (text.empty()) return true;
		auto *entry = GetLayout(text, error);
		if (!entry) return false;
		AccumulateDuration raster_timing(stats_.raster_ms);
		SkPaint paint;
		paint.setAntiAlias(true);
		paint.setColor(SkColorSetARGB(255 - color.a, color.r, color.g, color.b));
		// SkTextBlobBuilderRunHandler stores the shaped baseline in each glyph
		// position, so the draw origin is the requested top-left without another
		// font-metric baseline offset.
		surface.getCanvas()->drawTextBlob(
			entry->blob, static_cast<float>(x), static_cast<float>(top), paint);
		return true;
	}

	void InvalidateSystemFonts() noexcept override {
		aegisub::skia::PlatformFontRuntime::Get().InvalidateSystemFonts();
		font_ = SkFont();
		family_.clear();
		weight_ = 0;
		italic_ = false;
		size_ = 0.f;
		ClearLayouts();
	}

	void InvalidateRasterPolicy() noexcept override { }

	char const *Name() const noexcept override {
#if defined(__APPLE__)
		return "coretext-skshaper";
#else
		return "fontconfig-freetype-harfbuzz";
#endif
	}

	GridTextBackendStats Stats() const noexcept override {
		auto result = stats_;
		result.layout_entries = layouts_.size();
		result.layout_bytes = layout_bytes_;
		return result;
	}
};

} // namespace

std::unique_ptr<SkiaGridTextBackend> CreateSkiaGridTextBackend(std::string& error) {
	auto backend = std::make_unique<NativeSkShaperGridTextBackend>();
	if (!backend->Available()) {
		error = "the native platform SkShaper backend is unavailable";
		return {};
	}
	LOG_I("subtitle/grid/text") << "backend=" << backend->Name();
	return backend;
}

} // namespace aegisub::grid

#endif
