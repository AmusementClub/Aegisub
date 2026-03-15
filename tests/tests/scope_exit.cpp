#include <gtest/gtest.h>

#include <libaegisub/scope_exit.h>

#include <stdexcept>

TEST(lagi_scope_exit, runs_on_scope_exit) {
	int value = 0;
	{
		auto guard = agi::make_scope_exit([&] { value = 42; });
		(void)guard;
	}
	EXPECT_EQ(42, value);
}

TEST(lagi_scope_exit, release_prevents_execution) {
	int value = 0;
	{
		auto guard = agi::make_scope_exit([&] { value = 42; });
		guard.release();
	}
	EXPECT_EQ(0, value);
}

TEST(lagi_scope_exit, move_transfers_ownership) {
	int value = 0;
	{
		auto guard = agi::make_scope_exit([&] { ++value; });
		auto moved = std::move(guard);
		(void)moved;
	}
	EXPECT_EQ(1, value);
}

TEST(lagi_scope_exit, runs_during_exception_unwind) {
	int value = 0;
	try {
		auto guard = agi::make_scope_exit([&] { value = 7; });
		(void)guard;
		throw std::runtime_error("boom");
	}
	catch (std::runtime_error const&) {
	}
	EXPECT_EQ(7, value);
}
