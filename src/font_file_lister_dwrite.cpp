// Copyright (c) 2026, MIRIMIRIM

#include "font_file_lister_dwrite.h"

#include "font_file_lister.h"
#include "font_matching_libass.h"

#include <dwrite.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <winver.h>

#include <libaegisub/log.h>
#include <libaegisub/native_library.h>
#include <libaegisub/scope_exit.h>

namespace {
using DWriteCreateFactoryFn = HRESULT (WINAPI *)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

uint16_t read_big_endian_16(uint8_t const *data) {
	return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

int libass_weight_from_os2(uint16_t weight, bool bold) {
	switch (weight) {
		case 0: return bold ? 700 : 400;
		case 1: return 100;
		case 2: return 200;
		case 3: return 300;
		case 4: return 350;
		case 5: return 400;
		case 6: return 600;
		case 7: return 700;
		case 8: return 800;
		case 9: return 900;
		default: return weight;
	}
}

void read_libass_attributes(IDWriteFontFace *face, IDWriteFont *font, FontMatchCandidate& metadata) {
	metadata.weight = std::clamp(static_cast<int>(font->GetWeight()), 1, 999);
	metadata.bold = font->GetWeight() >= DWRITE_FONT_WEIGHT_BOLD;
	metadata.italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL;

	void const *table_data = nullptr;
	UINT32 table_size = 0;
	void *table_context = nullptr;
	BOOL exists = FALSE;
	if (SUCCEEDED(face->TryGetFontTable(
		DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'),
		&table_data,
		&table_size,
		&table_context,
		&exists)) && exists && table_data && table_size >= 64) {
		auto const *os2 = static_cast<uint8_t const *>(table_data);
		auto const selection = read_big_endian_16(os2 + 62);
		metadata.bold = (selection & (1u << 5)) != 0;
		metadata.italic = (selection & 1u) != 0;
		metadata.weight = libass_weight_from_os2(read_big_endian_16(os2 + 4), metadata.bold);
	}
	if (table_context)
		face->ReleaseFontTable(table_context);
}

std::wstring utf8_to_wide(std::string_view utf8) {
	if (utf8.empty()) return {};
	auto len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
	                               nullptr, 0);
	if (len <= 0) return {};
	std::wstring result(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
	                    &result[0], len);
	return result;
}

HMODULE try_load_dwrite_core(DWriteCreateFactoryFn &out_create_fn) {
	// DWriteCore should not be picked up from system paths.
	auto probes = agi::native::BuildLibraryLoadProbes("DWriteCore",
		agi::native::DefaultAppLocalLoadOptions(false));
	for (auto const& probe : probes) {
		auto wide_probe = utf8_to_wide(probe);
		if (wide_probe.empty()) continue;
		auto mod = LoadLibraryW(wide_probe.c_str());
		if (!mod) continue;

		out_create_fn = reinterpret_cast<DWriteCreateFactoryFn>(
			GetProcAddress(mod, "DWriteCoreCreateFactory"));
		if (out_create_fn) return mod;
		FreeLibrary(mod);
	}
	return nullptr;
}

HMODULE try_load_system_dwrite(DWriteCreateFactoryFn &out_create_fn) {
	// System DWrite must come from System32 to avoid DLL hijacking.
	auto mod = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!mod)
		return nullptr;

	out_create_fn = reinterpret_cast<DWriteCreateFactoryFn>(
		GetProcAddress(mod, "DWriteCreateFactory"));
	if (!out_create_fn) {
		FreeLibrary(mod);
		return nullptr;
	}
	return mod;
}

/// Process-lifetime pin for system dwrite.dll.
///
/// Bridges borrow this module handle and must not FreeLibrary it. Per-instance
/// LoadLibrary without a matching FreeLibrary used to leak a refcount on every
/// GdiFontResolver construction; a single pin keeps SHARED factories valid for
/// concurrent catalog builds without unbounded ref growth.
struct SystemDWritePin {
	HMODULE mod = nullptr;
	DWriteCreateFactoryFn create_fn = nullptr;

	SystemDWritePin() {
		mod = try_load_system_dwrite(create_fn);
		// Intentionally never FreeLibrary(mod): pin for process lifetime.
	}
};

SystemDWritePin const& system_dwrite_pin() {
	static SystemDWritePin const pin;
	return pin;
}

