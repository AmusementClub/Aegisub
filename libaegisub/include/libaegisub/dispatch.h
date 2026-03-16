// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include <functional>
#include <memory>

namespace agi {
	namespace dispatch {
		using Thunk = std::function<void()>;

		class Executor {
			virtual void DoPost(Thunk thunk)=0;
		protected:
			virtual void DoDispatch(Thunk thunk);
		public:
			virtual ~Executor() { }

			/// Post the thunk to this executor and return immediately
			void Post(Thunk thunk);

			/// Run the thunk on this executor and return only when it completes
			void Dispatch(Thunk thunk);
		};

		class Queue : public Executor {
		public:
			virtual ~Queue() { }

			/// Invoke the thunk on this processing queue, returning immediately
			void Async(Thunk thunk);

			/// Invoke the thunk on this processing queue, returning only when
			/// it's complete
			void Sync(Thunk thunk);
		};

		/// Initialize the dispatch thread pools
		/// @param invoke_main A function which invokes the thunk on the GUI thread
		void Init(std::function<void (Thunk)> invoke_main);

		/// Get the main-thread executor
		Executor& MainExecutor();

		/// Get the generic background executor
		Executor& BackgroundExecutor();

		/// Create a new serial executor
		std::unique_ptr<Executor> CreateExecutor();

		/// Get the main queue, which runs on the GUI thread
		Queue& Main();

		/// Get the generic background queue, which runs thunks in parallel
		Queue& Background();

		/// Create a new serial queue
		std::unique_ptr<Queue> Create();
	}
}
