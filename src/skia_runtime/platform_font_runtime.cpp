#include "platform_font_runtime.h"

#if defined(_WIN32)
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <libaegisub/log.h>

namespace aegisub::skia {
namespace {

constexpr std::size_t TypefaceCacheLimit = 256;

struct RequestHash {
	std::size_t operator()(PlatformFontRequest const& request) const noexcept {
		auto hash = std::hash<std::string>{}(request.family);
		hash ^= static_cast<std::size_t>(request.weight) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
		hash ^= static_cast<std::size_t>(request.width) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
		hash ^= static_cast<std::size_t>(request.slant) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
		return hash;
	}
};

std::pair<sk_sp<SkFontMgr>, PlatformFontBackend> CreatePlatformFontManager() {
#if defined(_WIN32)
	if (auto manager = SkFontMgr_New_DirectWrite())
		return {std::move(manager), PlatformFontBackend::DirectWrite};
	if (auto manager = SkFontMgr_New_GDI())
		return {std::move(manager), PlatformFontBackend::Gdi};
#elif defined(__APPLE__)
	if (auto manager = SkFontMgr_New_CoreText(nullptr))
		return {std::move(manager), PlatformFontBackend::CoreText};
#else
	if (auto scanner = SkFontScanner_Make_FreeType()) {
		if (auto manager = SkFontMgr_New_FontConfig(nullptr, std::move(scanner)))
			return {std::move(manager), PlatformFontBackend::FontconfigFreeType};
	}
#endif
	return {SkFontMgr::RefEmpty(), PlatformFontBackend::Empty};
}

} // namespace

struct PlatformFontRuntime::Impl {
	struct CacheEntry {
		sk_sp<SkTypeface> typeface;
		std::list<PlatformFontRequest>::iterator lru;
	};

	sk_sp<SkFontMgr> manager;
	PlatformFontBackend backend = PlatformFontBackend::Empty;
	mutable std::mutex mutex;
	std::unordered_map<PlatformFontRequest, CacheEntry, RequestHash> cache;
	std::list<PlatformFontRequest> lru;
	std::atomic<std::uint64_t> generation {1};
	std::atomic<std::uint64_t> hits {0};
	std::atomic<std::uint64_t> misses {0};
	std::atomic<std::uint64_t> evictions {0};

	Impl() {
		auto created = CreatePlatformFontManager();
		manager = std::move(created.first);
		backend = created.second;
		LOG_I("skia/font") << "Platform Skia font runtime initialized: "
			<< PlatformFontBackendName(backend);
	}
};

PlatformFontRuntime::PlatformFontRuntime()
:	impl_(new Impl) {
}

PlatformFontRuntime::~PlatformFontRuntime() {
	delete impl_;
}

PlatformFontRuntime& PlatformFontRuntime::Get() {
	static PlatformFontRuntime runtime;
	return runtime;
}

PlatformFontBackend PlatformFontRuntime::Backend() const noexcept {
	return impl_->backend;
}

sk_sp<SkFontMgr> PlatformFontRuntime::FontManager() const {
	return sk_ref_sp(impl_->manager.get());
}

sk_sp<SkTypeface> PlatformFontRuntime::ResolveTypeface(PlatformFontRequest const& request) {
	{
		std::lock_guard lock(impl_->mutex);
		auto const found = impl_->cache.find(request);
		if (found != impl_->cache.end()) {
			impl_->lru.splice(impl_->lru.begin(), impl_->lru, found->second.lru);
			impl_->hits.fetch_add(1, std::memory_order_relaxed);
			return found->second.typeface;
		}
	}

	impl_->misses.fetch_add(1, std::memory_order_relaxed);
	SkFontStyle const style(
		std::clamp(request.weight, 1, 1000),
		std::clamp(request.width, 1, 9),
		request.slant);
	auto const *family = request.family.empty() ? nullptr : request.family.c_str();
	auto typeface = impl_->manager->legacyMakeTypeface(family, style);
	if (!typeface && family)
		typeface = impl_->manager->legacyMakeTypeface(nullptr, style);
	if (!typeface && family)
		typeface = impl_->manager->matchFamilyStyle(family, style);

	std::lock_guard lock(impl_->mutex);
	if (auto const found = impl_->cache.find(request); found != impl_->cache.end()) {
		impl_->lru.splice(impl_->lru.begin(), impl_->lru, found->second.lru);
		return found->second.typeface;
	}
	impl_->lru.push_front(request);
	impl_->cache.emplace(request, Impl::CacheEntry{typeface, impl_->lru.begin()});
	while (impl_->cache.size() > TypefaceCacheLimit) {
		auto const& victim = impl_->lru.back();
		impl_->cache.erase(victim);
		impl_->lru.pop_back();
		impl_->evictions.fetch_add(1, std::memory_order_relaxed);
	}
	return typeface;
}

std::uint64_t PlatformFontRuntime::Generation() const noexcept {
	return impl_->generation.load(std::memory_order_acquire);
}

void PlatformFontRuntime::InvalidateSystemFonts() {
	std::lock_guard lock(impl_->mutex);
	impl_->cache.clear();
	impl_->lru.clear();
	impl_->generation.fetch_add(1, std::memory_order_release);
}

PlatformFontCacheStats PlatformFontRuntime::CacheStats() const noexcept {
	PlatformFontCacheStats result;
	result.hits = impl_->hits.load(std::memory_order_relaxed);
	result.misses = impl_->misses.load(std::memory_order_relaxed);
	result.evictions = impl_->evictions.load(std::memory_order_relaxed);
	std::lock_guard lock(impl_->mutex);
	result.entries = impl_->cache.size();
	return result;
}

char const *PlatformFontBackendName(PlatformFontBackend backend) noexcept {
	switch (backend) {
		case PlatformFontBackend::DirectWrite: return "DirectWrite";
		case PlatformFontBackend::Gdi: return "GDI";
		case PlatformFontBackend::CoreText: return "CoreText";
		case PlatformFontBackend::FontconfigFreeType: return "Fontconfig/FreeType";
		case PlatformFontBackend::Empty: return "empty";
	}
	return "unknown";
}

} // namespace aegisub::skia