bool init_dwrite_factory(DWriteCreateFactoryFn create_fn, IDWriteFactory **out_factory,
                          IDWriteGdiInterop **out_interop) {
	IDWriteFactory *factory = nullptr;
	auto hr = create_fn(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
	                    reinterpret_cast<IUnknown **>(&factory));
	if (FAILED(hr) || !factory)
		return false;

	IDWriteGdiInterop *interop = nullptr;
	hr = factory->GetGdiInterop(&interop);
	if (FAILED(hr) || !interop) {
		factory->Release();
		return false;
	}

	*out_factory = factory;
	*out_interop = interop;
	return true;
}

std::string wide_to_utf8(wchar_t const *wstr, UINT32 len) {
	if (!wstr || !len) return {};
	auto req = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(len),
	                                nullptr, 0, nullptr, nullptr);
	if (req <= 0) return {};
	std::string result(static_cast<size_t>(req), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(len),
	                    result.data(), req, nullptr, nullptr);
	// Strip the trailing null that WideCharToMultiByte always appends.
	if (!result.empty() && result.back() == '\0')
		result.pop_back();
	return result;
}

void add_unique(std::vector<std::string>& values, std::string value) {
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(std::move(value));
}

std::vector<std::string> localized_strings_to_utf8(IDWriteLocalizedStrings *strings) {
	std::vector<std::string> values;
	if (!strings)
		return values;

	auto count = strings->GetCount();
	values.reserve(count);
	for (UINT32 i = 0; i < count; ++i) {
		UINT32 len = 0;
		if (FAILED(strings->GetStringLength(i, &len)) || !len)
			continue;

		std::vector<wchar_t> text(len + 1);
		if (FAILED(strings->GetString(i, text.data(), len + 1)))
			continue;

		add_unique(values, wide_to_utf8(text.data(), len));
	}
	return values;
}

std::vector<DWriteLocalizedName> localized_strings_with_locale(IDWriteLocalizedStrings *strings) {
	std::vector<DWriteLocalizedName> values;
	if (!strings)
		return values;

	auto count = strings->GetCount();
	values.reserve(count);
	for (UINT32 i = 0; i < count; ++i) {
		UINT32 len = 0;
		if (FAILED(strings->GetStringLength(i, &len)) || !len)
			continue;

		std::vector<wchar_t> text(len + 1);
		if (FAILED(strings->GetString(i, text.data(), len + 1)))
			continue;

		std::string locale;
		UINT32 locale_len = 0;
		if (SUCCEEDED(strings->GetLocaleNameLength(i, &locale_len)) && locale_len) {
			std::vector<wchar_t> locale_buf(locale_len + 1);
			if (SUCCEEDED(strings->GetLocaleName(i, locale_buf.data(), locale_len + 1)))
				locale = wide_to_utf8(locale_buf.data(), locale_len);
		}

		DWriteLocalizedName entry;
		entry.value = wide_to_utf8(text.data(), len);
		entry.locale = std::move(locale);
		if (entry.value.empty())
			continue;

		bool duplicate = false;
		for (auto const& existing : values) {
			if (existing.value == entry.value && existing.locale == entry.locale) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			values.push_back(std::move(entry));
	}
	return values;
}

std::vector<std::string> informational_strings(
	IDWriteFont *font,
	DWRITE_INFORMATIONAL_STRING_ID id) {
	if (!font)
		return {};
	IDWriteLocalizedStrings *localized = nullptr;
	BOOL exists = FALSE;
	if (FAILED(font->GetInformationalStrings(id, &localized, &exists)) || !exists || !localized)
		return {};
	auto values = localized_strings_to_utf8(localized);
	localized->Release();
	return values;
}

std::vector<DWriteLocalizedName> win32_family_names(IDWriteFont *font) {
	if (!font)
		return {};

	IDWriteLocalizedStrings *localized = nullptr;
	BOOL exists = FALSE;
	auto hr = font->GetInformationalStrings(
		DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &localized, &exists);
	if (SUCCEEDED(hr) && exists && localized) {
		auto names = localized_strings_with_locale(localized);
		localized->Release();
		if (!names.empty())
			return names;
		localized = nullptr;
	}
	if (localized)
		localized->Release();

	IDWriteFontFamily *family = nullptr;
	if (FAILED(font->GetFontFamily(&family)) || !family)
		return {};
	std::vector<DWriteLocalizedName> names;
	if (SUCCEEDED(family->GetFamilyNames(&localized)) && localized)
		names = localized_strings_with_locale(localized);
	if (localized)
		localized->Release();
	family->Release();
	return names;
}

// Helper: get the first font file and its reference key from a font face.
// On success, caller must Release() the returned file.
bool get_font_file_and_key(IDWriteFontFace *face, IDWriteFontFile **out_file,
                            const void **out_key, UINT32 *out_key_size) {
	UINT32 n_files = 1;
	IDWriteFontFile *file = nullptr;
	auto hr = face->GetFiles(&n_files, &file);
	if (FAILED(hr) || !file)
		return false;

	const void *key = nullptr;
	UINT32 key_size = 0;
	hr = file->GetReferenceKey(&key, &key_size);
	if (FAILED(hr)) {
		file->Release();
		return false;
	}

	*out_file = file;
	*out_key = key;
	*out_key_size = key_size;
	return true;
}

std::string get_dll_description(HMODULE dll, bool is_dwritecore) {
	std::string result = is_dwritecore ? "DWriteCore" : "system DWrite";

	wchar_t dll_path[MAX_PATH] = {};
	if (GetModuleFileNameW(dll, dll_path, MAX_PATH)) {
		result += " (";
		result += wide_to_utf8(dll_path, static_cast<UINT32>(wcslen(dll_path)));
		DWORD handle = 0;
		auto size = GetFileVersionInfoSizeW(dll_path, &handle);
		if (size) {
			std::vector<char> ver_data(size);
			if (GetFileVersionInfoW(dll_path, 0, size, ver_data.data())) {
				VS_FIXEDFILEINFO *info = nullptr;
				UINT info_len = 0;
				if (VerQueryValueW(ver_data.data(), L"\\", reinterpret_cast<void **>(&info), &info_len) && info) {
					result += ", ";
					result += std::to_string(HIWORD(info->dwFileVersionMS)) + ".";
					result += std::to_string(LOWORD(info->dwFileVersionMS)) + ".";
					result += std::to_string(HIWORD(info->dwFileVersionLS)) + ".";
					result += std::to_string(LOWORD(info->dwFileVersionLS));
				}
			}
		}
		result += ")";
	}
	return result;
}

} // anonymous namespace

