#pragma once

#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkRefCnt.h>
#include <include/core/SkTypeface.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace aegisub::skia {

enum class PlatformFontBackend {
	DirectWrite,
	Gdi,
	CoreText,
	FontconfigFreeType,
	Empty
};

struct PlatformFontRequest {
	std::string family;
	int weight = SkFontStyle::kNormal_Weight;
	int width = SkFontStyle::kNormal_Width;
	SkFontStyle::Slant slant = SkFontStyle::kUpright_Slant;

	friend bool operator==(PlatformFontRequest const&, PlatformFontRequest const&) = default;
};

struct PlatformFontCacheStats {
	std::uint64_t hits = 0;
	std::uint64_t misses = 0;
	std::uint64_t evictions = 0;
	std::size_t entries = 0;
};

/// Shared platform font manager and bounded typeface cache for Skia consumers.
class PlatformFontRuntime final {
	struct Impl;
	Impl *impl_ = nullptr;

	PlatformFontRuntime();

public:
	~PlatformFontRuntime();

	PlatformFontRuntime(PlatformFontRuntime const&) = delete;
	PlatformFontRuntime& operator=(PlatformFontRuntime const&) = delete;

	static PlatformFontRuntime& Get();

	PlatformFontBackend Backend() const noexcept;
	sk_sp<SkFontMgr> FontManager() const;
	sk_sp<SkTypeface> ResolveTypeface(PlatformFontRequest const& request);
	std::uint64_t Generation() const noexcept;
	void InvalidateSystemFonts();
	PlatformFontCacheStats CacheStats() const noexcept;
};

char const *PlatformFontBackendName(PlatformFontBackend backend) noexcept;

} // namespace aegisub::skia
