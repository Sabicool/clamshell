#include "worker.h"

#ifdef __3DS__
#include <3ds.h>
#else
#include <thread>
#endif

namespace anki {

// On the 3DS this avoids condition variables: with libctru's, the app and
// both workers once all ended up asleep with work queued. Instead the queue
// has a plain lock, the worker sleeps on an event (with a timeout, so a missed
// signal costs a few milliseconds, never a hang), and waiting is polling.

#ifdef __3DS__
struct Worker::Sys {
    LightLock lock;
    LightEvent work;
};
#else
struct Worker::Sys {};
#endif

Worker::Worker(size_t stack_bytes) {
#ifdef __3DS__
    sys_ = new Sys;
    LightLock_Init(&sys_->lock);
    LightEvent_Init(&sys_->work, RESET_ONESHOT);
    // Anki's backend, SQLite and HTML layout need a deep stack. A lower
    // priority than the interface thread, so drawing and input come first;
    // the New 3DS's spare core (2) if it's available to the app, else the
    // app's own core.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s32 mine = prio + 1 > 0x3F ? 0x3F : prio + 1;
    Thread t = threadCreate(entry, this, stack_bytes, mine, 2, false);
    if (!t) t = threadCreate(entry, this, stack_bytes, mine, -2, false);
    thread_ = t;
#else
    (void)stack_bytes;
    thread_ = new std::thread(entry, this);
#endif
    started_ = thread_ != nullptr;
}

Worker::~Worker() {
#ifdef __3DS__
    LightLock_Lock(&sys_->lock);
    stop_ = true;
    LightLock_Unlock(&sys_->lock);
    LightEvent_Signal(&sys_->work);
    if (thread_) {
        threadJoin((Thread)thread_, U64_MAX);
        threadFree((Thread)thread_);
    }
    delete sys_;
#else
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (!thread_) return;
    auto* t = (std::thread*)thread_;
    t->join();
    delete t;
#endif
}

void Worker::entry(void* self) { ((Worker*)self)->loop(); }

#ifdef __3DS__

void Worker::loop() {
    for (;;) {
        LightLock_Lock(&sys_->lock);
        if (jobs_.empty()) {
            bool stop = stop_;
            LightLock_Unlock(&sys_->lock);
            if (stop) return;
            LightEvent_WaitTimeout(&sys_->work, 20 * 1000000LL);
            continue;
        }
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        running_ = true;
        LightLock_Unlock(&sys_->lock);
        job();
        LightLock_Lock(&sys_->lock);
        running_ = false;
        LightLock_Unlock(&sys_->lock);
    }
}

void Worker::submit(std::function<void()> job) {
    if (!started_) {  // no thread: run it here
        job();
        return;
    }
    LightLock_Lock(&sys_->lock);
    jobs_.push_back(std::move(job));
    LightLock_Unlock(&sys_->lock);
    LightEvent_Signal(&sys_->work);
}

bool Worker::idle() {
    LightLock_Lock(&sys_->lock);
    bool idle = jobs_.empty() && !running_;
    LightLock_Unlock(&sys_->lock);
    return idle;
}

void Worker::wait() {
    while (!idle()) svcSleepThread(1000000LL);
}

void Worker::wait_until(const std::function<bool()>& done) {
    while (!done() && !idle()) svcSleepThread(1000000LL);
}

#else

void Worker::loop() {
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) return;  // stopping, nothing left
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        running_ = true;
        l.unlock();
        job();
        l.lock();
        running_ = false;
        cv_.notify_all();
    }
}

void Worker::submit(std::function<void()> job) {
    if (!started_) {
        job();
        return;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

bool Worker::idle() {
    std::lock_guard<std::mutex> l(m_);
    return jobs_.empty() && !running_;
}

void Worker::wait() {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [this] { return jobs_.empty() && !running_; });
}

void Worker::wait_until(const std::function<bool()>& done) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { return done() || (jobs_.empty() && !running_); });
}

#endif

}  // namespace anki