DWriteBridge::DWriteBridge(DWriteBridgeMode mode) {
	DWriteCreateFactoryFn create_fn = nullptr;
	// True only for app-local DWriteCore loads owned by this instance.
	bool owns_module = false;

	if (mode != DWriteBridgeMode::SystemOnly) {
		dll_handle = try_load_dwrite_core(create_fn);
		if (dll_handle) {
			is_dwritecore_ = true;
			owns_module = true;
		}
	}

	if (!dll_handle) {
		// Borrow the process pin; do not LoadLibrary per bridge instance.
		auto const& pin = system_dwrite_pin();
		dll_handle = pin.mod;
		create_fn = pin.create_fn;
		is_dwritecore_ = false;
		owns_module = false;
	}

	if (!dll_handle || !create_fn) {
		LOG_D("font/dwrite") << "DWrite bridge unavailable: failed to load DWrite DLL";
		dll_handle = nullptr;
		return;
	}

	if (!init_dwrite_factory(create_fn, &factory, &gdi_interop)) {
		LOG_D("font/dwrite") << "DWrite bridge unavailable: failed to create factory or GDI interop";
		if (gdi_interop) { gdi_interop->Release(); gdi_interop = nullptr; }
		if (factory) { factory->Release(); factory = nullptr; }
		if (owns_module && dll_handle) {
			FreeLibrary(dll_handle);
		}
		dll_handle = nullptr;
		return;
	}

	// Stash ownership in is_dwritecore_ for the destructor: only Core loads are
	// instance-owned. System pin is never freed here.
	available_ = true;
	dll_description_ = get_dll_description(dll_handle, is_dwritecore_);
	LOG_I("font/dwrite") << "DWrite bridge initialized: " << dll_description_;
}

DWriteBridge::~DWriteBridge() {
	// Drop COM first so no interface outlives the DLL mapping we still hold.
	if (gdi_interop) {
		gdi_interop->Release();
		gdi_interop = nullptr;
	}
	if (factory) {
		factory->Release();
		factory = nullptr;
	}
	available_ = false;

	// Only FreeLibrary instance-owned DWriteCore modules. System dwrite.dll is
	// held by system_dwrite_pin() for process lifetime so concurrent catalog
	// builds cannot unload it under another bridge's CreateFontFaceFromHdc.
	if (dll_handle && is_dwritecore_)
		FreeLibrary(dll_handle);
	dll_handle = nullptr;
}

