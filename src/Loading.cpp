#include "Loading.h"

namespace ludifex::detail
{

LoadQueue& LoadQueue::Get()
{
    static LoadQueue queue;
    return queue;
}

LoadQueue::~LoadQueue()
{
    Stop();
}

void LoadQueue::EnsureThread()
{
    // Started on the first thing asked for, so a program that loads nothing in
    // the background never starts a thread.
    if (m_Running)
    {
        return;
    }

    m_Running = true;
    m_Thread = std::thread([this] { WorkerLoop(); });
}

void LoadQueue::Submit(std::function<void()> work, std::function<void()> finish)
{
    if (!work)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        EnsureThread();
        m_Queue.push(Task{ std::move(work), std::move(finish) });
        m_Pending.fetch_add(1, std::memory_order_relaxed);
    }
    m_Wake.notify_one();
}

void LoadQueue::WorkerLoop()
{
    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Wake.wait(lock, [this] { return !m_Running || !m_Queue.empty(); });
            if (!m_Running && m_Queue.empty())
            {
                return;
            }
            task = std::move(m_Queue.front());
            m_Queue.pop();
        }

        task.Work();

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (task.Finish)
            {
                m_Finished.push_back(std::move(task.Finish));
            }
            m_Pending.fetch_sub(1, std::memory_order_relaxed);
        }
        m_Idle.notify_all();
    }
}

void LoadQueue::Collect()
{
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ready.swap(m_Finished);
    }

    // Run outside the lock: a finish handler may load something else, and it
    // would otherwise deadlock on the queue it is adding to.
    for (std::function<void()>& finish : ready)
    {
        finish();
    }
}

void LoadQueue::WaitForAll()
{
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Idle.wait(lock, [this] { return m_Queue.empty() && m_Pending.load() == 0; });
    }
    Collect();
}

void LoadQueue::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running)
        {
            return;
        }
        m_Running = false;
    }
    m_Wake.notify_all();

    if (m_Thread.joinable())
    {
        m_Thread.join();
    }

    // Anything that finished but was never collected is dropped: the world it
    // would have written into is going away too.
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Finished.clear();
    while (!m_Queue.empty())
    {
        m_Queue.pop();
    }
    m_Pending.store(0, std::memory_order_relaxed);
}

} // namespace ludifex::detail
