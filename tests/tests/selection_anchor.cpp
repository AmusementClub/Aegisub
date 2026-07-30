#include <main.h>

#include "../../src/selection_anchor.h"

#include <optional>

namespace anchor = aegisub::selection_anchor;

TEST(selection_anchor, tracks_identity_and_current_row) {
	anchor::Anchor subject;
	subject.Set(42, 4);

	auto snapshot = subject.Refresh([](int line_id) -> std::optional<int> {
		EXPECT_EQ(42, line_id);
		return 8;
	});

	ASSERT_TRUE(snapshot);
	EXPECT_EQ(42, snapshot->line_id);
	EXPECT_EQ(8, snapshot->row);
	EXPECT_TRUE(snapshot->available);
}

TEST(selection_anchor, preserves_last_known_row_while_missing_and_recovers) {
	anchor::Anchor subject;
	subject.Set(42, 4);
	ASSERT_TRUE(subject.Refresh([](int) -> std::optional<int> { return 8; }));

	auto missing = subject.Refresh([](int) -> std::optional<int> { return std::nullopt; });
	ASSERT_TRUE(missing);
	EXPECT_EQ(8, missing->row);
	EXPECT_FALSE(missing->available);
	EXPECT_TRUE(subject.IsSet());

	auto restored = subject.Refresh([](int) -> std::optional<int> { return 2; });
	ASSERT_TRUE(restored);
	EXPECT_EQ(2, restored->row);
	EXPECT_TRUE(restored->available);
}

TEST(selection_anchor, clear_removes_the_anchor) {
	anchor::Anchor subject;
	subject.Set(42, 4);
	subject.Clear();

	EXPECT_FALSE(subject.IsSet());
	EXPECT_FALSE(subject.Peek([](int) -> std::optional<int> { return 4; }));
}