IDWriteFontFace *DWriteBridge::CreateFontFaceFromHdc(HDC hdc) const {
	// Defensive: never call through a null or half-torn interop pointer.
	// Callers (GdiFontResolver) already select an HFONT into hdc; reject empty
	// DCs early rather than letting DWrite AV inside CreateFontFaceFromHdc.
	if (!available_ || !gdi_interop || !hdc)
		return nullptr;

	IDWriteFontFace *face = nullptr;
	auto hr = gdi_interop->CreateFontFaceFromHdc(hdc, &face);
	if (FAILED(hr) || !face)
		return nullptr;
	return face;
}

static bool ascii_case_insensitive_equals(std::string_view left, std::string_view right) {
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index) {
		auto const left_char = static_cast<unsigned char>(left[index]);
		auto const right_char = static_cast<unsigned char>(right[index]);
		auto const normalized_left = left_char < 0x80 ? static_cast<unsigned char>(std::tolower(left_char)) : left_char;
		auto const normalized_right = right_char < 0x80 ? static_cast<unsigned char>(std::tolower(right_char)) : right_char;
		if (normalized_left != normalized_right)
			return false;
	}
	return true;
}

IDWriteFontFace *DWriteBridge::CreateFontFaceFromFont(IDWriteFont *font) const {
	if (!available_ || !font)
		return nullptr;

	IDWriteFontFace *face = nullptr;
	if (FAILED(font->CreateFontFace(&face)) || !face)
		return nullptr;
	return face;
}

bool DWriteBridge::BuildFontCatalog(
	std::vector<FontMatchCandidate>& faces,
	std::vector<IDWriteFontFace *>& dwrite_faces,
	std::vector<std::string> const& additional_font_files,
	bool include_system_fonts,
	std::string& error) const {
	error.clear();
	if (!available_ || !factory)
		return false;
	IDWriteFontCollection *collection = nullptr;
	IDWriteFactory5 *factory5 = nullptr;
	IDWriteFontSetBuilder1 *builder = nullptr;
	IDWriteFontSet *font_set = nullptr;
	IDWriteFontCollection1 *custom_collection = nullptr;
	bool custom_catalog = !additional_font_files.empty() || !include_system_fonts;
	bool added_private_file = false;
	bool catalog_ready = false;
	if (!additional_font_files.empty() || !include_system_fonts) {
		if (FAILED(factory->QueryInterface(&factory5)) || !factory5 ||
		    FAILED(factory5->CreateFontSetBuilder(&builder)) || !builder) {
			error = "DirectWrite custom font-set API is unavailable";
			goto cleanup;
		}

		if (include_system_fonts) {
			IDWriteFontSet *system_set = nullptr;
			if (SUCCEEDED(factory5->GetSystemFontSet(&system_set)) && system_set) {
				if (FAILED(builder->AddFontSet(system_set)) && error.empty())
					error = "failed to add the system DirectWrite font set";
				system_set->Release();
			}
			else if (error.empty()) {
				error = "failed to read the system DirectWrite font set";
			}
		}

		for (auto const& path : additional_font_files) {
			auto wide_path = utf8_to_wide(path);
			if (wide_path.empty()) {
				if (error.empty())
					error = "an additional font path is not valid UTF-8";
				continue;
			}
			IDWriteFontFile *file = nullptr;
			if (SUCCEEDED(factory->CreateFontFileReference(wide_path.c_str(), nullptr, &file)) && file) {
				if (SUCCEEDED(builder->AddFontFile(file)))
					added_private_file = true;
				else if (error.empty())
					error = "failed to add an additional font file to DirectWrite: " + path;
				file->Release();
			}
			else if (error.empty()) {
				error = "failed to open an additional font file in DirectWrite: " + path;
			}
		}

		if (FAILED(builder->CreateFontSet(&font_set)) || !font_set ||
		    FAILED(factory5->CreateFontCollectionFromFontSet(font_set, &custom_collection)) ||
		    !custom_collection) {
			if (error.empty())
				error = "failed to create the DirectWrite custom font collection";
			goto cleanup;
		}
		collection = custom_collection;
		collection->AddRef();
	}
	else if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection) {
		error = "failed to read the system DirectWrite font collection";
		goto cleanup;
	}

	for (UINT32 family_index = 0; family_index < collection->GetFontFamilyCount(); ++family_index) {
		IDWriteFontFamily *family = nullptr;
		if (FAILED(collection->GetFontFamily(family_index, &family)) || !family)
			continue;

		for (UINT32 font_index = 0; font_index < family->GetFontCount(); ++font_index) {
			IDWriteFont *font = nullptr;
			if (FAILED(family->GetFont(font_index, &font)) || !font)
				continue;
			if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
				font->Release();
				continue;
			}

			IDWriteFontFace *face = CreateFontFaceFromFont(font);
			if (!face) {
				font->Release();
				continue;
			}

			FontMatchCandidate metadata;
			for (auto const& name : GetWin32FamilyNamesFromFont(font))
				add_unique(metadata.families, name.value);
			metadata.fullnames = GetFullNamesFromFont(font);
			metadata.postscript_name = GetPostScriptNameFromFont(font);
			if (!metadata.families.empty())
				metadata.extended_family = metadata.families.front();
			metadata.face_index = static_cast<int>(face->GetIndex());
			GetFontFilePath(face, metadata.path, metadata.face_index);
			read_libass_attributes(face, font, metadata);
			// Match libass ass_directwrite.c check_postscript (CFF / RAW_CFF / Type1).
			auto const type = face->GetType();
			metadata.postscript_outlines =
				type == DWRITE_FONT_FACE_TYPE_CFF ||
				type == DWRITE_FONT_FACE_TYPE_RAW_CFF ||
				type == DWRITE_FONT_FACE_TYPE_TYPE1;

			if (!metadata.families.empty()) {
				faces.push_back(std::move(metadata));
				dwrite_faces.push_back(face);
			}
			else
				face->Release();
			font->Release();
		}
		family->Release();
	}
	catalog_ready = true;
	if (custom_catalog && faces.empty() && error.empty())
		error = added_private_file
			? "the DirectWrite custom font collection contains no usable font faces"
			: "no additional font files were loaded by DirectWrite";
	collection->Release();

