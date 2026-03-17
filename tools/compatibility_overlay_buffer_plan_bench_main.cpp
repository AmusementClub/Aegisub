#include "../src/compatibility_overlay_buffer_plan.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

struct LegacyBufferState {
	bool reusable = false;
};

struct BenchCase {
	std::string name;
	int preferred_slot = 0;
	std::array<CompatibilityOverlayBufferSlotState, 2> plan_slots = { };
	std::vector<LegacyBufferState> legacy_pool;
};

std::uint64_t legacy_select(std::vector<LegacyBufferState> const& buffers) {
	for (size_t i = 0; i < buffers.size(); ++i) {
		if (buffers[i].reusable)
			return static_cast<std::uint64_t>(i);
	}
	return static_cast<std::uint64_t>(buffers.size());
}

std::uint64_t new_select(int preferred_slot, std::array<CompatibilityOverlayBufferSlotState, 2> const& slots) {
	auto plan = DecideCompatibilityOverlayBufferPlan(preferred_slot, slots);
	switch (plan.action) {
	case CompatibilityOverlayBufferPlanAction::UseSlot0:
		return 0;
	case CompatibilityOverlayBufferPlanAction::UseSlot1:
		return 1;
	case CompatibilityOverlayBufferPlanAction::UseOverflowPool:
	default:
		return 2;
	}
}

template<typename Func>
double bench_ns_per_op(Func&& func) {
	volatile std::uint64_t sink = 0;
	for (int i = 0; i < 16; ++i)
		sink += func();

	auto start = clock_type::now();
	for (int i = 0; i < 500000; ++i)
		sink += func();
	auto end = clock_type::now();

	(void)sink;
	return std::chrono::duration<double, std::nano>(end - start).count() / 500000.0;
}
}

int main() {
	BenchCase common_ping_pong;
	common_ping_pong.name = "common_ping_pong";
	common_ping_pong.preferred_slot = 0;
	common_ping_pong.plan_slots[0] = { true, false, true };
	common_ping_pong.plan_slots[1] = { true, true, false };
	common_ping_pong.legacy_pool = {
		{ false },
		{ true }
	};

	BenchCase saturated_pool;
	saturated_pool.name = "saturated_pool_scan";
	saturated_pool.preferred_slot = 1;
	saturated_pool.plan_slots[0] = { true, false, false };
	saturated_pool.plan_slots[1] = { true, false, true };
	saturated_pool.legacy_pool = {
		{ false }, { false }, { false }, { false },
		{ false }, { false }, { false }, { false }
	};

	BenchCase cold_start;
	cold_start.name = "cold_start";
	cold_start.preferred_slot = 0;
	cold_start.plan_slots[0] = { false, false, false };
	cold_start.plan_slots[1] = { false, false, false };
	cold_start.legacy_pool = { };

	std::cout << "Compatibility overlay buffer plan benchmark\n";
	std::cout << std::left << std::setw(22) << "scenario"
		<< std::right << std::setw(14) << "legacy ns"
		<< std::setw(14) << "new ns"
		<< std::setw(16) << "legacy choice"
		<< std::setw(16) << "new choice"
		<< "\n";

	for (auto const* bench_case : { &common_ping_pong, &saturated_pool, &cold_start }) {
		double legacy_ns = bench_ns_per_op([&] {
			return legacy_select(bench_case->legacy_pool);
		});
		double current_ns = bench_ns_per_op([&] {
			return new_select(bench_case->preferred_slot, bench_case->plan_slots);
		});

		std::cout << std::left << std::setw(22) << bench_case->name
			<< std::right << std::setw(14) << std::fixed << std::setprecision(2) << legacy_ns
			<< std::setw(14) << std::fixed << std::setprecision(2) << current_ns
			<< std::setw(16) << legacy_select(bench_case->legacy_pool)
			<< std::setw(16) << new_select(bench_case->preferred_slot, bench_case->plan_slots)
			<< "\n";
	}

	return 0;
}
