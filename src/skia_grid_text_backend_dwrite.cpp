#ifdef _WIN32

#include "skia_grid_text_backend.h"

#include "font_file_lister_dwrite.h"
#include "grid_core/windows_text_raster_policy.h"
#include "perf_trace.h"
#include "skia_runtime/dwrite_runtime.h"

#include <include/core/SkRegion.h>
#include <include/core/SkSurface.h>

#include <libaegisub/color.h>
#include <libaegisub/log.h>

#include <wx/font.h>
#include <wx/string.h>

#include <dwrite.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <list>
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

template<class T>
struct ComRelease {
	void operator()(T *value) const noexcept {
		if (value) value->Release();
	}
};

template<class T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

std::wstring ToWide(wxString const& text) {
	return std::wstring(text.wc_str(), text.length());
}

RemoteSessionSignals ReadRemoteSessionSignals() noexcept {
	RemoteSessionSignals signals;
	signals.sm_remote_session = GetSystemMetrics(SM_REMOTESESSION) != 0;

	DWORD process_session = 0;
	if (ProcessIdToSessionId(GetCurrentProcessId(), &process_session))
		signals.process_session_id = process_session;

	DWORD glass_session = 0;
	DWORD size = sizeof(glass_session);
	auto const result = RegGetValueW(
		HKEY_LOCAL_MACHINE,
		L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
		L"GlassSessionId",
		RRF_RT_REG_DWORD,
		nullptr,
		&glass_session,
		&size);
	if (result == ERROR_SUCCESS)
		signals.glass_session_id = glass_session;
	return signals;
}

char const *RemoteStateName(RemoteSessionState state) noexcept {
	switch (state) {
		case RemoteSessionState::Local: return "local";
		case RemoteSessionState::Remote: return "remote";
		case RemoteSessionState::Unknown: return "unknown";
	}
	return "unknown";
}

TextPixelGeometry ToPixelGeometry(DWRITE_PIXEL_GEOMETRY geometry) noexcept {
	switch (geometry) {
		case DWRITE_PIXEL_GEOMETRY_RGB: return TextPixelGeometry::Rgb;
		case DWRITE_PIXEL_GEOMETRY_BGR: return TextPixelGeometry::Bgr;
		case DWRITE_PIXEL_GEOMETRY_FLAT: return TextPixelGeometry::FlatOrUnknown;
	}
	return TextPixelGeometry::FlatOrUnknown;
}

struct RasterPolicySnapshot {
	RemoteSessionState remote = RemoteSessionState::Unknown;
	TextPixelGeometry geometry = TextPixelGeometry::FlatOrUnknown;
	WindowsTextAntialiasPolicy policy = WindowsTextAntialiasPolicy::Grayscale;
	ComPtr<IDWriteRenderingParams> rendering_params;
};

bool SystemClearTypeEnabled() noexcept {
	BOOL smoothing_enabled = FALSE;
	UINT smoothing_type = 0;
	return SystemParametersInfoW(
		SPI_GETFONTSMOOTHING, 0, &smoothing_enabled, 0)
		&& smoothing_enabled
		&& SystemParametersInfoW(
			SPI_GETFONTSMOOTHINGTYPE, 0, &smoothing_type, 0)
		&& smoothing_type == FE_FONTSMOOTHINGCLEARTYPE;
}

