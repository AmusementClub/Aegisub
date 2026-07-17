#include <gtest/gtest.h>

#include "../../src/font_family_catalog_cache.h"
#include "../../src/font_family_catalog_cache_test.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

FontFamilyCatalog catalog_with_id(FontFamilyId id) {
	FontFamilyRecord record;
	record.id = id;
	record.localized_family_name = "Family " + std::to_string(id);
	return FontFamilyCatalog({std::move(record)});
}

struct ControlledBuilders {
	std::atomic<int> calls = 0;
	std::array<std::promise<void>, 3> started_promises;
	std::array<std::shared_future<void>, 3> started;
	std::array<std::promise<void>, 3> release_promises;
	std::array<std::shared_future<void>, 3> release;
	std::array<std::promise<void>, 3> completed_promises;
	std::array<std::shared_future<void>, 3> completed;
	std::array<bool, 3> released{};

	ControlledBuilders() {
		for (std::size_t i = 0; i < started.size(); ++i) {
			started[i] = started_promises[i].get_future().share();
			release[i] = release_promises[i].get_future().share();
			completed[i] = completed_promises[i].get_future().share();
		}
	}

	~ControlledBuilders() {
		for (std::size_t i = 0; i < released.size(); ++i) {
			if (!released[i])
				release_promises[i].set_value();
		}
	}

	FontFamilyCatalog Build() {
		auto index = calls.fetch_add(1);
		if (index >= static_cast<int>(started.size()))
			throw std::runtime_error("unexpected catalog build");
		started_promises[index].set_value();
		release[index].wait();
		auto catalog = catalog_with_id(static_cast<FontFamilyId>(index + 1));
		completed_promises[index].set_value();
		return catalog;
	}

	void Release(std::size_t index) {
		released[index] = true;
		release_promises[index].set_value();
	}
};

class font_family_catalog_cache_test : public ::testing::Test {
protected:
	void TearDown() override {
		font_family_catalog_cache::testing::RestoreDefaultBuilder();
	}
};

} // namespace

TEST_F(font_family_catalog_cache_test, invalidate_does_not_wait_for_warm_async_build) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[0].wait_for(5s));
	auto before = std::chrono::steady_clock::now();
	font_family_catalog_cache::Invalidate();
	auto elapsed = std::chrono::steady_clock::now() - before;
	EXPECT_LT(elapsed, 1s);

	controlled.Release(0);
	ASSERT_EQ(std::future_status::ready, controlled.completed[0].wait_for(5s));
}

TEST_F(font_family_catalog_cache_test, late_old_build_cannot_replace_new_generation) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[0].wait_for(5s));
	font_family_catalog_cache::Invalidate();
	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[1].wait_for(5s));

	controlled.Release(1);
	auto current = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, current);
	ASSERT_NE(nullptr, current->Find(2));

	controlled.Release(0);
	ASSERT_EQ(std::future_status::ready, controlled.completed[0].wait_for(5s));
	auto after_old_completion = font_family_catalog_cache::GetSnapshot();
	EXPECT_EQ(current, after_old_completion);
	EXPECT_EQ(nullptr, after_old_completion->Find(1));
}

TEST_F(font_family_catalog_cache_test, consecutive_invalidations_publish_only_latest_build) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	for (std::size_t i = 0; i < 3; ++i) {
		font_family_catalog_cache::WarmAsync();
		ASSERT_EQ(std::future_status::ready, controlled.started[i].wait_for(5s));
		if (i != 2)
			font_family_catalog_cache::Invalidate();
	}

	controlled.Release(0);
	controlled.Release(1);
	controlled.Release(2);
	auto current = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, current);
	EXPECT_NE(nullptr, current->Find(3));
	EXPECT_EQ(nullptr, current->Find(1));
	EXPECT_EQ(nullptr, current->Find(2));
}
