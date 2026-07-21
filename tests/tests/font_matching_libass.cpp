#include "../../src/font_matching_libass.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {
LibassFontFace face(
	std::string family,
	int weight,
	bool italic,
	std::string path,
	std::string fullname = {}) {
	LibassFontFace result;
	result.families.push_back(std::move(family));
	result.fullnames.push_back(fullname.empty() ? result.families.front() : std::move(fullname));
	result.path = std::move(path);
	result.weight = weight;
	result.bold = weight >= 600;
	result.italic = italic;
	return result;
}
}

TEST(font_matching_libass, uses_libass_weight_and_italic_score) {
	auto regular = face("Example", 400, false, "regular.ttf");
	auto bold = face("Example", 700, false, "bold.ttf");
	auto italic = face("Example", 400, true, "italic.ttf");

	LibassFontRequest request{"Example", 700, false, {}};
	EXPECT_EQ(0, LibassFontAttributesSimilarity(bold, request));
	EXPECT_LT(LibassFontAttributesSimilarity(regular, request),
	          LibassFontAttributesSimilarity(italic, request));
}

TEST(font_matching_libass, checks_glyph_before_committing_best_face) {
	auto narrow = face("Example", 700, false, "bold-no-cjk.ttf");
	auto fallback = face("Example", 400, false, "regular-cjk.ttf");
	std::vector<LibassFontFace> faces{narrow, fallback};

	LibassFontRequest request{"Example", 700, false, {}};
	uint32_t const codepoint = 0x4E00;
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[&](size_t index, uint32_t value) { return index == 1 && value == codepoint; });

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(1u, selection.faces.front());
	ASSERT_EQ(1u, selection.codepoints.size());
	EXPECT_EQ(1u, selection.codepoints.front().face.value());
	EXPECT_TRUE(selection.missing_codepoints.empty());
}

TEST(font_matching_libass, missing_zero_score_face_does_not_hide_later_match) {
	auto family = face("Example Regular", 700, false, "family.ttf");
	auto missing = face("Other", 400, false, "missing.ttf", "Example Regular");
	auto exact = face("Another", 400, false, "exact.ttf", "Example Regular");
	std::vector<LibassFontFace> faces{family, missing, exact};

	LibassFontRequest request{"Example Regular", 400, false, {}};
	uint32_t const codepoint = 'A';
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[](size_t index, uint32_t) { return index != 1; });

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(2u, selection.faces.front());
}

TEST(font_matching_libass, full_name_match_has_zero_score) {
	auto family = face("Example", 400, false, "family.ttf", "Example Regular");
	auto exact = face("Other", 400, false, "exact.ttf", "Example Regular");
	exact.postscript_name = "Example-Regular";
	std::vector<LibassFontFace> faces{family, exact};

	LibassFontRequest request{"Example Regular", 400, false, {}};
	uint32_t const codepoint = 'A';
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[](size_t, uint32_t) { return true; });

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(0u, selection.faces.front());
}

TEST(font_matching_libass, fallback_family_is_selected_per_codepoint) {
	auto primary = face("Example", 400, false, "primary.ttf");
	auto fallback = face("Fallback", 400, false, "fallback.ttf");
	fallback.extended_family = "Fallback";
	std::vector<LibassFontFace> faces{primary, fallback};

	LibassFontRequest request{"Example", 400, false, {}};
	uint32_t const codepoint = 0x4E00;
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[&](size_t index, uint32_t value) { return index == 1 && value == codepoint; },
		{},
		[](std::string_view family, uint32_t value) -> std::optional<std::string> {
			if (family == "Example" && value == 0x4E00)
				return "Fallback";
			return std::nullopt;
		});

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(1u, selection.faces.front());
	EXPECT_TRUE(selection.codepoints.front().fallback);
}

TEST(font_matching_libass, substitutions_are_tried_in_provider_order) {
	auto first = face("First Alias", 400, false, "first.ttf");
	auto preferred = face("Preferred Alias", 400, false, "preferred.ttf");
	std::vector<LibassFontFace> faces{first, preferred};

	LibassFontRequest request{"Requested", 400, false, {}};
	uint32_t const codepoint = 'A';
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[](size_t, uint32_t) { return true; },
		[](std::string_view) {
			return std::vector<std::string>{"Preferred Alias", "First Alias"};
		});

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(1u, selection.faces.front());
}

