#include "font_family_obs_store.h"

#include <libaegisub/crc32.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// magic..flags (6×u32) + created/app (2×u64) + counts/crc/reserved (8×u32)
constexpr std::size_t kHeaderBytes = 72;

struct Header {
	std::uint32_t magic = 0;
	std::uint32_t storage_schema_version = 0;
	std::uint32_t observation_contract_version = 0;
	std::uint32_t derivation_contract_version = 0;
	std::uint32_t manifest_contract_version = 0;
	std::uint32_t flags = 0;
	std::uint64_t created_utc_unix = 0;
	std::uint64_t app_build_id = 0;
	std::uint32_t face_count = 0;
	std::uint32_t seed_count = 0;
	std::uint32_t alias_count = 0;
	std::uint32_t string_count = 0;
	std::uint32_t string_pool_bytes = 0;
	std::uint32_t payload_size = 0;
	std::uint32_t payload_crc32 = 0;
	std::uint32_t reserved = 0;
};

static_assert(sizeof(Header) == kHeaderBytes, "observation store header size");

class ByteReader {
	std::string_view bytes_;
	std::size_t pos_ = 0;
	bool ok_ = true;

public:
	explicit ByteReader(std::string_view bytes) : bytes_(bytes) {}

	bool ok() const noexcept { return ok_; }
	std::size_t remaining() const noexcept {
		return ok_ && pos_ <= bytes_.size() ? bytes_.size() - pos_ : 0;
	}
	bool finished() const noexcept { return ok_ && pos_ == bytes_.size(); }

	void fail() { ok_ = false; }

	template <class T>
	T read_le() {
		static_assert(std::is_integral_v<T>);
		using U = std::make_unsigned_t<T>;
		if (!ok_ || remaining() < sizeof(U)) {
			fail();
			return T{};
		}
		U value = 0;
		for (std::size_t i = 0; i < sizeof(U); ++i)
			value |= static_cast<U>(static_cast<unsigned char>(bytes_[pos_ + i]))
				<< (8 * i);
		pos_ += sizeof(U);
		return static_cast<T>(value);
	}

	std::string_view read_bytes(std::size_t n) {
		if (!ok_ || remaining() < n) {
			fail();
			return {};
		}
		auto view = bytes_.substr(pos_, n);
		pos_ += n;
		return view;
	}
};

class ByteWriter {
	std::string bytes_;

public:
	std::string& data() { return bytes_; }
	std::string const& data() const { return bytes_; }
	std::size_t size() const { return bytes_.size(); }

	template <class T>
	void write_le(T value) {
		static_assert(std::is_integral_v<T>);
		using U = std::make_unsigned_t<T>;
		auto const raw = static_cast<U>(value);
		for (std::size_t i = 0; i < sizeof(U); ++i)
			bytes_.push_back(static_cast<char>((raw >> (8 * i)) & 0xFFu));
	}

	void write_bytes(std::string_view data) {
		bytes_.append(data.data(), data.size());
	}
};

struct StringPool {
	std::vector<std::string> strings;
	std::uint32_t total_bytes = 0;

	std::uint32_t intern(std::string_view value) {
		auto const it = std::find(strings.begin(), strings.end(), value);
		if (it != strings.end())
			return static_cast<std::uint32_t>(it - strings.begin());
		if (strings.size() >= std::numeric_limits<std::uint32_t>::max())
			throw std::overflow_error("string pool overflow");
		// length prefix + payload counted toward the hard pool limit
		auto const cost = static_cast<std::uint64_t>(4 + value.size());
		if (static_cast<std::uint64_t>(total_bytes) + cost >
			kFontFamilyObsMaxStringPoolBytes)
			throw std::length_error("string pool too large");
		total_bytes = static_cast<std::uint32_t>(total_bytes + cost);
		strings.emplace_back(value);
		return static_cast<std::uint32_t>(strings.size() - 1);
	}
};

bool counts_within_limits(
	std::uint32_t faces,
	std::uint32_t seeds,
	std::uint32_t aliases,
	std::uint32_t string_pool_bytes) {
	return faces <= kFontFamilyObsMaxFaces &&
		seeds <= kFontFamilyObsMaxSeeds &&
		aliases <= kFontFamilyObsMaxAliases &&
		string_pool_bytes <= kFontFamilyObsMaxStringPoolBytes;
}