cleanup:
	if (custom_collection)
		custom_collection->Release();
	if (font_set)
		font_set->Release();
	if (builder)
		builder->Release();
	if (factory5)
		factory5->Release();
	return catalog_ready;
}

std::vector<DWriteLocalizedName> DWriteBridge::GetWin32FamilyNamesFromFont(IDWriteFont *font) const {
	return win32_family_names(font);
}

std::vector<DWriteLocalizedName> DWriteBridge::GetWin32FamilyNamesFromFace(IDWriteFontFace *face) const {
	if (!face)
		return {};

	// IDWriteFontFace does not expose informational strings directly. The
	// face3 interface does, and unlike CreateFontFromLOGFONT this reads the
	// exact physical face selected by the HDC interop call.
	IDWriteFontFace3 *face3 = nullptr;
	if (FAILED(face->QueryInterface(&face3)) || !face3)
		return {};
	auto release_face3 = agi::make_scope_exit([&] { face3->Release(); });

	IDWriteLocalizedStrings *strings = nullptr;
	BOOL exists = FALSE;
	auto hr = face3->GetInformationalStrings(
		DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &strings, &exists);
	if (FAILED(hr) || !exists || !strings) {
		if (strings)
			strings->Release();
		return {};
	}
	auto names = localized_strings_with_locale(strings);
	strings->Release();
	return names;
}

namespace {
std::vector<DWriteLocalizedName> informational_names_from_face(
	IDWriteFontFace *face, DWRITE_INFORMATIONAL_STRING_ID id) {
	if (!face)
		return {};
	IDWriteFontFace3 *face3 = nullptr;
	if (FAILED(face->QueryInterface(&face3)) || !face3)
		return {};
	IDWriteLocalizedStrings *strings = nullptr;
	BOOL exists = FALSE;
	auto hr = face3->GetInformationalStrings(id, &strings, &exists);
	face3->Release();
	if (FAILED(hr) || !exists || !strings) {
		if (strings)
			strings->Release();
		return {};
	}
	auto result = localized_strings_with_locale(strings);
	strings->Release();
	return result;
}
}

std::vector<DWriteLocalizedName> DWriteBridge::GetFullNamesFromFace(IDWriteFontFace *face) const {
	return informational_names_from_face(face, DWRITE_INFORMATIONAL_STRING_FULL_NAME);
}

std::vector<DWriteLocalizedName> DWriteBridge::GetPostScriptNamesFromFace(IDWriteFontFace *face) const {
	return informational_names_from_face(face, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME);
}

std::vector<std::string> DWriteBridge::GetFullNamesFromFont(IDWriteFont *font) const {
	return informational_strings(font, DWRITE_INFORMATIONAL_STRING_FULL_NAME);
}

std::string DWriteBridge::GetPostScriptNameFromFont(IDWriteFont *font) const {
	auto values = informational_strings(font, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME);
	return values.empty() ? std::string() : values.front();
}