TEST(font_matching_libass, reports_ranked_candidates_without_changing_selection) {
	auto regular = face("Example", 400, false, "regular.ttf");
	auto semibold = face("Example", 600, false, "semibold.ttf");
	auto bold = face("Example", 700, false, "bold.ttf");
	std::vector<LibassFontFace> faces{regular, semibold, bold};

	LibassFontRequest request{"Example", 700, false, {}};
	uint32_t const codepoint = 'A';
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[](size_t, uint32_t) { return true; },
		{},
		{},
		true);

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(2u, selection.faces.front());
	ASSERT_EQ(3u, selection.candidates.size());
	EXPECT_GT(selection.candidates[0].score, selection.candidates[2].score);
	EXPECT_TRUE(selection.candidates[0].selected_codepoints.empty());
	EXPECT_EQ(std::vector<uint32_t>{codepoint}, selection.candidates[2].selected_codepoints);
	EXPECT_FALSE(selection.ambiguous);
}

TEST(font_matching_libass, reports_equal_score_provider_order_as_ambiguous) {
	auto first = face("Example", 400, false, "first.ttf");
	auto second = face("Example", 400, false, "second.ttf");
	std::vector<LibassFontFace> faces{first, second};

	LibassFontRequest request{"Example", 400, false, {}};
	uint32_t const codepoint = 'A';
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[](size_t, uint32_t) { return true; },
		{},
		{},
		true);

	ASSERT_EQ(1u, selection.faces.size());
	EXPECT_EQ(0u, selection.faces.front());
	ASSERT_EQ(2u, selection.candidates.size());
	EXPECT_EQ(selection.candidates[0].score, selection.candidates[1].score);
	EXPECT_TRUE(selection.ambiguous);
}

TEST(font_matching_libass, candidate_evidence_distinguishes_glyph_rejection) {
	auto preferred = face("Example", 700, false, "preferred.ttf");
	auto covering = face("Example", 400, false, "covering.ttf");
	std::vector<LibassFontFace> faces{preferred, covering};

	LibassFontRequest request{"Example", 700, false, {}};
	uint32_t const codepoint = 0x4E00;
	auto selection = SelectLibassFontFaces(
		faces,
		request,
		std::span<uint32_t const>(&codepoint, 1),
		[&](size_t index, uint32_t value) { return index == 1 && value == codepoint; },
		{},
		{},
		true);

	ASSERT_EQ(2u, selection.candidates.size());
	EXPECT_TRUE(selection.candidates[0].supported_codepoints.empty());
	EXPECT_TRUE(selection.candidates[0].selected_codepoints.empty());
	EXPECT_EQ(std::vector<uint32_t>{codepoint}, selection.candidates[1].supported_codepoints);
	EXPECT_EQ(std::vector<uint32_t>{codepoint}, selection.candidates[1].selected_codepoints);
	EXPECT_FALSE(selection.ambiguous);
}

TEST(font_matching_libass_family_ranking, selects_best_attributes_in_provider_order) {
	auto regular = face("Example", 400, false, "regular.ttf");
	auto bold = face("Example", 700, false, "bold.ttf");
	std::vector<LibassFontFace> faces{regular, bold};
	LibassFontRequest request{"Example", 700, false, {}};

	auto ranking = RankLibassFamilyFaces(faces, request);
	ASSERT_TRUE(ranking.face.has_value());
	EXPECT_EQ(1u, *ranking.face);
	EXPECT_FALSE(ranking.ambiguous);
	EXPECT_FALSE(ranking.used_substitution);
	ASSERT_EQ(2u, ranking.candidates.size());
	EXPECT_EQ(0u, ranking.candidates.front().face);
	EXPECT_EQ(1u, ranking.candidates.back().face);
}

TEST(font_matching_libass_family_ranking, reports_equal_best_faces_as_ambiguous) {
	auto first = face("Example", 400, false, "first.ttf");
	auto second = face("Example", 400, false, "second.ttf");
	std::vector<LibassFontFace> faces{first, second};
	LibassFontRequest request{"Example", 400, false, {}};

	auto ranking = RankLibassFamilyFaces(faces, request);
	ASSERT_TRUE(ranking.face.has_value());
	EXPECT_EQ(0u, *ranking.face);
	EXPECT_TRUE(ranking.ambiguous);
	EXPECT_FALSE(ranking.used_substitution);
}

TEST(font_matching_libass_family_ranking, marks_only_the_selected_alias_as_substitution) {
	auto alias = face("Alias", 400, false, "alias.ttf");
	auto unrelated = face("Other", 400, false, "other.ttf");
	std::vector<LibassFontFace> faces{alias, unrelated};
	LibassFontRequest request{"Requested", 400, false, {}};

	auto ranking = RankLibassFamilyFaces(
		faces, request, [](std::string_view) {
			return std::vector<std::string>{"Alias"};
		});
	ASSERT_TRUE(ranking.face.has_value());
	EXPECT_EQ(0u, *ranking.face);
	EXPECT_TRUE(ranking.used_substitution);
	ASSERT_EQ(1u, ranking.candidates.size());
	EXPECT_EQ("Alias", ranking.candidates.front().matched_name);
}