bool valid_name_kind(std::uint8_t kind) {
	switch (static_cast<FontFamilyNameKind>(kind)) {
		case FontFamilyNameKind::Win32Family:
		case FontFamilyNameKind::TypographicFamily:
		case FontFamilyNameKind::FullName:
		case FontFamilyNameKind::PostScript:
		case FontFamilyNameKind::PlatformAlias:
			return true;
	}
	return false;
}

bool valid_role(std::uint8_t role) {
	switch (static_cast<FontVariantRole>(role)) {
		case FontVariantRole::Unknown:
		case FontVariantRole::Regular:
		case FontVariantRole::Bold:
		case FontVariantRole::Italic:
		case FontVariantRole::BoldItalic:
			return true;
	}
	return false;
}

bool valid_status(std::uint8_t status) {
	switch (static_cast<FontVariantStatus>(status)) {
		case FontVariantStatus::Unknown:
		case FontVariantStatus::Canonical:
		case FontVariantStatus::NonCanonical:
		case FontVariantStatus::Synthetic:
			return true;
	}
	return false;
}

Header read_header(ByteReader& reader) {
	Header header;
	header.magic = reader.read_le<std::uint32_t>();
	header.storage_schema_version = reader.read_le<std::uint32_t>();
	header.observation_contract_version = reader.read_le<std::uint32_t>();
	header.derivation_contract_version = reader.read_le<std::uint32_t>();
	header.manifest_contract_version = reader.read_le<std::uint32_t>();
	header.flags = reader.read_le<std::uint32_t>();
	header.created_utc_unix = reader.read_le<std::uint64_t>();
	header.app_build_id = reader.read_le<std::uint64_t>();
	header.face_count = reader.read_le<std::uint32_t>();
	header.seed_count = reader.read_le<std::uint32_t>();
	header.alias_count = reader.read_le<std::uint32_t>();
	header.string_count = reader.read_le<std::uint32_t>();
	header.string_pool_bytes = reader.read_le<std::uint32_t>();
	header.payload_size = reader.read_le<std::uint32_t>();
	header.payload_crc32 = reader.read_le<std::uint32_t>();
	header.reserved = reader.read_le<std::uint32_t>();
	return header;
}

void write_header(ByteWriter& writer, Header const& header) {
	writer.write_le(header.magic);
	writer.write_le(header.storage_schema_version);
	writer.write_le(header.observation_contract_version);
	writer.write_le(header.derivation_contract_version);
	writer.write_le(header.manifest_contract_version);
	writer.write_le(header.flags);
	writer.write_le(header.created_utc_unix);
	writer.write_le(header.app_build_id);
	writer.write_le(header.face_count);
	writer.write_le(header.seed_count);
	writer.write_le(header.alias_count);
	writer.write_le(header.string_count);
	writer.write_le(header.string_pool_bytes);
	writer.write_le(header.payload_size);
	writer.write_le(header.payload_crc32);
	writer.write_le(header.reserved);
}

std::string resolve_string(
	std::vector<std::string> const& pool,
	std::uint32_t index,
	ByteReader& reader) {
	if (index >= pool.size()) {
		reader.fail();
		return {};
	}
	return pool[index];
}

void write_name(ByteWriter& writer, StringPool& pool, FontFamilyName const& name) {
	writer.write_le(pool.intern(name.value));
	writer.write_le(pool.intern(name.locale));
	writer.write_le(static_cast<std::uint8_t>(name.kind));
	writer.write_le(static_cast<std::uint8_t>(name.variant_role));
	writer.write_le(static_cast<std::uint16_t>(0));
	writer.write_le(name.entity_token);
}

FontFamilyName read_name(
	ByteReader& reader,
	std::vector<std::string> const& pool) {
	FontFamilyName name;
	auto const value_idx = reader.read_le<std::uint32_t>();
	auto const locale_idx = reader.read_le<std::uint32_t>();
	auto const kind = reader.read_le<std::uint8_t>();
	auto const role = reader.read_le<std::uint8_t>();
	(void)reader.read_le<std::uint16_t>();
	name.entity_token = reader.read_le<std::uint64_t>();
	if (!valid_name_kind(kind) || !valid_role(role)) {
		reader.fail();
		return {};
	}
	name.value = resolve_string(pool, value_idx, reader);
	name.locale = resolve_string(pool, locale_idx, reader);
	name.kind = static_cast<FontFamilyNameKind>(kind);
	name.variant_role = static_cast<FontVariantRole>(role);
	return name;
}

