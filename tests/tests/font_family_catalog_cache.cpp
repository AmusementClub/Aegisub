#include <gtest/gtest.h>

#include "../../src/font_family_catalog_cache.h"
#include "../../src/font_family_catalog_cache_test.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
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

class IdentifiedSource final : public IFontFamilyCatalogSource {
	FontFamilyCatalogSourceInfo info;
	FontFamilyId id;
	std::atomic<int> builds = 0;

public:
	IdentifiedSource(FontFamilyCatalogSourceInfo info, FontFamilyId id)
	: info(std::move(info))
	, id(id)
	{}

	FontFamilyCatalogSourceInfo const& Info() const noexcept override {
		return info;
	}

	FontFamilyCatalog Build() override {
		builds.fetch_add(1, std::memory_order_relaxed);
		return catalog_with_id(id);
	}

	int build_count() const noexcept {
		return builds.load(std::memory_order_relaxed);
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

TEST_F(font_family_catalog_cache_test, ready_snapshot_never_waits_for_an_inflight_build) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	EXPECT_EQ(nullptr, font_family_catalog_cache::GetReadySnapshot());
	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[0].wait_for(5s));

	auto const before = std::chrono::steady_clock::now();
	EXPECT_EQ(nullptr, font_family_catalog_cache::GetReadySnapshot());
	EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);

	controlled.Release(0);
	auto const published = font_family_catalog_cache::GetSnapshot();
	EXPECT_EQ(published, font_family_catalog_cache::GetReadySnapshot());
}

TEST_F(font_family_catalog_cache_test, nonblocking_warm_path_retries_a_completed_failure) {
	std::atomic<int> calls = 0;
	std::promise<void> first_called;
	auto const first_called_future = first_called.get_future();
	font_family_catalog_cache::testing::SetBuilder([&] {
		if (calls.fetch_add(1) == 0) {
			first_called.set_value();
			throw std::runtime_error("catalog build failed");
		}
		return catalog_with_id(2);
	});

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, first_called_future.wait_for(5s));
	auto const deadline = std::chrono::steady_clock::now() + 5s;
	while (calls.load() < 2 && std::chrono::steady_clock::now() < deadline) {
		font_family_catalog_cache::WarmAsync();
		std::this_thread::sleep_for(1ms);
	}

	ASSERT_EQ(2, calls.load());
	auto const snapshot = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, snapshot);
	EXPECT_NE(nullptr, snapshot->Find(2));
}

TEST_F(font_family_catalog_cache_test, waiter_retries_when_its_generation_is_invalidated) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[0].wait_for(5s));
	auto waiter = std::async(std::launch::async, [] {
		return font_family_catalog_cache::GetSnapshot();
	});

	font_family_catalog_cache::Invalidate();
	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[1].wait_for(5s));

	controlled.Release(0);
	EXPECT_EQ(std::future_status::timeout, waiter.wait_for(100ms));
	controlled.Release(1);

	ASSERT_EQ(std::future_status::ready, waiter.wait_for(5s));
	auto current = waiter.get();
	ASSERT_NE(nullptr, current);
	EXPECT_NE(nullptr, current->Find(2));
	EXPECT_EQ(nullptr, current->Find(1));
	ASSERT_EQ(std::future_status::ready, controlled.completed[0].wait_for(5s));
	ASSERT_EQ(std::future_status::ready, controlled.completed[1].wait_for(5s));
}

TEST_F(font_family_catalog_cache_test, failed_build_does_not_poison_later_callers) {
	std::atomic<int> calls = 0;
	font_family_catalog_cache::testing::SetBuilder([&] {
		if (calls.fetch_add(1) == 0)
			throw std::runtime_error("catalog build failed");
		return catalog_with_id(2);
	});

	EXPECT_THROW(font_family_catalog_cache::GetSnapshot(), std::runtime_error);
	auto recovered = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, recovered);
	EXPECT_NE(nullptr, recovered->Find(2));
	EXPECT_EQ(2, calls.load());
}

TEST_F(font_family_catalog_cache_test, best_effort_snapshot_returns_null_then_retries) {
	std::atomic<int> calls = 0;
	font_family_catalog_cache::testing::SetBuilder([&] {
		if (calls.fetch_add(1) == 0)
			throw std::runtime_error("catalog build failed");
		return catalog_with_id(2);
	});

	EXPECT_EQ(nullptr, font_family_catalog_cache::TryGetSnapshot());
	auto recovered = font_family_catalog_cache::TryGetSnapshot();
	ASSERT_NE(nullptr, recovered);
	EXPECT_NE(nullptr, recovered->Find(2));
	EXPECT_EQ(2, calls.load());
}

TEST_F(font_family_catalog_cache_test, stale_failed_build_is_ignored_after_invalidation) {
	std::promise<void> old_started;
	auto old_started_future = old_started.get_future().share();
	std::promise<void> release_old;
	auto old_release_future = release_old.get_future().share();
	std::atomic<int> calls = 0;
	font_family_catalog_cache::testing::SetBuilder([&] {
		if (calls.fetch_add(1) == 0) {
			old_started.set_value();
			old_release_future.wait();
			throw std::runtime_error("stale catalog build failed");
		}
		return catalog_with_id(2);
	});

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, old_started_future.wait_for(5s));
	auto waiter = std::async(std::launch::async, [] {
		return font_family_catalog_cache::GetSnapshot();
	});

	font_family_catalog_cache::Invalidate();
	font_family_catalog_cache::WarmAsync();
	release_old.set_value();

	ASSERT_EQ(std::future_status::ready, waiter.wait_for(5s));
	auto snapshot = waiter.get();
	ASSERT_NE(nullptr, snapshot);
	EXPECT_NE(nullptr, snapshot->Find(2));
	EXPECT_EQ(2, calls.load());
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
	ASSERT_EQ(std::future_status::ready, controlled.completed[0].wait_for(5s));
	ASSERT_EQ(std::future_status::ready, controlled.completed[1].wait_for(5s));
	ASSERT_EQ(std::future_status::ready, controlled.completed[2].wait_for(5s));
}

