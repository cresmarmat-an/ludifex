#include "Jobs.h"

#include <algorithm>

namespace ludifex::detail
{
namespace
{

// 0 means the thread that started the pool. Pool threads set their own.
thread_local uint32_t t_WorkerIndex = 0;

// Box3D allows at most this many workers, and Box2D is not happier with more.
constexpr uint32_t MaximumWorkers = 32;

// Room made when the pool starts. Both engines split their work into at most
// one chunk per worker, so a step's queue stays far below this; the queue and
// the pool still grow if they ever need to.
constexpr size_t InitialQueueCapacity = 1024;
constexpr size_t InitialGroups = 64;

} // namespace

JobSystem& GetJobSystem()
{
    static JobSystem system;
    return system;
}

JobSystem::~JobSystem()
{
    Stop();
}

uint32_t JobSystem::GetWorkerIndex()
{
    return t_WorkerIndex;
}

void JobSystem::Start(uint32_t workerCount)
{
    if (m_Running)
    {
        return;
    }

    if (workerCount == 0)
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        workerCount = (hardware == 0) ? 1u : hardware;
    }

    workerCount = std::clamp(workerCount, 1u, MaximumWorkers);

    m_WorkerCount = workerCount;
    m_Running = true;

    // One counter per worker, fixed for the life of the pool so no worker ever
    // writes into a vector that is growing under it.
    m_Busy = std::vector<std::atomic<uint64_t>>(workerCount);
    m_WindowOpened = std::chrono::steady_clock::now();

    m_Queue.assign(InitialQueueCapacity, Job{});
    m_QueueHead = 0;
    m_QueueCount = 0;

    m_Groups.clear();
    m_FreeGroups.clear();
    m_Groups.reserve(InitialGroups);
    m_FreeGroups.reserve(InitialGroups);
    for (size_t index = 0; index < InitialGroups; ++index)
    {
        m_Groups.push_back(std::make_unique<Group>());
        m_FreeGroups.push_back(InitialGroups - 1 - index);
    }

    // One fewer thread than workers: the calling thread is worker 0 and does
    // its share while it waits.
    for (uint32_t index = 1; index < workerCount; ++index)
    {
        m_Workers.emplace_back([this, index] { WorkerLoop(index); });
    }

    LogMessage(LogLevel::Info, "jobs", "Task scheduler running with %u workers.", workerCount);
}

void JobSystem::Stop()
{
    if (!m_Running)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Running = false;
        m_QueueHead = 0;
        m_QueueCount = 0;
    }
    m_Available.notify_all();

    for (std::thread& worker : m_Workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    m_Workers.clear();
    m_Groups.clear();
    m_FreeGroups.clear();
    m_WorkerCount = 1;
}

void JobSystem::WorkerLoop(uint32_t workerIndex)
{
    t_WorkerIndex = workerIndex;

    while (true)
    {
        Job job;

        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Available.wait(lock, [this] { return !m_Running || m_QueueCount > 0; });

            if (!m_Running)
            {
                return;
            }

            PopJob(job);
        }

        Run(job);
    }
}

void JobSystem::Run(const Job& job)
{
    // Timed per worker, so GetWorkerUtilisation can report how busy each
    // worker actually was.
    const auto started = std::chrono::steady_clock::now();

    if (job.Plain != nullptr)
    {
        job.Plain(job.Context);
    }
    else if (job.Range != nullptr)
    {
        job.Range(job.StartIndex, job.EndIndex, t_WorkerIndex, job.Context);
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (t_WorkerIndex < m_Busy.size())
    {
        m_Busy[t_WorkerIndex].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
            std::memory_order_relaxed);
    }

    if (job.Remaining != nullptr)
    {
        job.Remaining->fetch_sub(1, std::memory_order_release);
    }
}

std::vector<float> JobSystem::TakeUtilisation()
{
    std::vector<float> busy;
    if (!m_Running)
    {
        return busy;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto window = now - m_WindowOpened;
    m_WindowOpened = now;

    const double nanoseconds = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(window).count());

    busy.reserve(m_Busy.size());
    for (std::atomic<uint64_t>& worker : m_Busy)
    {
        const uint64_t spent = worker.exchange(0, std::memory_order_relaxed);
        busy.push_back(nanoseconds > 0.0 ? static_cast<float>(static_cast<double>(spent) / nanoseconds)
                                         : 0.0f);
    }
    return busy;
}