void write_probe(
	ByteWriter& writer,
	StringPool& pool,
	FontFamilyProbeObservation const& probe,
	std::uint32_t face_count) {
	writer.write_le(static_cast<std::int32_t>(probe.outcome.requested_weight));
	writer.write_le(static_cast<std::uint8_t>(probe.outcome.requested_italic ? 1 : 0));
	writer.write_le(static_cast<std::int32_t>(probe.outcome.realized_weight));
	writer.write_le(static_cast<std::uint8_t>(probe.outcome.realized_italic ? 1 : 0));
	writer.write_le(static_cast<std::uint8_t>(probe.outcome.role));
	writer.write_le(static_cast<std::uint8_t>(probe.outcome.status));
	writer.write_le(static_cast<std::uint8_t>(probe.success ? 1 : 0));
	writer.write_le(static_cast<std::uint8_t>(probe.matches_requested_family ? 1 : 0));
	writer.write_le(probe.outcome.entity_token);
	writer.write_le(probe.face_ref);
	if (probe.face_ref != kFontFamilyInvalidFaceRef && probe.face_ref >= face_count)
		throw std::invalid_argument("probe face_ref out of range");
	if (probe.informational_names.size() > kFontFamilyObsMaxNamesPerList)
		throw std::length_error("too many informational names");
	writer.write_le(static_cast<std::uint32_t>(probe.informational_names.size()));
	for (auto const& name : probe.informational_names)
		write_name(writer, pool, name);
}

FontFamilyProbeObservation read_probe(
	ByteReader& reader,
	std::vector<std::string> const& pool,
	std::uint32_t face_count) {
	FontFamilyProbeObservation probe;
	probe.outcome.requested_weight = reader.read_le<std::int32_t>();
	probe.outcome.requested_italic = reader.read_le<std::uint8_t>() != 0;
	probe.outcome.realized_weight = reader.read_le<std::int32_t>();
	probe.outcome.realized_italic = reader.read_le<std::uint8_t>() != 0;
	auto const role = reader.read_le<std::uint8_t>();
	auto const status = reader.read_le<std::uint8_t>();
	probe.success = reader.read_le<std::uint8_t>() != 0;
	probe.matches_requested_family = reader.read_le<std::uint8_t>() != 0;
	probe.outcome.entity_token = reader.read_le<std::uint64_t>();
	probe.face_ref = reader.read_le<std::uint32_t>();
	auto const info_count = reader.read_le<std::uint32_t>();
	if (!valid_role(role) || !valid_status(status) ||
	    info_count > kFontFamilyObsMaxNamesPerList ||
	    (probe.face_ref != kFontFamilyInvalidFaceRef &&
	     probe.face_ref >= face_count)) {
		reader.fail();
		return {};
	}
	probe.outcome.role = static_cast<FontVariantRole>(role);
	probe.outcome.status = static_cast<FontVariantStatus>(status);
	probe.informational_names.reserve(info_count);
	for (std::uint32_t i = 0; i < info_count; ++i)
		probe.informational_names.push_back(read_name(reader, pool));
	return probe;
}

