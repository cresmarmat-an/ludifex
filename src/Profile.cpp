#include "Profile.h"

#include "Jobs.h"

#include <algorithm>

namespace ludifex
{
namespace detail
{

Profiler& Profiler::Get()
{
    static Profiler profiler;
    return profiler;
}

void Profiler::SetEnabled(bool enabled)
{
    if (m_Enabled == enabled)
    {
        return;
    }

    m_Enabled = enabled;
    m_Current.clear();
    m_Last.clear();
}

void Profiler::BeginFrame()
{
    if (!m_Enabled)
    {
        return;
    }

    m_Last = m_Current;
    m_Current.clear();
}

void Profiler::Add(const char* name, double milliseconds)
{
    if (!m_Enabled)
    {
        return;
    }

    // Names are literals, so the same section is the same pointer and the
    // lookup is a handful of pointer comparisons over a short list.
    for (Entry& entry : m_Current)
    {
        if (entry.Name == name)
        {
            entry.Milliseconds += milliseconds;
            ++entry.Calls;
            return;
        }
    }

    m_Current.push_back(Entry{ name, milliseconds, 1 });
}

std::vector<ProfileSection> Profiler::TakeSnapshot() const
{
    std::vector<ProfileSection> sections;
    sections.reserve(m_Last.size());

    for (const Entry& entry : m_Last)
    {
        sections.push_back(ProfileSection{ entry.Name, static_cast<float>(entry.Milliseconds),
                                           entry.Calls });
    }
    return sections;
}

} // namespace detail

// ---------------------------------------------------------------------------
// The public calls
// ---------------------------------------------------------------------------

void SetProfilingEnabled(bool enabled)
{
    detail::Profiler::Get().SetEnabled(enabled);
}

bool IsProfilingEnabled()
{
    return detail::Profiler::Get().IsEnabled();
}

std::vector<ProfileSection> GetProfile()
{
    return detail::Profiler::Get().TakeSnapshot();
}

std::vector<float> GetWorkerUtilisation()
{
    return detail::GetJobSystem().TakeUtilisation();
}

} // namespace ludifex