bool JobSystem::TryRunOne()
{
    Job job;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!PopJob(job))
        {
            return false;
        }
    }

    Run(job);
    return true;
}

void JobSystem::PushJob(const Job& job)
{
    if (m_QueueCount == m_Queue.size())
    {
        // Full: double it, keeping the jobs in order from the front.
        std::vector<Job> larger(std::max<size_t>(m_Queue.size() * 2, InitialQueueCapacity));
        for (size_t index = 0; index < m_QueueCount; ++index)
        {
            larger[index] = m_Queue[(m_QueueHead + index) % m_Queue.size()];
        }
        m_Queue.swap(larger);
        m_QueueHead = 0;
    }

    m_Queue[(m_QueueHead + m_QueueCount) % m_Queue.size()] = job;
    ++m_QueueCount;
}

bool JobSystem::PopJob(Job& job)
{
    if (m_QueueCount == 0)
    {
        return false;
    }

    job = m_Queue[m_QueueHead];
    m_QueueHead = (m_QueueHead + 1) % m_Queue.size();
    --m_QueueCount;
    return true;
}

JobSystem::Group* JobSystem::AcquireGroup()
{
    if (m_FreeGroups.empty())
    {
        m_Groups.push_back(std::make_unique<Group>());
        m_FreeGroups.reserve(m_Groups.size());
        return m_Groups.back().get();
    }

    const size_t slot = m_FreeGroups.back();
    m_FreeGroups.pop_back();
    return m_Groups[slot].get();
}

void* JobSystem::SubmitTask(PlainTask task, void* context)
{
    if (!m_Running || task == nullptr)
    {
        // Running it here and returning nothing is a valid answer: both
        // engines treat a null handle as "already done".
        if (task != nullptr)
        {
            task(context);
        }
        return nullptr;
    }

    Group* group = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        group = AcquireGroup();
        group->Remaining.store(1, std::memory_order_relaxed);

        Job job;
        job.Plain = task;
        job.Context = context;
        job.Remaining = &group->Remaining;
        PushJob(job);
    }

    m_Available.notify_one();
    return group;
}

void* JobSystem::SubmitRange(RangeTask task, int itemCount, int minRange, void* context)
{
    if (!m_Running || task == nullptr || itemCount <= 0)
    {
        if (task != nullptr && itemCount > 0)
        {
            task(0, itemCount, 0, context);
        }
        return nullptr;
    }

    minRange = std::max(1, minRange);

    // Never more chunks than workers, and never chunks below minRange, because
    // handing off less work than that costs more than doing it.
    int chunkCount = std::max(1, itemCount / minRange);
    chunkCount = std::min(chunkCount, static_cast<int>(m_WorkerCount));

    if (chunkCount == 1)
    {
        task(0, itemCount, t_WorkerIndex, context);
        return nullptr;
    }

    Group* group = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        group = AcquireGroup();
        group->Remaining.store(chunkCount, std::memory_order_relaxed);

        const int chunkSize = itemCount / chunkCount;
        int start = 0;

        for (int chunk = 0; chunk < chunkCount; ++chunk)
        {
            // The last chunk absorbs the remainder, so every item is covered
            // exactly once.
            const int end = (chunk == chunkCount - 1) ? itemCount : start + chunkSize;

            Job job;
            job.Range = task;
            job.Context = context;
            job.StartIndex = start;
            job.EndIndex = end;
            job.Remaining = &group->Remaining;
            PushJob(job);

            start = end;
        }
    }

    m_Available.notify_all();
    return group;
}

void JobSystem::Wait(void* handle)
{
    if (handle == nullptr)
    {
        return;
    }

    auto* group = static_cast<Group*>(handle);

    // Helping rather than blocking. A physics task that waits on its own
    // sub-tasks would deadlock a pool that put the waiting thread to sleep;
    // here it keeps draining the queue instead.
    while (group->Remaining.load(std::memory_order_acquire) > 0)
    {
        if (!TryRunOne())
        {
            std::this_thread::yield();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        for (size_t index = 0; index < m_Groups.size(); ++index)
        {
            if (m_Groups[index].get() == group)
            {
                m_FreeGroups.push_back(index);
                break;
            }
        }
    }
}

} // namespace ludifex::detail
