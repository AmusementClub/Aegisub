#include "font_family_obs_face_stamp_win.h"

#include <algorithm>

void FontFamilyFaceStampValidator::ensure_volumes() {
	if (volumes_loaded_)
		return;
	volumes_loaded_ = true;
	wchar_t volume_name[MAX_PATH] = {};
	HANDLE find = FindFirstVolumeW(
		volume_name, static_cast<DWORD>(std::size(volume_name)));
	if (find == INVALID_HANDLE_VALUE)
		return;
	do {
		DWORD serial = 0;
		if (!GetVolumeInformationW(
				volume_name, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) ||
			serial == 0)
			continue;
		std::wstring open_path = volume_name;
		if (open_path.size() >= 2 && open_path.back() == L'\\')
			open_path.pop_back();
		// First volume path wins if serials collide (extremely rare).
		volume_paths_.emplace(serial, std::move(open_path));
	} while (FindNextVolumeW(
		find, volume_name, static_cast<DWORD>(std::size(volume_name))));
	FindVolumeClose(find);
}

std::optional<FontFamilyFaceStampValidator::LiveFileStamp>
FontFamilyFaceStampValidator::open_live_stamp(FileIdentityKey const& key) {
	ensure_volumes();
	auto const volume_it = volume_paths_.find(key.volume_serial);
	if (volume_it == volume_paths_.end())
		return std::nullopt;

	HANDLE volume = CreateFileW(
		volume_it->second.c_str(), 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (volume == INVALID_HANDLE_VALUE)
		return std::nullopt;

	FILE_ID_DESCRIPTOR descriptor{};
	descriptor.dwSize = static_cast<DWORD>(sizeof(descriptor));
	descriptor.Type = FileIdType;
	descriptor.FileId.QuadPart = static_cast<LONGLONG>(key.file_index);

	HANDLE file = OpenFileById(
		volume, &descriptor, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, 0);
	CloseHandle(volume);
	if (file == INVALID_HANDLE_VALUE)
		return std::nullopt;

	BY_HANDLE_FILE_INFORMATION information{};
	LARGE_INTEGER size{};
	bool const ok = GetFileInformationByHandle(file, &information) != FALSE &&
		GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
	CloseHandle(file);
	if (!ok)
		return std::nullopt;

	auto const live_index =
		(static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
		information.nFileIndexLow;
	if (information.dwVolumeSerialNumber != key.volume_serial ||
		live_index != key.file_index)
		return std::nullopt;

	LiveFileStamp stamp;
	stamp.size = static_cast<std::uint64_t>(size.QuadPart);
	stamp.mtime_utc_100ns =
		(static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
		information.ftLastWriteTime.dwLowDateTime;
	return stamp;
}

bool FontFamilyFaceStampValidator::matches(FontFamilyFaceIdentity const& face) {
	if (!face.IsKnown())
		return false;
	FileIdentityKey const key{face.volume_serial, face.file_index};
	auto it = file_stamps_.find(key);
	if (it == file_stamps_.end())
		it = file_stamps_.emplace(key, open_live_stamp(key)).first;
	if (!it->second)
		return false;
	return it->second->size == face.size &&
		it->second->mtime_utc_100ns == face.mtime_utc_100ns;
}

std::vector<bool> ClassifyWindowsFontFamilyFaceStamps(
	std::vector<FontFamilyFaceIdentity> const& faces) {
	FontFamilyFaceStampValidator validator;
	std::vector<bool> ok(faces.size(), false);
	for (std::size_t i = 0; i < faces.size(); ++i)
		ok[i] = validator.matches(faces[i]);
	return ok;
}

bool ValidateWindowsFontFamilyFaceStamps(
	std::vector<FontFamilyFaceIdentity> const& faces) {
	auto const ok = ClassifyWindowsFontFamilyFaceStamps(faces);
	return std::all_of(ok.begin(), ok.end(), [](bool live) { return live; });
}