TEST_F(font_family_catalog_cache_test, shutdown_waits_for_build_and_prevents_restart) {
	ControlledBuilders controlled;
	font_family_catalog_cache::testing::SetBuilder([&] { return controlled.Build(); });

	font_family_catalog_cache::WarmAsync();
	ASSERT_EQ(std::future_status::ready, controlled.started[0].wait_for(5s));

	auto shutdown = std::async(std::launch::async, [] {
		font_family_catalog_cache::Shutdown();
	});
	EXPECT_EQ(std::future_status::timeout, shutdown.wait_for(100ms));

	controlled.Release(0);
	ASSERT_EQ(std::future_status::ready, shutdown.wait_for(5s));
	shutdown.get();

	font_family_catalog_cache::WarmAsync();
	EXPECT_EQ(1, controlled.calls.load());
	EXPECT_THROW(font_family_catalog_cache::GetSnapshot(), std::logic_error);
	EXPECT_THROW(font_family_catalog_cache::Rebuild(), std::logic_error);
}

TEST_F(font_family_catalog_cache_test, shutdown_is_visible_to_builders_before_wait_returns) {
	// Platform builders poll IsShutdownRequested() so they can leave GDI/DWrite
	// before process teardown. The flag must flip as soon as Shutdown starts,
	// not only after outstanding futures complete.
	std::promise<void> saw_shutdown;
	auto saw_shutdown_future = saw_shutdown.get_future();
	std::atomic<bool> promise_set{false};

	font_family_catalog_cache::testing::SetBuilder([&] {
		while (!font_family_catalog_cache::IsShutdownRequested())
			std::this_thread::sleep_for(1ms);
		if (!promise_set.exchange(true))
			saw_shutdown.set_value();
		// Keep the future alive until Shutdown collects it.
		std::this_thread::sleep_for(20ms);
		return FontFamilyCatalog{};
	});

	font_family_catalog_cache::WarmAsync();
	// Give the builder a moment to enter its poll loop.
	std::this_thread::sleep_for(20ms);

	auto shutdown = std::async(std::launch::async, [] {
		font_family_catalog_cache::Shutdown();
	});
	ASSERT_EQ(std::future_status::ready, saw_shutdown_future.wait_for(5s));
	ASSERT_EQ(std::future_status::ready, shutdown.wait_for(5s));
	shutdown.get();
	EXPECT_TRUE(font_family_catalog_cache::IsShutdownRequested());
}

TEST_F(font_family_catalog_cache_test, injected_source_identity_is_not_cache_generation) {
	auto source = std::make_shared<IdentifiedSource>(
		FontFamilyCatalogSourceInfo{
			FontVariantBackend::CoreText, "test-coretext", false},
		17);
	auto const before = font_family_catalog_cache::GetGeneration();
	font_family_catalog_cache::testing::SetSource(source);
	auto const after = font_family_catalog_cache::GetGeneration();

	EXPECT_GT(after, before);
	EXPECT_EQ(
		(FontFamilyCatalogSourceInfo{
			FontVariantBackend::CoreText, "test-coretext", false}),
		font_family_catalog_cache::GetSourceInfo());

	auto snapshot = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, snapshot);
	EXPECT_NE(nullptr, snapshot->Find(17));
	EXPECT_EQ(1, source->build_count());
}

TEST_F(font_family_catalog_cache_test, replacing_source_invalidates_previous_snapshot) {
	auto first = std::make_shared<IdentifiedSource>(
		FontFamilyCatalogSourceInfo{
			FontVariantBackend::CoreText, "first", false},
		21);
	auto second = std::make_shared<IdentifiedSource>(
		FontFamilyCatalogSourceInfo{
			FontVariantBackend::Fontconfig, "second", false},
		22);
	font_family_catalog_cache::testing::SetSource(first);
	auto old_snapshot = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, old_snapshot);
	ASSERT_NE(nullptr, old_snapshot->Find(21));

	font_family_catalog_cache::SetSource(second);
	EXPECT_EQ(
		(FontFamilyCatalogSourceInfo{
			FontVariantBackend::Fontconfig, "second", false}),
		font_family_catalog_cache::GetSourceInfo());
	auto current = font_family_catalog_cache::GetSnapshot();
	ASSERT_NE(nullptr, current);
	EXPECT_NE(nullptr, current->Find(22));
	EXPECT_EQ(nullptr, current->Find(21));
	// Holding an old shared snapshot remains valid, but it is never republished
	// after the source/generation transition.
	EXPECT_NE(nullptr, old_snapshot->Find(21));
}

TEST_F(font_family_catalog_cache_test, null_source_is_rejected) {
	EXPECT_THROW(
		font_family_catalog_cache::SetSource(FontFamilyCatalogSourcePtr{}),
		std::invalid_argument);
}