RasterPolicySnapshot ReadRasterPolicy(
	IDWriteFactory *factory,
	SkiaGridTextRenderTarget const& target) noexcept {
	RasterPolicySnapshot snapshot;
	snapshot.remote = ClassifyRemoteSession(ReadRemoteSessionSignals());
	if (!factory)
		return snapshot;

	IDWriteRenderingParams *raw_source = nullptr;
	bool monitor_params_valid = false;
	if (target.monitor_token) {
		auto const monitor = reinterpret_cast<HMONITOR>(target.monitor_token);
		monitor_params_valid = SUCCEEDED(
			factory->CreateMonitorRenderingParams(monitor, &raw_source))
			&& raw_source;
	}
	if (!raw_source
		&& (FAILED(factory->CreateRenderingParams(&raw_source)) || !raw_source))
		return snapshot;
	ComPtr<IDWriteRenderingParams> source(raw_source);

	snapshot.geometry = ToPixelGeometry(source->GetPixelGeometry());
	auto const clear_type_enabled = monitor_params_valid
		&& SystemClearTypeEnabled()
		&& source->GetClearTypeLevel() > 0.f;
	snapshot.policy = SelectWindowsTextAntialiasPolicy({
		snapshot.remote,
		snapshot.geometry,
		target.opaque_target,
		target.one_to_one_present,
		clear_type_enabled,
		target.clear_type_requested,
	});

	auto const geometry = snapshot.policy == WindowsTextAntialiasPolicy::ClearType
		? source->GetPixelGeometry() : DWRITE_PIXEL_GEOMETRY_FLAT;
	auto const clear_type_level = snapshot.policy == WindowsTextAntialiasPolicy::ClearType
		? source->GetClearTypeLevel() : 0.f;
	auto const rendering_mode = snapshot.policy == WindowsTextAntialiasPolicy::ClearType
		? DWRITE_RENDERING_MODE_NATURAL
		: DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
	IDWriteRenderingParams *raw_params = nullptr;
	if (SUCCEEDED(factory->CreateCustomRenderingParams(
		source->GetGamma(),
		source->GetEnhancedContrast(),
		clear_type_level,
		geometry,
		rendering_mode,
		&raw_params)) && raw_params) {
		snapshot.rendering_params.reset(raw_params);
	}
	return snapshot;
}

struct RasterContext {
	IDWriteBitmapRenderTarget *target = nullptr;
	IDWriteRenderingParams *rendering_params = nullptr;
	COLORREF color = RGB(0, 0, 0);
	std::string error;
};

bool RasterGlyphRun(
	RasterContext& context,
	float baseline_x,
	float baseline_y,
	DWRITE_MEASURING_MODE measuring_mode,
	DWRITE_GLYPH_RUN const& glyph_run) {
	if (!context.target || !context.rendering_params) {
		context.error = "DirectWrite grid raster target is unavailable";
		return false;
	}
	auto const result = context.target->DrawGlyphRun(
		baseline_x,
		baseline_y,
		measuring_mode,
		&glyph_run,
		context.rendering_params,
		context.color);
	if (FAILED(result)) {
		context.error = "DirectWrite native glyph-run raster failed";
		return false;
	}
	return true;
}

