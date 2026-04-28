// Copyright (c) 2026, MIRIMIRIM

#include "font_file_lister_dwrite.h"

#include <dwrite.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <winver.h>

#include <libaegisub/log.h>
#include <libaegisub/native_library.h>

namespace {
using DWriteCreateFactoryFn = HRESULT (WINAPI *)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

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

FontFormat DetermineFontFormat(std::span<const char, 4> data) {
	auto sig = reinterpret_cast<const unsigned char *>(data.data());
	// 'ttcf' covers both TTC (TrueType) and OTC (CFF) collections.
	if (sig[0] == 't' && sig[1] == 't' && sig[2] == 'c' && sig[3] == 'f')
		return FontFormat::Collection;
	if (sig[0] == 'O' && sig[1] == 'T' && sig[2] == 'T' && sig[3] == 'O')
		return FontFormat::OpenType;
	if (sig[0] == 0 && sig[1] == 1 && sig[2] == 0 && sig[3] == 0)
		return FontFormat::TrueType;
	if (sig[0] == 'w' && sig[1] == 'O' && sig[2] == 'F' && sig[3] == 'F')
		return FontFormat::Woff;
	return FontFormat::Unknown;
}

const char *FontFormatExtension(FontFormat format) {
	switch (format) {
		case FontFormat::OpenType:   return ".otf";
		case FontFormat::Collection: return ".ttc";
		case FontFormat::Woff:       return ".woff";
		default:                     return ".ttf";
	}
}

DWriteBridge::DWriteBridge() {
	DWriteCreateFactoryFn create_fn = nullptr;
	dll_handle = try_load_dwrite_core(create_fn);
	is_dwritecore_ = (dll_handle != nullptr);

	if (!dll_handle) {
		dll_handle = try_load_system_dwrite(create_fn);
	}

	if (!dll_handle || !create_fn) {
		LOG_D("font/dwrite") << "DWrite bridge unavailable: failed to load DWrite DLL";
		return;
	}

	if (!init_dwrite_factory(create_fn, &factory, &gdi_interop)) {
		LOG_D("font/dwrite") << "DWrite bridge unavailable: failed to create factory or GDI interop";
		if (gdi_interop) { gdi_interop->Release(); gdi_interop = nullptr; }
		if (factory) { factory->Release(); factory = nullptr; }
		if (dll_handle) { FreeLibrary(dll_handle); dll_handle = nullptr; }
		return;
	}

	available_ = true;
	dll_description_ = get_dll_description(dll_handle, is_dwritecore_);
	LOG_I("font/dwrite") << "DWrite bridge initialized: " << dll_description_;
}

DWriteBridge::~DWriteBridge() {
	if (gdi_interop) gdi_interop->Release();
	if (factory) factory->Release();
	if (dll_handle) FreeLibrary(dll_handle);
}

IDWriteFontFace *DWriteBridge::CreateFontFaceFromHdc(HDC hdc) const {
	if (!available_)
		return nullptr;

	IDWriteFontFace *face = nullptr;
	auto hr = gdi_interop->CreateFontFaceFromHdc(hdc, &face);
	if (FAILED(hr) || !face)
		return nullptr;
	return face;
}

IDWriteFontFace *DWriteBridge::CreateFontFaceFromLogFont(LOGFONTW const &lf) const {
	if (!available_ || !gdi_interop)
		return nullptr;

	IDWriteFont *font = nullptr;
	auto hr = gdi_interop->CreateFontFromLOGFONT(&lf, &font);
	if (FAILED(hr) || !font)
		return nullptr;

	IDWriteFontFace *face = nullptr;
	hr = font->CreateFontFace(&face);
	font->Release();
	if (FAILED(hr) || !face)
		return nullptr;
	return face;
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

	// DWrite may return the path in all-caps (e.g. C:\WINDOWS\FONTS\ARIAL.TTF).
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

bool DWriteBridge::HasGlyph(IDWriteFontFace *face, uint32_t codepoint) const {
	if (!face || codepoint == 0)
		return true;

	UINT16 glyph_index = 0;
	auto hr = face->GetGlyphIndices(&codepoint, 1, &glyph_index);
	return SUCCEEDED(hr) && glyph_index != 0;
}

bool DWriteBridge::ReadFontData(IDWriteFontFace *face, std::vector<char> &out_bytes) const {
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
	void *fragment_ctx = nullptr;
	hr = stream->ReadFileFragment(&fragment, 0, file_size, &fragment_ctx);
	if (FAILED(hr) || !fragment) {
		stream->Release();
		file->Release();
		return false;
	}

	out_bytes.assign(static_cast<const char *>(fragment),
	                 static_cast<const char *>(fragment) + file_size);
	stream->ReleaseFileFragment(fragment_ctx);
	stream->Release();
	file->Release();

	return true;
}
