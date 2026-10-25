#pragma once
///@file

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nix {

/* Threads that lex the chunks of big files, kept between files */
class WorkPool
{
    std::mutex busy;
    std::mutex m;
    std::condition_variable wake, finished;
    std::function<void(size_t)> job;
    size_t next = 0, total = 0, running = 0;
    std::vector<std::thread> threads;

    void work()
    {
        std::unique_lock lock(m);
        while (true) {
            wake.wait(lock, [&] { return next < total; });
            size_t i = next++;
            running++;
            lock.unlock();
            job(i);
            lock.lock();
            if (--running == 0 && next >= total)
                finished.notify_all();
        }
    }

public:
    /* Never destroyed: its threads wait on it until the process exits */
    static WorkPool & shared()
    {
        static auto & pool = *new WorkPool;
        return pool;
    }

    /* Run `fn(1)` to `fn(count - 1)` on the pool while the caller runs
       `fn(0)`, and wait for all of them. `fn` must not throw */
    void run(size_t count, std::function<void(size_t)> fn, size_t threadCount)
    {
        /* Another thread has the pool: do this alone */
        std::unique_lock busyLock(busy, std::try_to_lock);
        if (!busyLock) {
            for (size_t i = 0; i < count; i++)
                fn(i);
            return;
        }
        std::unique_lock lock(m);
        while (threads.size() < threadCount)
            threads.emplace_back([this] { work(); }).detach();
        job = std::move(fn);
        next = 1;
        total = count;
        wake.notify_all();
        lock.unlock();
        job(0);
        lock.lock();
        finished.wait(lock, [&] { return next >= total && running == 0; });
        total = 0;
    }
};

} // namespace nix
