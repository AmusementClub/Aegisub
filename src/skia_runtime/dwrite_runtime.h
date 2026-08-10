#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <string>

struct IDWriteFactory;
struct IDWriteGdiInterop;

namespace aegisub::font {

enum class DWriteRuntimeMode {
	PreferProvider,
	SystemOnly
};

/// Process-shared DirectWrite provider and factory lifetime.
///
/// SystemOnly is the authority for wx/GDI UI faces and monitor-aware UI text.
/// PreferProvider preserves the existing app-local DWriteCore-first behavior
/// used by the libass font provider.
class DWriteRuntime final {
	IDWriteFactory *factory_ = nullptr;
	IDWriteGdiInterop *gdi_interop_ = nullptr;
	HMODULE module_ = nullptr;
	bool owns_module_ = false;
	bool dwrite_core_ = false;
	std::string description_;

	DWriteRuntime() = default;
	static std::shared_ptr<DWriteRuntime const> Create(bool dwrite_core);

public:
	~DWriteRuntime();

	DWriteRuntime(DWriteRuntime const&) = delete;
	DWriteRuntime& operator=(DWriteRuntime const&) = delete;

	static std::shared_ptr<DWriteRuntime const> Acquire(DWriteRuntimeMode mode);

	bool available() const noexcept { return factory_ && gdi_interop_; }
	bool is_dwrite_core() const noexcept { return dwrite_core_; }
	std::string const& description() const noexcept { return description_; }
	IDWriteFactory *factory() const noexcept { return factory_; }
	IDWriteGdiInterop *gdi_interop() const noexcept { return gdi_interop_; }
};

} // namespace aegisub::font
