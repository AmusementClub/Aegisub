// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <gtest/gtest.h>

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <boost/locale/generator.hpp>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

namespace {
std::mutex main_queue_mutex;
std::deque<agi::dispatch::Thunk> main_queue;
std::thread::id main_thread_id;

std::size_t FlushMainQueue() {
	std::size_t executed = 0;
	while (true) {
		agi::dispatch::Thunk thunk;
		{
			std::lock_guard<std::mutex> lock(main_queue_mutex);
			if (main_queue.empty())
				return executed;

			thunk = std::move(main_queue.front());
			main_queue.pop_front();
		}

		++executed;
		thunk();
	}
}
}

int main(int argc, char **argv) {
	main_thread_id = std::this_thread::get_id();
	agi::dispatch::Init([](agi::dispatch::Thunk f) {
		std::lock_guard<std::mutex> lock(main_queue_mutex);
		main_queue.emplace_back(std::move(f));
	}, [] {
		return std::this_thread::get_id() == main_thread_id;
	}, [] {
		return FlushMainQueue();
	});
	std::locale::global(boost::locale::generator().generate(""));

	int retval;
	agi::log::log = new agi::log::LogSink;
	agi::log::log->Subscribe(agi::make_unique<agi::log::JsonEmitter>("./"));
	::testing::InitGoogleTest(&argc, argv);

	srand(time(nullptr));

	retval = RUN_ALL_TESTS();

	delete agi::log::log;

	return retval;
}

