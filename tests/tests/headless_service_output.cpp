#include <gtest/gtest.h>

#include "../../src/headless_service_output.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/fs.h>

#include <sstream>

TEST(headless_service_output, trace_values_are_always_valid_json) {
	aegisub::trace_inspect_service::TraceSessionSummary session;
	session.session_dir = agi::fs::PathFromString("trace");
	session.manifest = {
		{"boolean", "true"},
		{"integer", "1"},
		{"fraction", "0.5"},
		{"plus", "+1"},
		{"leading_zero", "01"},
		{"short_fraction", ".5"},
		{"trailing_fraction", "1."},
	};

	auto const serialized = aegisub::headless_service_output::BuildTraceInspectJson(session);
	json::UnknownElement parsed;
	std::istringstream input(serialized);
	ASSERT_NO_THROW(json::Reader::Read(parsed, input));

	auto const& root = static_cast<json::Object const&>(parsed);
	auto const& manifest = static_cast<json::Object const&>(root.at("manifest"));
	EXPECT_EQ(static_cast<json::Boolean const&>(manifest.at("boolean")), true);
	EXPECT_EQ(static_cast<json::Integer const&>(manifest.at("integer")), 1);
	EXPECT_DOUBLE_EQ(static_cast<json::Double const&>(manifest.at("fraction")), 0.5);
	EXPECT_EQ(static_cast<json::String const&>(manifest.at("plus")), "+1");
	EXPECT_EQ(static_cast<json::String const&>(manifest.at("leading_zero")), "01");
	EXPECT_EQ(static_cast<json::String const&>(manifest.at("short_fraction")), ".5");
	EXPECT_EQ(static_cast<json::String const&>(manifest.at("trailing_fraction")), "1.");
}