namespace {
// Minimal IDWriteTextRenderer that only captures the font used for the first glyph run.
// Mirrors libass ass_directwrite.c FallbackLogTextRenderer.
struct FallbackLogTextRenderer final : IDWriteTextRenderer {
	ULONG ref_count = 1;
	IDWriteFactory *factory = nullptr;

	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (!ppvObject)
			return E_POINTER;
		if (riid == __uuidof(IUnknown) ||
		    riid == __uuidof(IDWritePixelSnapping) ||
		    riid == __uuidof(IDWriteTextRenderer)) {
			*ppvObject = static_cast<IDWriteTextRenderer *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override {
		return InterlockedIncrement(&ref_count);
	}
	ULONG STDMETHODCALLTYPE Release() override {
		// Stack-owned; never delete.
		return InterlockedDecrement(&ref_count);
	}

	// IDWritePixelSnapping
	HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *isDisabled) override {
		if (!isDisabled)
			return E_POINTER;
		*isDisabled = TRUE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *) override {
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *) override {
		return E_NOTIMPL;
	}

	// IDWriteTextRenderer
	HRESULT STDMETHODCALLTYPE DrawGlyphRun(
		void *clientDrawingContext,
		FLOAT, FLOAT,
		DWRITE_MEASURING_MODE,
		DWRITE_GLYPH_RUN const *glyphRun,
		DWRITE_GLYPH_RUN_DESCRIPTION const *,
		IUnknown *) override {
		if (!clientDrawingContext || !glyphRun || !glyphRun->fontFace || !factory)
			return E_FAIL;
		auto **out_font = static_cast<IDWriteFont **>(clientDrawingContext);
		if (*out_font)
			return S_OK;

		IDWriteFontCollection *collection = nullptr;
		if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection)
			return E_FAIL;
		IDWriteFont *font = nullptr;
		auto const hr = collection->GetFontFromFontFace(glyphRun->fontFace, &font);
		collection->Release();
		if (FAILED(hr) || !font)
			return E_FAIL;
		*out_font = font;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, DWRITE_UNDERLINE const *, IUnknown *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const *, IUnknown *) override {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DrawInlineObject(
		void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL, IUnknown *) override {
		return S_OK;
	}
};

int encode_utf16_codepoint(uint32_t codepoint, wchar_t out[2]) {
	if (codepoint <= 0xFFFF) {
		out[0] = static_cast<wchar_t>(codepoint);
		return 1;
	}
	codepoint -= 0x10000;
	out[0] = static_cast<wchar_t>(0xD800 + (codepoint >> 10));
	out[1] = static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF));
	return 2;
}
} // namespace

std::optional<std::string> DWriteBridge::ResolveSystemFallbackFamily(uint32_t codepoint) const {
	if (!available_ || !factory)
		return std::nullopt;

	// Match libass FALLBACK_DEFAULT_FONT (Arial) used as the layout seed font.
	IDWriteTextFormat *text_format = nullptr;
	if (FAILED(factory->CreateTextFormat(
		L"Arial",
		nullptr,
		DWRITE_FONT_WEIGHT_MEDIUM,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		1.0f,
		L"",
		&text_format)) || !text_format)
		return std::nullopt;

	wchar_t char_string[2] = {};
	int const char_len = encode_utf16_codepoint(codepoint, char_string);
	IDWriteTextLayout *text_layout = nullptr;
	if (FAILED(factory->CreateTextLayout(
		char_string,
		static_cast<UINT32>(char_len),
		text_format,
		0.0f,
		0.0f,
		&text_layout)) || !text_layout) {
		text_format->Release();
		return std::nullopt;
	}

	// Stack-allocated renderer; Draw only AddRefs if QI'd, which layout may not do.
	FallbackLogTextRenderer renderer;
	renderer.factory = factory;
	IDWriteFont *font = nullptr;
	auto const draw_hr = text_layout->Draw(&font, &renderer, 0.0f, 0.0f);
	text_layout->Release();
	text_format->Release();
	if (FAILED(draw_hr) || !font)
		return std::nullopt;

	if (codepoint > 0) {
		BOOL exists = FALSE;
		if (FAILED(font->HasCharacter(codepoint, &exists)) || !exists) {
			font->Release();
			return std::nullopt;
		}
	}

	std::optional<std::string> family;
	auto names = win32_family_names(font);
	if (!names.empty())
		family = names.front().value;
	font->Release();
	return family;
}

