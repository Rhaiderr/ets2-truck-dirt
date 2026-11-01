// D11n (the user, 30/10: "optimise the CPU too, without losing quality, the work in more threads"): the plugin's
// files (telemetry.csv, configs.log, dirt_state.txt, wash_spots.txt) written on a thread of their own. The game's
// thread, where the telemetry callbacks run, only formats the bytes and queues them: a slow disk or an antivirus
// scan on a flush or a rename no longer holds a frame. The jobs run one after another in the order they came;
// Stop runs every job left before it returns (the shutdown's save is written before the files close).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace file_writer {

class Writer {
public:
    // The plugin's shutdown stops it; still running here means the process is ending without it (the thread may be
    // gone, its lock held): let it go rather than wait.
    ~Writer() {
        if (thread_.joinable()) thread_.detach();
    }

    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    // The job on the writer's thread, after every job before it; here and now when the thread is not running.
    void Post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (thread_.joinable() && !stop_) {
                jobs_.push_back(std::move(job));
                wake_.notify_one();
                return;
            }
        }
        job();
    }

    // Runs the jobs left, then ends the thread.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!thread_.joinable()) return;
            stop_ = true;
            wake_.notify_one();
        }
        thread_.join();
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stop_, nothing left
            std::function<void()> job = std::move(jobs_.front());
            jobs_.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
    bool stop_ = false;
};

}  // namespace file_writer
