#include <main.h>

#include "../../src/coreclr/bridge_error.h"

using agi::coreclr::BridgeErrorCategory;

TEST(plugin_bridge_error, parses_versioned_error_envelope) {
	auto error = agi::coreclr::ParseBridgeErrorEnvelope(R"({
		"schemaVersion":1,
		"code":"sample.file_locked",
		"category":"extension",
		"message":"字幕文件正被占用",
		"details":"System.IO.IOException: diagnostic details",
		"exceptionType":"System.IO.IOException",
		"retryable":true
	})");

	ASSERT_TRUE(error);
	EXPECT_EQ(1, error->schema_version);
	EXPECT_EQ("sample.file_locked", error->code);
	EXPECT_EQ(BridgeErrorCategory::Extension, error->category);
	EXPECT_EQ("字幕文件正被占用", error->message);
	EXPECT_EQ("System.IO.IOException: diagnostic details", error->details);
	EXPECT_EQ("System.IO.IOException", error->exception_type);
	EXPECT_TRUE(error->retryable);
}

TEST(plugin_bridge_error, accepts_optional_diagnostic_fields) {
	auto error = agi::coreclr::ParseBridgeErrorEnvelope(
		R"({"schemaVersion":1,"code":"contract.invalid_result","category":"contract","message":"Invalid result","retryable":false})");

	ASSERT_TRUE(error);
	EXPECT_TRUE(error->details.empty());
	EXPECT_TRUE(error->exception_type.empty());
	EXPECT_EQ("contract", agi::coreclr::ToString(error->category));
}

TEST(plugin_bridge_error, rejects_malformed_or_unknown_envelopes) {
	EXPECT_FALSE(agi::coreclr::ParseBridgeErrorEnvelope("plain exception text"));
	EXPECT_FALSE(agi::coreclr::ParseBridgeErrorEnvelope(
		R"({"schemaVersion":2,"code":"future.error","category":"bridge","message":"Future","retryable":false})"));
	EXPECT_FALSE(agi::coreclr::ParseBridgeErrorEnvelope(
		R"({"schemaVersion":4294967297,"code":"wrapped.version","category":"bridge","message":"Wrapped","retryable":false})"));
	EXPECT_FALSE(agi::coreclr::ParseBridgeErrorEnvelope(
		R"({"schemaVersion":1,"code":"bad.category","category":"other","message":"Unknown","retryable":false})"));
	EXPECT_FALSE(agi::coreclr::ParseBridgeErrorEnvelope(
		R"({"schemaVersion":1,"code":"missing.retryable","category":"bridge","message":"Missing"})"));
}
