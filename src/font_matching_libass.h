// Font selection behavior derived from libass (ISC License):
//   https://github.com/libass/libass/blob/f9fd3d20dff1cd84b7c74c8ae7f79711ad7736fa/libass/ass_fontselect.c

#pragma once

#include "font_matching_common.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using LibassFontFace = FontMatchCandidate;

struct LibassFontRequest {
	std::string family;
	int weight = 400;
	bool italic = false;
	std::string default_family;
};

struct LibassCodepointMatch {
	uint32_t codepoint = 0;
	std::optional<size_t> face;
	bool fallback = false;
};

enum class LibassFontMatchSource {
	Requested,
	Default,
	Fallback
};

enum class LibassFontNameMatch {
	Family,
	ExtendedFamily,
	FullName,
	PostScript,
	FullNameAndPostScript
};

struct LibassFontCandidateMatch {
	size_t face = 0;
	std::string matched_name;
	LibassFontMatchSource source = LibassFontMatchSource::Requested;
	LibassFontNameMatch name_match = LibassFontNameMatch::Family;
	int score = 0;
	std::vector<uint32_t> considered_codepoints;
	std::vector<uint32_t> supported_codepoints;
	std::vector<uint32_t> selected_codepoints;
};

struct LibassFontSelection {
	std::vector<size_t> faces;
	std::vector<LibassCodepointMatch> codepoints;
	std::vector<uint32_t> missing_codepoints;
	std::vector<LibassFontCandidateMatch> candidates;
	bool ambiguous = false;
};

struct LibassRankedFamilyFace {
	size_t face = 0;
	std::string matched_name;
	LibassFontNameMatch name_match = LibassFontNameMatch::Family;
	int score = 0;
};

struct LibassFamilyFaceRanking {
	std::optional<size_t> face;
	std::vector<LibassRankedFamilyFace> candidates;
	bool ambiguous = false;
	bool used_substitution = false;
};

using LibassGlyphChecker = std::function<bool(size_t face_index, uint32_t codepoint)>;
using LibassFamilySubstituter = std::function<std::vector<std::string>(std::string_view family)>;
using LibassFallbackResolver =
	std::function<std::optional<std::string>(std::string_view family, uint32_t codepoint)>;

/// Catalog + glyph + substitution surface used by LibassFontFileLister.
class ILibassFontProvider {
public:
	virtual ~ILibassFontProvider() = default;
	virtual std::span<LibassFontFace const> GetLibassFaces() const = 0;
	virtual bool HasLibassGlyph(size_t face_index, uint32_t codepoint) const = 0;
	virtual std::vector<std::string> GetLibassSubstitutions(std::string_view family) const = 0;
	virtual std::optional<std::string> GetLibassFallback(std::string_view family, uint32_t codepoint) = 0;
	virtual std::string_view GetLibassProviderName() const = 0;
	virtual std::shared_ptr<std::vector<char> const> GetLibassFontData(size_t face_index) const { return {}; }
};

int LibassFontAttributesSimilarity(LibassFontFace const& face, LibassFontRequest const& request);

/// Rank one requested family without glyph fallback. This models libass's
/// family/name and attribute stages for variant diagnostics; it is not runtime
/// renderer introspection and therefore supplies algorithmic evidence only.
LibassFamilyFaceRanking RankLibassFamilyFaces(
	std::span<LibassFontFace const> faces,
	LibassFontRequest const& request,
	LibassFamilySubstituter const& substitute_family = {});

LibassFontSelection SelectLibassFontFaces(
	std::span<LibassFontFace const> faces,
	LibassFontRequest const& request,
	std::span<uint32_t const> codepoints,
	LibassGlyphChecker const& has_glyph,
	LibassFamilySubstituter const& substitute_family = {},
	LibassFallbackResolver const& resolve_fallback = {},
	bool collect_candidates = false);