FontFamilyObsStoreStatus encode_payload(
	FontFamilyObsStorePayload const& payload,
	std::string& out_bytes) {
	if (!payload.observations.complete)
		return FontFamilyObsStoreStatus::Incomplete;
	if (!counts_within_limits(
			static_cast<std::uint32_t>(payload.observations.faces.size()),
			static_cast<std::uint32_t>(payload.observations.seeds.size()),
			static_cast<std::uint32_t>(payload.observations.aliases.size()),
			0))
		return FontFamilyObsStoreStatus::TooLarge;
	if (payload.manifest.gdi_family_names.size() > kFontFamilyObsMaxSeeds)
		return FontFamilyObsStoreStatus::TooLarge;

	try {
		StringPool pool;
		ByteWriter body;
		auto const face_count =
			static_cast<std::uint32_t>(payload.observations.faces.size());

		body.write_le(payload.manifest.provider_fingerprint);
		body.write_le(payload.manifest.os_build_fingerprint);
		body.write_le(payload.manifest.font_registry_fingerprint);
		body.write_le(static_cast<std::uint32_t>(payload.manifest.gdi_family_names.size()));
		for (auto const& name : payload.manifest.gdi_family_names)
			body.write_le(pool.intern(name));

		for (auto const& face : payload.observations.faces) {
			body.write_le(face.volume_serial);
			body.write_le(face.file_index);
			body.write_le(face.face_index);
			body.write_le(face.size);
			body.write_le(face.mtime_utc_100ns);
			body.write_le(face.usn);
			body.write_le(face.content_hash);
			body.write_le(face.flags);
		}

		for (auto const& seed : payload.observations.seeds) {
			body.write_le(pool.intern(seed.seed_family_name));
			body.write_le(static_cast<std::uint8_t>(
				seed.profile_matches_requested_family ? 1 : 0));
			body.write_le(static_cast<std::uint8_t>(0));
			body.write_le(static_cast<std::uint16_t>(0));
			for (auto const& probe : seed.rbiz)
				write_probe(body, pool, probe, face_count);
			if (seed.win32_family_names.size() > kFontFamilyObsMaxNamesPerList)
				return FontFamilyObsStoreStatus::TooLarge;
			body.write_le(static_cast<std::uint32_t>(seed.win32_family_names.size()));
			for (auto const& name : seed.win32_family_names)
				write_name(body, pool, name);
		}

		for (auto const& alias : payload.observations.aliases) {
			body.write_le(pool.intern(alias.candidate_name));
			for (auto const& probe : alias.rbiz)
				write_probe(body, pool, probe, face_count);
		}

		ByteWriter pool_bytes;
		for (auto const& value : pool.strings) {
			if (value.size() > std::numeric_limits<std::uint32_t>::max())
				return FontFamilyObsStoreStatus::TooLarge;
			pool_bytes.write_le(static_cast<std::uint32_t>(value.size()));
			pool_bytes.write_bytes(value);
		}

		ByteWriter payload_writer;
		payload_writer.write_bytes(pool_bytes.data());
		payload_writer.write_bytes(body.data());
		if (payload_writer.size() > kFontFamilyObsMaxFileBytes - kHeaderBytes)
			return FontFamilyObsStoreStatus::TooLarge;
		if (payload_writer.size() > std::numeric_limits<std::uint32_t>::max())
			return FontFamilyObsStoreStatus::TooLarge;

		Header header;
		header.magic = kFontFamilyObsMagic;
		header.storage_schema_version = kFontFamilyObsStorageSchemaVersion;
		header.observation_contract_version = payload.observation_contract_version;
		header.derivation_contract_version = payload.derivation_contract_version;
		header.manifest_contract_version = payload.manifest.manifest_contract_version;
		header.flags = 0;
		header.created_utc_unix = payload.created_utc_unix;
		header.app_build_id = payload.app_build_id;
		header.face_count = face_count;
		header.seed_count =
			static_cast<std::uint32_t>(payload.observations.seeds.size());
		header.alias_count =
			static_cast<std::uint32_t>(payload.observations.aliases.size());
		header.string_count = static_cast<std::uint32_t>(pool.strings.size());
		header.string_pool_bytes = static_cast<std::uint32_t>(pool_bytes.size());
		header.payload_size = static_cast<std::uint32_t>(payload_writer.size());
		header.payload_crc32 = agi::util::crc32(std::string_view(
			payload_writer.data().data(), payload_writer.data().size()));
		header.reserved = 0;

		ByteWriter file;
		write_header(file, header);
		file.write_bytes(payload_writer.data());
		if (file.size() > kFontFamilyObsMaxFileBytes)
			return FontFamilyObsStoreStatus::TooLarge;
		out_bytes = std::move(file.data());
		return FontFamilyObsStoreStatus::Ok;
	}
	catch (std::exception const&) {
		return FontFamilyObsStoreStatus::RefuseWrite;
	}
}

} // namespace

char const* FontFamilyObsStoreStatusName(FontFamilyObsStoreStatus status) noexcept {
	switch (status) {
		case FontFamilyObsStoreStatus::Ok: return "ok";
		case FontFamilyObsStoreStatus::NotFound: return "not_found";
		case FontFamilyObsStoreStatus::IoError: return "io_error";
		case FontFamilyObsStoreStatus::BadMagic: return "bad_magic";
		case FontFamilyObsStoreStatus::BadSchema: return "bad_schema";
		case FontFamilyObsStoreStatus::BadCrc: return "bad_crc";
		case FontFamilyObsStoreStatus::BadCounts: return "bad_counts";
		case FontFamilyObsStoreStatus::TooLarge: return "too_large";
		case FontFamilyObsStoreStatus::Incomplete: return "incomplete";
		case FontFamilyObsStoreStatus::Truncated: return "truncated";
		case FontFamilyObsStoreStatus::ContractMismatch: return "contract_mismatch";
		case FontFamilyObsStoreStatus::RefuseWrite: return "refuse_write";
	}
	return "unknown";
}

