// Loading things without stopping the frame.
//
// Parsing a model or decoding an image takes tens of milliseconds, which is
// several frames' worth: doing it while the frame is being drawn is what a
// hitch is. This runs that work on a thread of its own and hands the result
// back at a point the program chooses, where it is safe to touch the world and
// the GPU.
//
// One thread, not a pool. Asset loading is bound by the disk and by a parser
// that is a single stream of work; a second thread would mostly wait, and the
// job scheduler's workers are busy with the physics step.

#pragma once

#include "Internal.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ludifex::detail
{

class LoadQueue
{
public:
    static LoadQueue& Get();

    ~LoadQueue();

    // Runs `work` off the main thread. When it finishes, `finish` waits until
    // the next Collect and runs on the thread that calls it, which is where
    // the world and the GPU may be touched.
    void Submit(std::function<void()> work, std::function<void()> finish);

    // Runs whatever has finished. Called once a frame from the main thread.
    void Collect();

    // How many are still in flight.
    size_t Pending() const { return m_Pending.load(std::memory_order_relaxed); }

    // Blocks until everything in flight has loaded.
    void WaitForAll();

    void Stop();

private:
    void WorkerLoop();
    void EnsureThread();

    struct Task
    {
        std::function<void()> Work;
        std::function<void()> Finish;
    };

    mutable std::mutex m_Mutex;
    std::condition_variable m_Wake;
    std::condition_variable m_Idle;
    std::queue<Task> m_Queue;
    std::vector<std::function<void()>> m_Finished;

    std::thread m_Thread;
    bool m_Running = false;
    std::atomic<size_t> m_Pending{ 0 };
};

} // namespace ludifex::detail
