#pragma once

// M1 face-stamp checks for Windows observation cache. Shared by repository Hit
// admission and incremental Observe (seed reuse). Lives outside repository so
// observe_win does not depend on the higher-level LoadOrRebuild entry point.

#ifndef _WIN32
#error "font_family_obs_face_stamp_win.h is Windows-only"
#endif

#include "font_family_obs_types.h"

#include <optional>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/// One volume enumeration per validation pass, and one OpenFileById per unique
/// (volume_serial, file_index). TTC multi-face rows share a single open.
/// Callers should reuse one instance across many faces in a rebuild.
class FontFamilyFaceStampValidator {
	struct FileIdentityKey {
		std::uint32_t volume_serial = 0;
		std::uint64_t file_index = 0;
		bool operator==(FileIdentityKey const&) const = default;
	};
	struct FileIdentityKeyHash {
		std::size_t operator()(FileIdentityKey const& key) const noexcept {
			return static_cast<std::size_t>(key.volume_serial) ^
				(static_cast<std::size_t>(key.file_index) * 0x9E3779B97F4A7C15ull);
		}
	};
	struct LiveFileStamp {
		std::uint64_t size = 0;
		std::uint64_t mtime_utc_100ns = 0;
	};

	std::unordered_map<std::uint32_t, std::wstring> volume_paths_;
	std::unordered_map<FileIdentityKey, std::optional<LiveFileStamp>, FileIdentityKeyHash>
		file_stamps_;
	bool volumes_loaded_ = false;

	void ensure_volumes();
	std::optional<LiveFileStamp> open_live_stamp(FileIdentityKey const& key);

public:
	/// True when the durable identity is known and size/mtime still match live.
	bool matches(FontFamilyFaceIdentity const& face);
};

/// Per-face live stamp classification (same rules as Validate...).
std::vector<bool> ClassifyWindowsFontFamilyFaceStamps(
	std::vector<FontFamilyFaceIdentity> const& faces);

/// Prove every durable face stamp still matches the live file identity.
/// Unknown (zero) identities are never trusted. Empty input returns true
/// (vacuous); callers that need strong M1 must treat faces.empty() separately.
bool ValidateWindowsFontFamilyFaceStamps(
	std::vector<FontFamilyFaceIdentity> const& faces);