FontFamilyObsStoreStatus DecodeFontFamilyObsStore(
	std::string_view bytes,
	FontFamilyObsStorePayload& out) {
	try {
		out = {};
		if (bytes.size() < kHeaderBytes)
			return FontFamilyObsStoreStatus::Truncated;
		if (bytes.size() > kFontFamilyObsMaxFileBytes)
			return FontFamilyObsStoreStatus::TooLarge;

		ByteReader header_reader(bytes.substr(0, kHeaderBytes));
		auto const header = read_header(header_reader);
		if (!header_reader.finished())
			return FontFamilyObsStoreStatus::Truncated;
		if (header.magic != kFontFamilyObsMagic)
			return FontFamilyObsStoreStatus::BadMagic;
		if (header.storage_schema_version != kFontFamilyObsStorageSchemaVersion)
			return FontFamilyObsStoreStatus::BadSchema;
		if (header.flags != 0)
			return FontFamilyObsStoreStatus::BadSchema;
		if (header.payload_size != bytes.size() - kHeaderBytes)
			return FontFamilyObsStoreStatus::Truncated;
		if (!counts_within_limits(
				header.face_count, header.seed_count, header.alias_count,
				header.string_pool_bytes))
			return FontFamilyObsStoreStatus::TooLarge;
		if (header.string_pool_bytes > header.payload_size)
			return FontFamilyObsStoreStatus::BadCounts;
		// Guard pool.reserve against hostile string_count before any allocation.
		// Each pool entry costs at least a 4-byte length prefix in the pool region.
		if (header.string_count > kFontFamilyObsMaxStrings)
			return FontFamilyObsStoreStatus::TooLarge;
		if (header.string_pool_bytes / sizeof(std::uint32_t) < header.string_count)
			return FontFamilyObsStoreStatus::BadCounts;

		auto const payload_bytes = bytes.substr(kHeaderBytes);
		auto const crc = agi::util::crc32(payload_bytes);
		if (crc != header.payload_crc32)
			return FontFamilyObsStoreStatus::BadCrc;

		ByteReader reader(payload_bytes);
		std::vector<std::string> pool;
		pool.reserve(header.string_count);
		std::uint32_t consumed_pool = 0;
		for (std::uint32_t i = 0; i < header.string_count; ++i) {
			auto const length = reader.read_le<std::uint32_t>();
			auto const data = reader.read_bytes(length);
			if (!reader.ok())
				return FontFamilyObsStoreStatus::Truncated;
			consumed_pool += 4 + length;
			if (consumed_pool > header.string_pool_bytes)
				return FontFamilyObsStoreStatus::BadCounts;
			pool.emplace_back(data);
		}
		if (consumed_pool != header.string_pool_bytes)
			return FontFamilyObsStoreStatus::BadCounts;

		FontFamilyObsStorePayload decoded;
		decoded.observation_contract_version = header.observation_contract_version;
		decoded.derivation_contract_version = header.derivation_contract_version;
		decoded.created_utc_unix = header.created_utc_unix;
		decoded.app_build_id = header.app_build_id;
		decoded.manifest.manifest_contract_version = header.manifest_contract_version;
		decoded.manifest.provider_fingerprint = reader.read_le<std::uint64_t>();
		decoded.manifest.os_build_fingerprint = reader.read_le<std::uint64_t>();
		decoded.manifest.font_registry_fingerprint = reader.read_le<std::uint64_t>();
		auto const family_count = reader.read_le<std::uint32_t>();
		if (family_count > kFontFamilyObsMaxSeeds) {
			reader.fail();
		} else {
			decoded.manifest.gdi_family_names.reserve(family_count);
			for (std::uint32_t i = 0; i < family_count; ++i) {
				auto const idx = reader.read_le<std::uint32_t>();
				decoded.manifest.gdi_family_names.push_back(
					resolve_string(pool, idx, reader));
			}
		}

		decoded.observations.faces.reserve(header.face_count);
		for (std::uint32_t i = 0; i < header.face_count; ++i) {
			FontFamilyFaceIdentity face;
			face.volume_serial = reader.read_le<std::uint32_t>();
			face.file_index = reader.read_le<std::uint64_t>();
			face.face_index = reader.read_le<std::int32_t>();
			face.size = reader.read_le<std::uint64_t>();
			face.mtime_utc_100ns = reader.read_le<std::uint64_t>();
			face.usn = reader.read_le<std::uint64_t>();
			face.content_hash = reader.read_le<std::uint64_t>();
			face.flags = reader.read_le<std::uint32_t>();
			decoded.observations.faces.push_back(face);
		}

		decoded.observations.seeds.reserve(header.seed_count);
		for (std::uint32_t i = 0; i < header.seed_count; ++i) {
			FontFamilySeedObservation seed;
			auto const name_idx = reader.read_le<std::uint32_t>();
			seed.seed_family_name = resolve_string(pool, name_idx, reader);
			seed.profile_matches_requested_family = reader.read_le<std::uint8_t>() != 0;
			(void)reader.read_le<std::uint8_t>();
			(void)reader.read_le<std::uint16_t>();
			for (auto& probe : seed.rbiz)
				probe = read_probe(reader, pool, header.face_count);
			auto const win32_count = reader.read_le<std::uint32_t>();
			if (win32_count > kFontFamilyObsMaxNamesPerList) {
				reader.fail();
				break;
			}
			seed.win32_family_names.reserve(win32_count);
			for (std::uint32_t n = 0; n < win32_count; ++n)
				seed.win32_family_names.push_back(read_name(reader, pool));
			decoded.observations.seeds.push_back(std::move(seed));
		}

		decoded.observations.aliases.reserve(header.alias_count);
		for (std::uint32_t i = 0; i < header.alias_count; ++i) {
			FontFamilyAliasObservation alias;
			auto const name_idx = reader.read_le<std::uint32_t>();
			alias.candidate_name = resolve_string(pool, name_idx, reader);
			for (auto& probe : alias.rbiz)
				probe = read_probe(reader, pool, header.face_count);
			decoded.observations.aliases.push_back(std::move(alias));
		}

		if (!reader.ok())
			return FontFamilyObsStoreStatus::Truncated;
		if (!reader.finished())
			return FontFamilyObsStoreStatus::BadCounts;

		decoded.observations.complete = true;
		out = std::move(decoded);
		return FontFamilyObsStoreStatus::Ok;
	}
	catch (std::length_error const&) {
		out = {};
		return FontFamilyObsStoreStatus::TooLarge;
	}
	catch (std::bad_alloc const&) {
		out = {};
		return FontFamilyObsStoreStatus::TooLarge;
	}
}

