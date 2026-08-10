#include "dwrite_runtime.h"

#include <dwrite.h>
#include <winver.h>

#include <string>
#include <string_view>
#include <vector>

#include <libaegisub/log.h>
#include <libaegisub/native_library.h>

namespace aegisub::font {
namespace {

using DWriteCreateFactoryFn = HRESULT (WINAPI *)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

std::wstring Utf8ToWide(std::string_view utf8) {
	if (utf8.empty()) return {};
	auto const length = MultiByteToWideChar(
		CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	if (!MultiByteToWideChar(
			CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length))
		return {};
	return result;
}

std::string WideToUtf8(wchar_t const *text, std::size_t length) {
	if (!text || !length) return {};
	auto const required = WideCharToMultiByte(
		CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
	if (required <= 0) return {};
	std::string result(static_cast<std::size_t>(required), '\0');
	if (!WideCharToMultiByte(
			CP_UTF8, 0, text, static_cast<int>(length), result.data(), required, nullptr, nullptr))
		return {};
	return result;
}

HMODULE TryLoadDWriteCore(DWriteCreateFactoryFn& create_factory) {
	auto const probes = agi::native::BuildLibraryLoadProbes(
		"DWriteCore", agi::native::DefaultAppLocalLoadOptions(false));
	for (auto const& probe : probes) {
		auto const wide_probe = Utf8ToWide(probe);
		if (wide_probe.empty()) continue;
		auto const module = LoadLibraryW(wide_probe.c_str());
		if (!module) continue;
		create_factory = reinterpret_cast<DWriteCreateFactoryFn>(
			GetProcAddress(module, "DWriteCoreCreateFactory"));
		if (create_factory) return module;
		FreeLibrary(module);
	}
	return nullptr;
}

HMODULE TryLoadSystemDWrite(DWriteCreateFactoryFn& create_factory) {
	auto const module = LoadLibraryExW(
		L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!module) return nullptr;
	create_factory = reinterpret_cast<DWriteCreateFactoryFn>(
		GetProcAddress(module, "DWriteCreateFactory"));
	if (create_factory) return module;
	FreeLibrary(module);
	return nullptr;
}

struct SystemDWritePin {
	HMODULE module = nullptr;
	DWriteCreateFactoryFn create_factory = nullptr;

	SystemDWritePin() {
		module = TryLoadSystemDWrite(create_factory);
		// The shared factory can be used during static destruction, so the system
		// module remains pinned for the process lifetime.
	}
};

SystemDWritePin const& SystemPin() {
	static SystemDWritePin const pin;
	return pin;
}

std::string DescribeModule(HMODULE module, bool dwrite_core) {
	std::string result = dwrite_core ? "DWriteCore" : "system DirectWrite";
	wchar_t path[MAX_PATH] = {};
	auto const path_length = GetModuleFileNameW(module, path, MAX_PATH);
	if (!path_length) return result;

	result += " (";
	result += WideToUtf8(path, path_length);
	DWORD ignored = 0;
	auto const version_size = GetFileVersionInfoSizeW(path, &ignored);
	if (version_size) {
		std::vector<char> version_data(version_size);
		if (GetFileVersionInfoW(path, 0, version_size, version_data.data())) {
			VS_FIXEDFILEINFO *info = nullptr;
			UINT info_length = 0;
			if (VerQueryValueW(
					version_data.data(), L"\\", reinterpret_cast<void **>(&info), &info_length)
				&& info) {
				result += ", ";
				result += std::to_string(HIWORD(info->dwFileVersionMS)) + ".";
				result += std::to_string(LOWORD(info->dwFileVersionMS)) + ".";
				result += std::to_string(HIWORD(info->dwFileVersionLS)) + ".";
				result += std::to_string(LOWORD(info->dwFileVersionLS));
			}
		}
	}
	result += ")";
	return result;
}

} // namespace

std::shared_ptr<DWriteRuntime const> DWriteRuntime::Create(bool dwrite_core) {
	auto runtime = std::shared_ptr<DWriteRuntime>(new DWriteRuntime);
	DWriteCreateFactoryFn create_factory = nullptr;
	if (dwrite_core) {
		runtime->module_ = TryLoadDWriteCore(create_factory);
		runtime->owns_module_ = runtime->module_ != nullptr;
		runtime->dwrite_core_ = runtime->module_ != nullptr;
	}
	else {
		auto const& pin = SystemPin();
		runtime->module_ = pin.module;
		create_factory = pin.create_factory;
	}

	if (!runtime->module_ || !create_factory)
		return runtime;

	IDWriteFactory *factory = nullptr;
	auto result = create_factory(
		DWRITE_FACTORY_TYPE_SHARED,
		__uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown **>(&factory));
	if (FAILED(result) || !factory)
		return runtime;

	IDWriteGdiInterop *interop = nullptr;
	result = factory->GetGdiInterop(&interop);
	if (FAILED(result) || !interop) {
		factory->Release();
		return runtime;
	}

	runtime->factory_ = factory;
	runtime->gdi_interop_ = interop;
	runtime->description_ = DescribeModule(runtime->module_, runtime->dwrite_core_);
	LOG_I("font/dwrite") << "Shared DirectWrite runtime initialized: "
		<< runtime->description_;
	return runtime;
}

DWriteRuntime::~DWriteRuntime() {
	if (gdi_interop_) gdi_interop_->Release();
	if (factory_) factory_->Release();
	gdi_interop_ = nullptr;
	factory_ = nullptr;
	if (owns_module_ && module_) FreeLibrary(module_);
	module_ = nullptr;
}

std::shared_ptr<DWriteRuntime const> DWriteRuntime::Acquire(DWriteRuntimeMode mode) {
	static auto const system = Create(false);
	if (mode == DWriteRuntimeMode::SystemOnly)
		return system;

	static auto const preferred = [] {
		auto core = Create(true);
		return core->available() ? core : system;
	}();
	return preferred;
}

} // namespace aegisub::font
