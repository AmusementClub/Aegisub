// Font selection behavior derived from libass (ISC License):
//   https://github.com/libass/libass/blob/f9fd3d20dff1cd84b7c74c8ae7f79711ad7736fa/libass/ass_fontselect.c

#include "font_matching_libass.h"

#include "font_matching_common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace {
bool ascii_iequals(std::string_view left, std::string_view right) {
	if (left.size() != right.size())
		return false;

	for (size_t i = 0; i < left.size(); ++i) {
		auto l = static_cast<unsigned char>(left[i]);
		auto r = static_cast<unsigned char>(right[i]);
		if (l < 0x80)
			l = static_cast<unsigned char>(std::tolower(l));
		if (r < 0x80)
			r = static_cast<unsigned char>(std::tolower(r));
		if (l != r)
			return false;
	}
	return true;
}

std::optional<LibassFontNameMatch> match_font_name(
	LibassFontFace const& face,
	std::string_view family,
	bool match_extended_family) {
	for (auto const& candidate : face.families) {
		if (ascii_iequals(candidate, family))
			return LibassFontNameMatch::Family;
	}
	if (match_extended_family && !face.extended_family.empty() &&
	    ascii_iequals(face.extended_family, family))
		return LibassFontNameMatch::ExtendedFamily;

	bool fullname_match = false;
	for (auto const& fullname : face.fullnames) {
		if (ascii_iequals(fullname, family)) {
			fullname_match = true;
			break;
		}
	}

	bool const postscript_match = !face.postscript_name.empty() &&
	                              ascii_iequals(face.postscript_name, family);
	if (fullname_match && postscript_match)
		return LibassFontNameMatch::FullNameAndPostScript;
	if (face.postscript_outlines && postscript_match)
		return LibassFontNameMatch::PostScript;
	if (!face.postscript_outlines && fullname_match)
		return LibassFontNameMatch::FullName;
	return std::nullopt;
}

std::vector<std::string> requested_names(
	std::string_view family,
	LibassFamilySubstituter const& substitute_family) {
	if (substitute_family) {
		auto substitutions = substitute_family(family);
		if (!substitutions.empty())
			return substitutions;
	}
	return {std::string(family)};
}

void append_unique(std::vector<uint32_t>& values, uint32_t value) {
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
}

using CandidateIndex = std::unordered_multimap<size_t, size_t>;

void record_candidate(
	LibassFontSelection& result,
	CandidateIndex& index,
	size_t face,
	std::string_view matched_name,
	LibassFontMatchSource source,
	LibassFontNameMatch name_match,
	int score,
	uint32_t codepoint,
	bool supported,
	bool selected) {
	auto candidate_index = result.candidates.size();
	auto const [begin, end] = index.equal_range(face);
	for (auto it = begin; it != end; ++it) {
		auto const& item = result.candidates[it->second];
		if (item.matched_name == matched_name && item.source == source &&
		    item.name_match == name_match && item.score == score) {
			candidate_index = it->second;
			break;
		}
	}
	if (candidate_index == result.candidates.size()) {
		LibassFontCandidateMatch item;
		item.face = face;
		item.matched_name = matched_name;
		item.source = source;
		item.name_match = name_match;
		item.score = score;
		result.candidates.push_back(std::move(item));
		candidate_index = result.candidates.size() - 1;
		index.emplace(face, candidate_index);
	}
	auto& candidate = result.candidates[candidate_index];

	append_unique(candidate.considered_codepoints, codepoint);
	if (supported)
		append_unique(candidate.supported_codepoints, codepoint);
	if (selected)
		append_unique(candidate.selected_codepoints, codepoint);
}