bool DWriteBridge::GetFontFilePath(IDWriteFontFace *face, std::string &out_path, int &out_face_index) const {
	if (!face)
		return false;

	IDWriteFontFile *file = nullptr;
	const void *ref_key = nullptr;
	UINT32 ref_key_size = 0;
	if (!get_font_file_and_key(face, &file, &ref_key, &ref_key_size))
		return false;

	IDWriteFontFileLoader *loader = nullptr;
	auto hr = file->GetLoader(&loader);
	if (FAILED(hr) || !loader) {
		file->Release();
		return false;
	}

	IDWriteLocalFontFileLoader *local_loader = nullptr;
	hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
	                            reinterpret_cast<void **>(&local_loader));
	loader->Release();
	if (FAILED(hr) || !local_loader) {
		file->Release();
		return false;
	}

	UINT32 path_len = 0;
	hr = local_loader->GetFilePathLengthFromKey(ref_key, ref_key_size, &path_len);
	if (FAILED(hr)) {
		local_loader->Release();
		file->Release();
		return false;
	}

	std::vector<wchar_t> wpath(path_len + 1);
	hr = local_loader->GetFilePathFromKey(ref_key, ref_key_size, wpath.data(), path_len + 1);
	local_loader->Release();
	file->Release();

	if (FAILED(hr))
		return false;

	// DWrite may return a font path with different casing than the filesystem.
	{
		std::wstring fixed;
		fixed.reserve(path_len);
		// Copy the drive root or UNC prefix as-is (e.g. "C:" or "\\server\share").
		auto const root_end = path_len >= 2 && wpath[1] == L':' ? 2 :
		                      (path_len >= 2 && wpath[0] == L'\\' && wpath[1] == L'\\') ? path_len : 0;
		fixed.assign(wpath.data(), root_end);

		for (size_t i = root_end; i < path_len; ) {
			while (i < path_len && (wpath[i] == L'\\' || wpath[i] == L'/'))
				++i;
			if (i >= path_len) break;

			size_t comp_end = i;
			while (comp_end < path_len && wpath[comp_end] != L'\\' && wpath[comp_end] != L'/')
				++comp_end;

			fixed.push_back(L'\\');
			auto const comp_begin = fixed.size();
			fixed.append(wpath.data() + i, comp_end - i);

			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW(fixed.c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				FindClose(h);
				fixed.resize(comp_begin);
				fixed.append(fd.cFileName);
			}
			i = comp_end;
		}
		wpath.assign(fixed.begin(), fixed.end());
		wpath.push_back(L'\0');
		path_len = static_cast<UINT32>(fixed.size());
	}

	out_path = wide_to_utf8(wpath.data(), path_len);
	if (!out_path.empty())
		out_face_index = static_cast<int>(face->GetIndex());
	return !out_path.empty();
}

bool DWriteBridge::ReadFontData(IDWriteFontFace *face, std::vector<char>& out_bytes) const {
	out_bytes.clear();
	if (!face)
		return false;

	IDWriteFontFile *file = nullptr;
	const void *ref_key = nullptr;
	UINT32 ref_key_size = 0;
	if (!get_font_file_and_key(face, &file, &ref_key, &ref_key_size))
		return false;

	IDWriteFontFileLoader *loader = nullptr;
	auto hr = file->GetLoader(&loader);
	if (FAILED(hr) || !loader) {
		file->Release();
		return false;
	}

	IDWriteFontFileStream *stream = nullptr;
	hr = loader->CreateStreamFromKey(ref_key, ref_key_size, &stream);
	loader->Release();
	if (FAILED(hr) || !stream) {
		file->Release();
		return false;
	}

	UINT64 file_size = 0;
	hr = stream->GetFileSize(&file_size);
	if (FAILED(hr) || file_size == 0 || file_size > 64 * 1024 * 1024) {
		stream->Release();
		file->Release();
		return false;
	}

	const void *fragment = nullptr;
	void *fragment_context = nullptr;
	hr = stream->ReadFileFragment(&fragment, 0, file_size, &fragment_context);
	if (FAILED(hr) || !fragment) {
		stream->Release();
		file->Release();
		return false;
	}

	auto const *begin = static_cast<char const *>(fragment);
	out_bytes.assign(begin, begin + static_cast<size_t>(file_size));
	stream->ReleaseFileFragment(fragment_context);
	stream->Release();
	file->Release();
	return true;
}