class DWriteRasterRenderer final : public IDWriteTextRenderer {
	ULONG references_ = 1;

public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
		if (!object) return E_POINTER;
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer))
			*object = static_cast<IDWriteTextRenderer *>(this);
		else if (iid == __uuidof(IDWritePixelSnapping))
			*object = static_cast<IDWritePixelSnapping *>(this);
		else {
			*object = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
	ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&references_); }

	HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *disabled) override {
		if (!disabled) return E_POINTER;
		*disabled = FALSE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *transform) override {
		if (!transform) return E_POINTER;
		*transform = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *pixels_per_dip) override {
		if (!pixels_per_dip) return E_POINTER;
		*pixels_per_dip = 1.f;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawGlyphRun(
		void *drawing_context,
		FLOAT baseline_x,
		FLOAT baseline_y,
		DWRITE_MEASURING_MODE measuring_mode,
		DWRITE_GLYPH_RUN const *glyph_run,
		DWRITE_GLYPH_RUN_DESCRIPTION const *,
		IUnknown *) override {
		if (!drawing_context || !glyph_run || !glyph_run->fontFace)
			return E_INVALIDARG;
		auto& context = *static_cast<RasterContext *>(drawing_context);
		return RasterGlyphRun(context, baseline_x, baseline_y, measuring_mode, *glyph_run)
			? S_OK : E_FAIL;
	}
	HRESULT STDMETHODCALLTYPE DrawUnderline(
		void *, FLOAT, FLOAT, DWRITE_UNDERLINE const *, IUnknown *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawStrikethrough(
		void *, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const *, IUnknown *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawInlineObject(
		void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL, IUnknown *) override { return S_OK; }
};

class WindowsDWriteGridTextBackend final : public SkiaGridTextBackend {
	struct LayoutEntry {
		ComPtr<IDWriteTextLayout> layout;
		DWRITE_TEXT_METRICS metrics {};
		std::size_t bytes = 0;
		std::list<std::wstring>::iterator lru;
	};

	std::shared_ptr<aegisub::font::DWriteRuntime const> runtime_;
	DWriteBridge bridge_ {DWriteBridgeMode::SystemOnly};
	ComPtr<IDWriteTextFormat> format_;
	std::string requested_family_;
	int requested_weight_ = 0;
	bool requested_italic_ = false;
	float requested_size_ = 0.f;
	std::wstring font_family_;
	int font_weight_ = 0;
	bool italic_ = false;
	float font_size_ = 0.f;
	RasterPolicySnapshot raster_policy_;
	SkiaGridTextRenderTarget render_target_;
	bool raster_policy_dirty_ = true;
	std::unordered_map<std::wstring, LayoutEntry> layouts_;
	std::list<std::wstring> lru_;
	std::size_t layout_bytes_ = 0;
	GridTextBackendStats stats_;

	void ClearLayouts() noexcept {
		layouts_.clear();
		lru_.clear();
		layout_bytes_ = 0;
	}

	bool UpdateRasterPolicy(
		SkiaGridTextRenderTarget const& target, std::string& error) {
		if (!target.platform_raster_target) {
			error = "DirectWrite grid bitmap target is unavailable";
			return false;
		}
		auto const conditions_changed = target.monitor_token != render_target_.monitor_token
			|| target.opaque_target != render_target_.opaque_target
			|| target.one_to_one_present != render_target_.one_to_one_present
			|| target.clear_type_requested != render_target_.clear_type_requested;
		render_target_ = target;
		if (!raster_policy_dirty_ && !conditions_changed
			&& raster_policy_.rendering_params)
			return true;

		auto updated = ReadRasterPolicy(
			runtime_ ? runtime_->factory() : nullptr, target);
		if (!updated.rendering_params) {
			error = "DirectWrite grid rendering parameters are unavailable";
			return false;
		}
		auto const changed = updated.remote != raster_policy_.remote
			|| updated.geometry != raster_policy_.geometry
			|| updated.policy != raster_policy_.policy
			|| conditions_changed;
		raster_policy_ = std::move(updated);
		raster_policy_dirty_ = false;
		if (changed) {
			LOG_I("subtitle/grid/dwrite")
				<< "raster policy changed remote=" << RemoteStateName(raster_policy_.remote)
				<< " requested=" << (target.clear_type_requested ? "cleartype" : "grayscale")
				<< " antialias=" << (raster_policy_.policy == WindowsTextAntialiasPolicy::ClearType
					? "cleartype" : "grayscale")
				<< " rendering_mode=" << (raster_policy_.policy == WindowsTextAntialiasPolicy::ClearType
					? "natural" : "natural-symmetric");
		}
		return true;
	}

	LayoutEntry *GetLayout(std::wstring_view text, std::string& error) {
		std::wstring key(text);
		if (auto found = layouts_.find(key); found != layouts_.end()) {
			lru_.splice(lru_.begin(), lru_, found->second.lru);
			++stats_.layout_hits;
			return &found->second;
		}
		++stats_.layout_misses;
		AccumulateDuration layout_timing(stats_.layout_ms);
		if (!format_) {
			error = "DirectWrite grid font is not initialized";
			return nullptr;
		}
		if (text.size() > MaxLayoutTextUnits) {
			error = "subtitle grid text is too long for DirectWrite";
			return nullptr;
		}

		IDWriteTextLayout *raw_layout = nullptr;
		auto const result = runtime_->factory()->CreateTextLayout(
			text.data(),
			static_cast<UINT32>(text.size()),
			format_.get(),
			1048576.f,
			65536.f,
			&raw_layout);
		if (FAILED(result) || !raw_layout) {
			error = "DirectWrite text layout creation failed";
			return nullptr;
		}
		ComPtr<IDWriteTextLayout> layout(raw_layout);
		DWRITE_TEXT_METRICS metrics {};
		if (FAILED(layout->GetMetrics(&metrics))) {
			error = "DirectWrite text metrics failed";
			return nullptr;
		}

		// The map key and LRU list each own a string object and its character
		// storage. Native IDWriteTextLayout internals are intentionally still
		// represented by the bounded estimate below rather than guessed exactly.
		auto const key_storage_bytes = key.size() * sizeof(wchar_t);
		auto const entry_bytes = sizeof(LayoutEntry) + 2u * sizeof(std::wstring)
			+ 2u * key_storage_bytes + 256u;
		if (entry_bytes > LayoutByteLimit) {
			error = "DirectWrite subtitle grid layout exceeds the cache budget";
			return nullptr;
		}
		lru_.push_front(key);
		auto inserted = layouts_.emplace(std::move(key), LayoutEntry{
			std::move(layout), metrics, entry_bytes, lru_.begin()});
		layout_bytes_ += entry_bytes;
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
	WindowsDWriteGridTextBackend()
	: runtime_(aegisub::font::DWriteRuntime::Acquire(
		aegisub::font::DWriteRuntimeMode::SystemOnly)) {
		if (runtime_ && runtime_->available()) {
			LOG_I("subtitle/grid/dwrite")
				<< "provider=" << runtime_->description();
		}
	}

	bool Available() const noexcept {
		return runtime_ && runtime_->available() && bridge_.available();
	}

	bool SetRenderTarget(
		SkiaGridTextRenderTarget const& target, std::string& error) override {
		return UpdateRasterPolicy(target, error);
	}

	bool SetFont(wxFont const& font, std::string& error) override {
		if (!Available()) {
			error = "system DirectWrite or GDI interop is unavailable";
			return false;
		}
		auto family_utf8 = font.GetFaceName().utf8_string();
		auto weight = std::clamp(font.GetNumericWeight(), 1, 999);
		auto italic = font.GetStyle() != wxFONTSTYLE_NORMAL;
		auto pixel_size = font.GetPixelSize().GetHeight();
		if (pixel_size <= 0)
			pixel_size = std::max(1, static_cast<int>(std::lround(font.GetFractionalPointSize() * 96.0 / 72.0)));
		auto const size = static_cast<float>(pixel_size);
		if (format_ && family_utf8 == requested_family_ && weight == requested_weight_
			&& italic == requested_italic_ && size == requested_size_)
			return true;

		auto const request_family = family_utf8;
		auto const request_weight = weight;
		auto const request_italic = italic;
		auto const resolved = bridge_.ResolveTextFormatFaceFromGdiFace(
			family_utf8, weight, italic);
		auto const requested_weight_for_log = weight;
		if (resolved.ok) {
			family_utf8 = resolved.family;
			if (resolved.weight > 0) weight = resolved.weight;
			italic = resolved.italic;
		}
		if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
			LOG_I("subtitle/grid/dwrite_font")
				<< "requested_family=" << request_family
				<< " resolved_family=" << family_utf8
				<< " requested_weight=" << requested_weight_for_log
				<< " resolved_weight=" << weight
				<< " italic=" << (italic ? 1 : 0)
				<< " wx_pixel_height=" << pixel_size
				<< " dwrite_size_dip=" << size;
		}
		auto family = ToWide(wxString::FromUTF8(family_utf8));
		if (family.empty())
			family = L"Segoe UI";

		IDWriteTextFormat *raw_format = nullptr;
		auto const result = runtime_->factory()->CreateTextFormat(
			family.c_str(),
			nullptr,
			static_cast<DWRITE_FONT_WEIGHT>(weight),
			italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL,
			size,
			L"",
			&raw_format);
		if (FAILED(result) || !raw_format) {
			error = "DirectWrite text format creation failed";
			return false;
		}
		ComPtr<IDWriteTextFormat> format(raw_format);
		if (FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))
			|| FAILED(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING))
			|| FAILED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR))) {
			error = "DirectWrite text format configuration failed";
			return false;
		}

		format_ = std::move(format);
		requested_family_ = request_family;
		requested_weight_ = request_weight;
		requested_italic_ = request_italic;
		requested_size_ = size;
		font_family_ = std::move(family);
		font_weight_ = weight;
		italic_ = italic;
		font_size_ = size;
		ClearLayouts();
		return true;
	}

	bool Measure(
		std::wstring_view text, int& width, int& height, std::string& error) override {
		if (text.empty()) return true;
		auto *entry = GetLayout(text, error);
		if (!entry) return false;
		width = std::max(0, static_cast<int>(std::ceil(entry->metrics.widthIncludingTrailingWhitespace)));
		height = std::max(0, static_cast<int>(std::ceil(entry->metrics.height)));
		return true;
	}

	bool Draw(
		SkSurface& surface,
		SkRegion const& clip,
		std::wstring_view text,
		int x,
		int top,
		agi::Color const& color,
		std::string& error) override {
		(void)clip;
		if (text.empty()) return true;
		auto *entry = GetLayout(text, error);
		if (!entry) return false;
		if (color.a != 0) {
			error = "DirectWrite grid text requires an opaque foreground color";
			return false;
		}
		auto *target = static_cast<IDWriteBitmapRenderTarget *>(
			render_target_.platform_raster_target);
		auto const dc = target ? target->GetMemoryDC() : nullptr;
		if (!target || !dc) {
			error = "DirectWrite grid raster DC is unavailable";
			return false;
		}
		AccumulateDuration raster_timing(stats_.raster_ms);
		surface.notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
		RasterContext context {
			target,
			raster_policy_.rendering_params.get(),
			RGB(color.r, color.g, color.b),
			{}};
		DWriteRasterRenderer renderer;
		auto const result = entry->layout->Draw(
			&context, &renderer, static_cast<float>(x), static_cast<float>(top));
		if (FAILED(result)) {
			error = context.error.empty() ? "DirectWrite text layout rendering failed" : context.error;
			return false;
		}
		return true;
	}

	void InvalidateSystemFonts() noexcept override {
		format_.reset();
		requested_family_.clear();
		requested_weight_ = 0;
		requested_italic_ = false;
		requested_size_ = 0.f;
		font_family_.clear();
		font_weight_ = 0;
		italic_ = false;
		font_size_ = 0.f;
		ClearLayouts();
	}

	void InvalidateRasterPolicy() noexcept override {
		raster_policy_dirty_ = true;
	}

	char const *Name() const noexcept override { return "system-directwrite"; }

	GridTextBackendStats Stats() const noexcept override {
		auto result = stats_;
		result.layout_entries = layouts_.size();
		result.layout_bytes = layout_bytes_;
		result.raster_policy_known = raster_policy_.rendering_params != nullptr;
		result.clear_type_active = result.raster_policy_known
			&& raster_policy_.policy == WindowsTextAntialiasPolicy::ClearType;
		return result;
	}
};

} // namespace

std::unique_ptr<SkiaGridTextBackend> CreateSkiaGridTextBackend(std::string& error) {
	auto backend = std::make_unique<WindowsDWriteGridTextBackend>();
	if (!backend->Available()) {
		error = "the Windows subtitle grid requires system DirectWrite";
		return {};
	}
	return backend;
}

} // namespace aegisub::grid

#endif