std::optional<size_t> find_font(
	std::span<LibassFontFace const> faces,
	std::span<std::string const> names,
	LibassFontRequest const& request,
	uint32_t codepoint,
	bool match_extended_family,
	LibassGlyphChecker const& has_glyph,
	LibassFontMatchSource source,
	LibassFontSelection& result,
	CandidateIndex& candidate_index,
	bool collect_candidates) {
	for (auto const& name : names) {
		int minimum_score = std::numeric_limits<int>::max();
		std::optional<size_t> selected;
		LibassFontNameMatch selected_name_match = LibassFontNameMatch::Family;
		size_t equal_score_count = 0;

		for (size_t i = 0; i < faces.size(); ++i) {
			auto const& face = faces[i];
			auto name_match = match_font_name(face, name, match_extended_family);
			if (!name_match)
				continue;

			int score = *name_match == LibassFontNameMatch::Family ||
			            *name_match == LibassFontNameMatch::ExtendedFamily
				? LibassFontAttributesSimilarity(face, request)
				: 0;
			if (!collect_candidates) {
				if (score < minimum_score) {
					if (has_glyph && !has_glyph(i, codepoint))
						continue;
					minimum_score = score;
					selected = i;
				}
				if (score == 0)
					break;
				continue;
			}

			bool const supported = !has_glyph || has_glyph(i, codepoint);
			record_candidate(
				result, candidate_index, i, name, source, *name_match, score, codepoint, supported, false);

			if (supported && score < minimum_score) {
				minimum_score = score;
				selected = i;
				selected_name_match = *name_match;
				equal_score_count = 1;
			}
			else if (supported && score == minimum_score) {
				++equal_score_count;
			}
		}

		if (selected && collect_candidates) {
			record_candidate(
				result,
				candidate_index,
				*selected,
				name,
				source,
				selected_name_match,
				minimum_score,
				codepoint,
				true,
				true);
			if (equal_score_count > 1)
				result.ambiguous = true;
		}
		if (selected)
			return selected;
	}
	return std::nullopt;
}

void append_unique(std::vector<size_t>& values, size_t value) {
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
}
}

int LibassFontAttributesSimilarity(LibassFontFace const& face, LibassFontRequest const& request) {
	FontMatchFaceAttributes attributes;
	attributes.weight = face.weight;
	attributes.bold = face.bold;
	attributes.italic = face.italic;

	FontMatchRequest normalized_request;
	normalized_request.requested_weight = request.weight;
	normalized_request.requested_italic = request.italic;
	return FontAttributesSimilarity(attributes, normalized_request);
}

LibassFontSelection SelectLibassFontFaces(
	std::span<LibassFontFace const> faces,
	LibassFontRequest const& request,
	std::span<uint32_t const> codepoints,
	LibassGlyphChecker const& has_glyph,
	LibassFamilySubstituter const& substitute_family,
	LibassFallbackResolver const& resolve_fallback,
	bool collect_candidates) {
	LibassFontSelection result;
	result.codepoints.reserve(codepoints.size());
	CandidateIndex candidate_index;

	for (auto codepoint : codepoints) {
		LibassCodepointMatch codepoint_match;
		codepoint_match.codepoint = codepoint;

		if (!request.family.empty()) {
			auto names = requested_names(request.family, substitute_family);
			codepoint_match.face = find_font(
				faces, names, request, codepoint, false, has_glyph,
				LibassFontMatchSource::Requested, result, candidate_index, collect_candidates);
		}

		if (!codepoint_match.face && !request.default_family.empty()) {
			auto names = requested_names(request.default_family, substitute_family);
			codepoint_match.face = find_font(
				faces, names, request, codepoint, false, has_glyph,
				LibassFontMatchSource::Default, result, candidate_index, collect_candidates);
		}

		if (!codepoint_match.face && resolve_fallback) {
			auto fallback = resolve_fallback(
				request.family.empty() ? std::string_view("Arial") : std::string_view(request.family),
				codepoint);
			if (fallback) {
				auto names = requested_names(*fallback, substitute_family);
				codepoint_match.face = find_font(
					faces, names, request, codepoint, true, has_glyph,
					LibassFontMatchSource::Fallback, result, candidate_index, collect_candidates);
				codepoint_match.fallback = codepoint_match.face.has_value();
			}
		}

		if (codepoint_match.face)
			append_unique(result.faces, *codepoint_match.face);
		else
			result.missing_codepoints.push_back(codepoint);

		result.codepoints.push_back(std::move(codepoint_match));
	}

	return result;
}
