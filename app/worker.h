// One background thread that runs jobs in order: Clamshell saves answers
// there (an SD card write on the 3DS can take seconds) while the next card
// is already on screen.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace anki {

class Worker {
public:
    explicit Worker(size_t stack_bytes = 1024 * 1024);
    ~Worker();
    void submit(std::function<void()> job);
    bool idle();  // nothing queued or running
    void wait();  // until idle
    // until done() is true (checked after each job) or nothing is left
    void wait_until(const std::function<bool()>& done);

private:
    static void entry(void* self);
    void loop();
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool running_ = false, stop_ = false, started_ = false;
    void* thread_ = nullptr;
    struct Sys;  // the 3DS's own lock and event
    Sys* sys_ = nullptr;
};

}  // namespace anki
