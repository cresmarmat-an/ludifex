// The shared task scheduler.
// Not installed and not part of the public API.
//
// This exists to drive the physics engines' own multithreading: both Box2D and
// Box3D accept enqueue/finish callbacks and subdivide their solver work into
// them. One pool serves both, so physics never over-subscribes the CPU with
// competing thread pools.
//
// Waiting is cooperative. Box3D's own documentation warns that a task which
// blocks on its sub-tasks without yielding its thread can deadlock, so Wait
// runs other pending jobs on the waiting thread instead of sleeping. That is
// also why a job may be started by one thread and finished by another.

#pragma once

#include "Internal.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

namespace ludifex::detail
{

using PlainTask = void (*)(void* context);
using RangeTask = void (*)(int startIndex, int endIndex, uint32_t workerIndex, void* context);

class JobSystem
{
public:
    ~JobSystem();

    // workerCount counts the calling thread, so 4 starts three pool threads.
    // Zero asks for one worker per hardware thread.
    void Start(uint32_t workerCount);
    void Stop();

    bool IsRunning() const { return m_Running; }
    uint32_t GetWorkerCount() const { return m_WorkerCount; }

    // 0 on the thread that started the pool, 1..n on pool threads. Box2D needs
    // this to index its per-worker scratch space.
    static uint32_t GetWorkerIndex();

    // How much of the time since the last call each worker spent running jobs
    // rather than waiting, from 0 to 1. Reading resets the window, so two
    // consecutive calls describe the stretch between them.
    std::vector<float> TakeUtilisation();

    // One job, run once on some thread. Returns a handle for Wait.
    void* SubmitTask(PlainTask task, void* context);

    // A parallel for. The range is split into at most one chunk per worker,
    // and never into chunks smaller than minRange, because below that the
    // hand-off costs more than the work.
    void* SubmitRange(RangeTask task, int itemCount, int minRange, void* context);

    // Runs other pending jobs until this one is done, rather than blocking.
    void Wait(void* handle);

private:
    struct Job
    {
        PlainTask Plain = nullptr;
        RangeTask Range = nullptr;
        void* Context = nullptr;

        int StartIndex = 0;
        int EndIndex = 0;

        // A group is complete when every chunk has run. The counter belongs to
        // the pooled group, which outlives every job that points at it: a
        // group is only returned to the pool by Wait, and Wait is what waits
        // for those jobs.
        std::atomic<int>* Remaining = nullptr;
    };

    struct Group
    {
        std::atomic<int> Remaining{ 0 };
    };

    void WorkerLoop(uint32_t workerIndex);
    bool TryRunOne();
    void Run(const Job& job);

    // The queue and the group pool, both called with m_Mutex held.
    void PushJob(const Job& job);
    bool PopJob(Job& job);
    Group* AcquireGroup();

    std::vector<std::thread> m_Workers;

    // A ring buffer, allocated when the pool starts and only grown if it
    // fills, so queuing a job does not allocate.
    std::vector<Job> m_Queue;
    size_t m_QueueHead = 0;
    size_t m_QueueCount = 0;

    mutable std::mutex m_Mutex;
    std::condition_variable m_Available;

    // Groups outlive the jobs that reference them, so handles stay valid until
    // Wait releases them. A number of them are created when the pool starts,
    // and m_FreeGroups always has room for every group, so returning one to
    // the pool does not allocate.
    std::vector<std::unique_ptr<Group>> m_Groups;
    std::vector<size_t> m_FreeGroups;

    std::atomic<bool> m_Running{ false };

    // Nanoseconds each worker has spent inside a job since the window opened.
    std::vector<std::atomic<uint64_t>> m_Busy;
    std::chrono::steady_clock::time_point m_WindowOpened;
    uint32_t m_WorkerCount = 1;
};

JobSystem& GetJobSystem();

} // namespace ludifex::detail
