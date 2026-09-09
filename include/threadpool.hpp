#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

// A fixed-size pool of worker threads that pull tasks off a shared queue.
//
// Why we need this: if we spawned a new std::thread for every client
// connection, a slow-loris attack (or just 10,000 clients) would spawn
// 10,000 OS threads and the machine would fall over from context-switch
// overhead alone. A bounded pool means connections queue up instead of
// threads exploding.
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stopFlag(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    // Submit a task (any callable with no args/return) to the pool.
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopFlag) return; // pool is shutting down, refuse new work
            taskQueue.push(std::move(task));
        }
        condVar.notify_one(); // wake exactly one sleeping worker
    }

    // Signal all workers to finish current task and exit, then join them.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopFlag = true;
        }
        condVar.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    // Not copyable - it owns threads and a mutex.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                // Sleep until there's work OR we're told to stop.
                // This avoids busy-waiting / spinning on the CPU.
                condVar.wait(lock, [this] { return stopFlag || !taskQueue.empty(); });

                if (stopFlag && taskQueue.empty()) {
                    return; // no more work and we're shutting down -> exit thread
                }

                task = std::move(taskQueue.front());
                taskQueue.pop();
            }
            task(); // run the task OUTSIDE the lock so workers don't block each other
        }
    }

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> taskQueue;
    std::mutex queueMutex;
    std::condition_variable condVar;
    std::atomic<bool> stopFlag;
};