bool DWriteBridge::HasGlyph(IDWriteFontFace *face, uint32_t codepoint) const {
	if (!face || codepoint == 0)
		return true;

	UINT16 glyph_index = 0;
	auto hr = face->GetGlyphIndices(&codepoint, 1, &glyph_index);
	return SUCCEEDED(hr) && glyph_index != 0;
}

namespace {
class DWriteLibassFontProvider final : public ILibassFontProvider {
	std::unique_ptr<DWriteBridge> bridge;
	std::vector<FontMatchCandidate> faces;
	std::vector<IDWriteFontFace *> dwrite_faces;
	std::unordered_map<uint32_t, std::optional<std::string>> fallback_cache;
	mutable std::unordered_map<size_t, std::shared_ptr<std::vector<char> const>> font_data_cache;
	bool include_system_fonts = true;

public:
	explicit DWriteLibassFontProvider(FontCollectorEventSink& event_sink, FontProviderOptions const& options)
	: bridge(std::make_unique<DWriteBridge>())
	, include_system_fonts(options.include_system_fonts) {
		std::string error;
		if (!bridge->available()) {
			error = "DirectWrite is unavailable";
		}
		else {
			// Eager system catalog matches current collector design: one-shot batch
			// resolution benefits from a prebuilt face list. Real libass DirectWrite
			// is lazy (match_fonts on demand); catalog order may differ from GDI enum.
			bool const catalog_ready = bridge->BuildFontCatalog(
				faces, dwrite_faces, options.additional_font_files, options.include_system_fonts, error);
			if (!catalog_ready && error.empty())
				error = "failed to build the DirectWrite font catalog";
			if (catalog_ready && faces.empty() && error.empty()) {
				error = options.include_system_fonts
					? "DirectWrite system font catalog is empty"
					: "DirectWrite private font catalog is empty";
			}
		}
		if (!error.empty() && event_sink) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FontCacheError;
			event.message = error;
			event_sink(event);
		}
	}

	~DWriteLibassFontProvider() override {
		for (auto *face : dwrite_faces)
			face->Release();
	}

	std::span<LibassFontFace const> GetLibassFaces() const override { return faces; }

	bool HasLibassGlyph(size_t face_index, uint32_t codepoint) const override {
		return face_index < dwrite_faces.size() && bridge->HasGlyph(dwrite_faces[face_index], codepoint);
	}

	std::shared_ptr<std::vector<char> const> GetLibassFontData(size_t face_index) const override {
		if (auto cached = font_data_cache.find(face_index); cached != font_data_cache.end())
			return cached->second;

		std::shared_ptr<std::vector<char> const> data;
		if (face_index < dwrite_faces.size()) {
			std::vector<char> bytes;
			if (bridge->ReadFontData(dwrite_faces[face_index], bytes) && !bytes.empty())
				data = std::make_shared<std::vector<char> const>(std::move(bytes));
		}
		font_data_cache.emplace(face_index, data);
		return data;
	}

	std::vector<std::string> GetLibassSubstitutions(std::string_view family) const override {
		// Match libass ass_directwrite.c font_substitutions exactly.
		if (ascii_case_insensitive_equals(family, "sans-serif"))
			return {"Arial"};
		if (ascii_case_insensitive_equals(family, "serif"))
			return {"Times New Roman"};
		if (ascii_case_insensitive_equals(family, "monospace"))
			return {"Courier New"};
		return {};
	}

	std::optional<std::string> GetLibassFallback(std::string_view, uint32_t codepoint) override {
		if (auto cached = fallback_cache.find(codepoint); cached != fallback_cache.end())
			return cached->second;

		// A private-only catalog must remain self-contained and deterministic.
		if (include_system_fonts) {
			if (auto layout = bridge->ResolveSystemFallbackFamily(codepoint)) {
				fallback_cache.emplace(codepoint, layout);
				return layout;
			}
		}

		for (size_t index = 0; index < faces.size(); ++index) {
			if (faces[index].families.empty())
				continue;
			if (!bridge->HasGlyph(dwrite_faces[index], codepoint))
				continue;
			auto fallback = std::optional<std::string>(faces[index].families.front());
			fallback_cache.emplace(codepoint, fallback);
			return fallback;
		}

		fallback_cache.emplace(codepoint, std::nullopt);
		return std::nullopt;
	}

	std::string_view GetLibassProviderName() const override { return "directwrite"; }
};
}

std::unique_ptr<ILibassFontProvider> CreateDWriteLibassFontProvider(
	FontCollectorEventSink& event_sink,
	FontProviderOptions const& options) {
	return std::make_unique<DWriteLibassFontProvider>(event_sink, options);
}