FontFamilyObsStoreStatus EncodeFontFamilyObsStore(
	FontFamilyObsStorePayload const& payload,
	std::string& out_bytes) {
	out_bytes.clear();
	return encode_payload(payload, out_bytes);
}

FontFamilyObsStoreStatus TryLoadFontFamilyObsStore(
	agi::fs::path const& path,
	FontFamilyObsStorePayload& out) {
	out = {};
	try {
		if (!agi::fs::FileExists(path))
			return FontFamilyObsStoreStatus::NotFound;
		auto const size = agi::fs::Size(path);
		if (size > kFontFamilyObsMaxFileBytes)
			return FontFamilyObsStoreStatus::TooLarge;
		auto stream = agi::io::Open(path, true);
		std::string bytes(static_cast<std::size_t>(size), '\0');
		if (size > 0) {
			stream->read(bytes.data(), static_cast<std::streamsize>(size));
			if (stream->gcount() != static_cast<std::streamsize>(size))
				return FontFamilyObsStoreStatus::IoError;
		}
		return DecodeFontFamilyObsStore(bytes, out);
	}
	catch (...) {
		return FontFamilyObsStoreStatus::IoError;
	}
}

FontFamilyObsStoreStatus SaveFontFamilyObsStoreAtomic(
	agi::fs::path const& path,
	FontFamilyObsStorePayload const& payload) {
	std::string bytes;
	auto const encoded = EncodeFontFamilyObsStore(payload, bytes);
	if (encoded != FontFamilyObsStoreStatus::Ok)
		return encoded;
	try {
		agi::fs::CreateDirectory(path.parent_path());
		{
			agi::io::Save save(path, true);
			save.Get().write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!save.Get().good())
				return FontFamilyObsStoreStatus::IoError;
			save.Close();
		}
		return FontFamilyObsStoreStatus::Ok;
	}
	catch (...) {
		LOG_W("font/family_catalog/obs_store")
			<< "Failed to write observation cache atomically";
		return FontFamilyObsStoreStatus::IoError;
	}
}
