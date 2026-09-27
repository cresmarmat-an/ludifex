// The profiler. Not installed and not part of the public API.
//
// Off until a program asks for it. While it is off, a scope costs one relaxed
// read of a boolean, so the timing points can stay in the code permanently.
//
// Sections are named by pointer, not by string: the names are literals, so
// comparing and hashing them compares addresses. A frame's sections are kept
// in the order they first ran.

#pragma once

#include "Internal.h"

#include <chrono>
#include <vector>

namespace ludifex::detail
{

class Profiler
{
public:
    static Profiler& Get();

    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled);

    // Opens a new frame: what was gathered becomes the last complete frame,
    // and the gathering starts again.
    void BeginFrame();

    void Add(const char* name, double milliseconds);

    std::vector<ProfileSection> TakeSnapshot() const;

private:
    struct Entry
    {
        const char* Name = "";
        double Milliseconds = 0.0;
        int Calls = 0;
    };

    bool m_Enabled = false;
    std::vector<Entry> m_Current;
    std::vector<Entry> m_Last;
};

// Times a stretch of work and hands it to the profiler when it ends.
class ProfileScope
{
public:
    explicit ProfileScope(const char* name)
        : m_Name(name)
    {
        if (Profiler::Get().IsEnabled())
        {
            m_Started = std::chrono::steady_clock::now();
            m_Timing = true;
        }
    }

    ~ProfileScope()
    {
        if (!m_Timing)
        {
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - m_Started;
        Profiler::Get().Add(
            m_Name, static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
                        1e6);
    }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    const char* m_Name = "";
    std::chrono::steady_clock::time_point m_Started;
    bool m_Timing = false;
};

} // namespace ludifex::detail

// One line at the top of the work it measures. The name is joined through a
// second macro because __LINE__ only expands when it is passed on.
#define LUDIFEX_PROFILE_JOIN_INNER(a, b) a##b
#define LUDIFEX_PROFILE_JOIN(a, b) LUDIFEX_PROFILE_JOIN_INNER(a, b)
#define LUDIFEX_PROFILE(name)                                                                          ::ludifex::detail::ProfileScope LUDIFEX_PROFILE_JOIN(profileScope, __LINE__)(name)
