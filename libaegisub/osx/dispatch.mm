// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "libaegisub/dispatch.h"

#include <dispatch/dispatch.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using namespace agi::dispatch;
std::function<void (Thunk)> invoke_main;

struct OSXQueue : Queue {
    virtual void DoDispatch(Thunk thunk) override = 0;
};

struct MainQueue final : OSXQueue {
    void DoPost(Thunk thunk) override { invoke_main(std::move(thunk)); }

    void DoDispatch(Thunk thunk) override {
        std::mutex m;
        std::condition_variable cv;
        std::unique_lock<std::mutex> l(m);
        std::exception_ptr e;
        bool done = false;
        invoke_main([&]{
            std::unique_lock<std::mutex> l(m);
            try {
                thunk();
            }
            catch (...) {
                e = std::current_exception();
            }
            done = true;
            cv.notify_all();
        });
        cv.wait(l, [&]{ return done; });
        if (e) std::rethrow_exception(e);
    }
};

struct GCDQueue final : OSXQueue {
    dispatch_queue_t queue;
    GCDQueue(dispatch_queue_t queue) : queue(queue) { }
    ~GCDQueue() { dispatch_release(queue); }

    void DoPost(Thunk thunk) override {
        dispatch_async(queue, ^{
            try {
                thunk();
            }
            catch (...) {
                auto e = std::current_exception();
                invoke_main([=] { std::rethrow_exception(e); });
            }
        });
    }

    void DoDispatch(Thunk thunk) override {
        std::exception_ptr e;
        std::exception_ptr *e_ptr = &e;
        dispatch_sync(queue, ^{
            try {
                thunk();
            }
            catch (...) {
                *e_ptr = std::current_exception();
            }
        });
        if (e) std::rethrow_exception(e);
    }
};
}

namespace agi { namespace dispatch {
void Init(std::function<void (Thunk)> invoke_main) {
    ::invoke_main = std::move(invoke_main);
}

void Shutdown() {
    ::invoke_main = [](Thunk thunk) {
        if (thunk)
            std::thread worker([thunk = std::move(thunk)]() mutable { thunk(); });
            worker.join();
    };
}

void Executor::Post(Thunk thunk) {
    DoPost([thunk = std::move(thunk)]() mutable {
        try {
            thunk();
        }
        catch (...) {
            auto e = std::current_exception();
            invoke_main([e] { std::rethrow_exception(e); });
        }
    });
}

void Executor::Dispatch(Thunk thunk) {
    static_cast<OSXQueue *>(this)->DoDispatch(std::move(thunk));
}

void Queue::Async(Thunk thunk) { Post(std::move(thunk)); }
void Queue::Sync(Thunk thunk) { Dispatch(std::move(thunk)); }

Executor& MainExecutor() { return Main(); }
Executor& BackgroundExecutor() { return Background(); }

Queue& Main() {
    static MainQueue q;
    return q;
}

Queue& Background() {
    static GCDQueue q(dispatch_get_global_queue(0, DISPATCH_QUEUE_PRIORITY_DEFAULT));
    return q;
}

std::unique_ptr<Queue> Create() {
    return std::unique_ptr<Queue>(new GCDQueue(dispatch_queue_create("Aegisub worker queue",
                                                                     DISPATCH_QUEUE_SERIAL)));
}

std::unique_ptr<Executor> CreateExecutor() {
    return std::unique_ptr<Executor>(Create().release());
}
} }
