// miniaudio pulls in windows.h, whose min and max macros would otherwise break
// every std::max and std::clamp below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "Audio.h"

#include "Profile.h"

#include "Assets.h"
#include "HostProtocol.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

// miniaudio and stb_vorbis are compiled here with internal linkage (MA_API as
// static, and stb_vorbis renamed), so a program that also links opane (which
// has its own copies) has no duplicate symbols.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4505) // unreferenced local function removed
#pragma warning(disable : 4701) // potentially uninitialized local variable
#pragma warning(disable : 4703) // potentially uninitialized local pointer
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define MA_API static
#define MA_NO_ENCODING

// The one miniaudio global declared without MA_API, renamed so it stays apart
// from opane's copy too.
#define ma_atomic_global_lock ludifex_ma_atomic_global_lock

#include "VorbisNames.h"
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ludifex
{
namespace detail
{
namespace
{

constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;

// Ramps long enough that no change of level is audible as a click, short
// enough that nothing feels late.
constexpr uint32_t StartFadeMilliseconds = 40;
constexpr uint32_t StopFadeMilliseconds = 40;
constexpr uint32_t GainFadeMilliseconds = 60;

// A voice already playing counts as this much louder when deciding who plays,
// so two voices of nearly equal loudness do not trade places every frame.
constexpr float PlayingBias = 1.25f;

// How often an occluded voice rechecks the line between it and the listener.
constexpr double OcclusionInterval = 0.1;

// Faster than anything in a game plausibly moves. A camera cut or a teleport
// would otherwise read as a supersonic velocity and wrench the pitch.
constexpr float MaxAudibleSpeed = 120.0f;

struct SoundRecord
{
    uint32_t Generation = 0;
    bool Alive = false;
    ma_sound* Prototype = nullptr;
    std::string Path;
    float Duration = 0.0f;
};

// The process's one audio engine. Created on first use and intentionally never
// destroyed: a host's audio thread may still be pulling this engine's mix
// while static objects are destroyed at exit.
class AudioSystem
{
public:
    static AudioSystem& Get()
    {
        static AudioSystem* system = new AudioSystem();
        return *system;
    }

    AudioConfig Config;

    bool Ensure()
    {
        if (m_Engine != nullptr)
        {
            return true;
        }
        if (m_Failed)
        {
            return false;
        }

        m_Engine = new ma_engine();

        ma_engine_config config = ma_engine_config_init();
        config.listenerCount = MA_ENGINE_MAX_LISTENERS;

        auto* host = static_cast<HostAudio*>(
            SDL_GetPointerProperty(SDL_GetGlobalProperties(), HostAudioProperty, nullptr));

        const char* mode = "its own audio device";
        if (Config.Output == AudioOutput::Manual)
        {
            config.noDevice = MA_TRUE;
            config.channels = std::max<uint32_t>(1, Config.Channels);
            config.sampleRate = std::max<uint32_t>(8000, Config.SampleRate);
            mode = "manual output";
        }
        else if (host != nullptr && host->Version == HostProtocolVersion && host->AddSource != nullptr)
        {
            // The host's device, not a second one: this engine mixes without
            // a device, and the host pulls the mix into its own output.
            config.noDevice = MA_TRUE;
            config.channels = host->Channels;
            config.sampleRate = host->SampleRate;
            mode = "the host application's audio output";
        }

        if (ma_engine_init(&config, m_Engine) != MA_SUCCESS)
        {
            delete m_Engine;
            m_Engine = nullptr;
            m_Failed = true;

            // No audio device is normal on a headless machine; the world runs
            // on in silence.
            LogMessage(LogLevel::Warning, "audio", "No audio output could be opened. World sound is disabled.");
            return false;
        }

        if (Config.Output != AudioOutput::Manual && host != nullptr && config.noDevice)
        {
            m_Host = host;
            m_HostSource = host->AddSource(host->Context, &ReadForHost, m_Engine);
            if (m_HostSource < 0)
            {
                LogMessage(LogLevel::Warning, "audio", "The host would not take this mix; world sound is silent.");
            }
        }

        LogMessage(LogLevel::Info, "audio", "World audio running at %u Hz through %s.",
                   ma_engine_get_sample_rate(m_Engine), mode);
        return true;
    }

    ma_engine* Engine() { return m_Engine; }

    bool IsManual() const { return m_Engine != nullptr && Config.Output == AudioOutput::Manual; }

    void DetachFromHost()
    {
        if (m_Host != nullptr && m_HostSource >= 0 && m_Host->RemoveSource != nullptr)
        {
            m_Host->RemoveSource(m_Host->Context, m_HostSource);
        }
        m_Host = nullptr;
        m_HostSource = -1;
    }

    // Listener slots, one per world, so each world hears its own sounds from
    // its own camera.
    uint32_t AcquireListener()
    {
        for (uint32_t index = 0; index < MA_ENGINE_MAX_LISTENERS; ++index)
        {
            if (!m_ListenerUsed[index])
            {
                m_ListenerUsed[index] = true;
                return index;
            }
        }
        LogMessage(LogLevel::Warning, "audio",
                   "More than %d worlds are playing sound at once; the extra ones share the first listener.",
                   MA_ENGINE_MAX_LISTENERS);
        return 0;
    }

    void ReleaseListener(uint32_t index)
    {
        if (index < MA_ENGINE_MAX_LISTENERS)
        {
            m_ListenerUsed[index] = false;
        }
    }

    // --- sounds ------------------------------------------------------------

    SoundId Load(const std::string& path)
    {
        const auto cached = m_ByPath.find(path);
        if (cached != m_ByPath.end() && Resolve(cached->second) != nullptr)
        {
            return cached->second;
        }

        if (!Ensure())
        {
            return SoundId{};
        }

        // A missing sound is a warning, not a failure: the program keeps
        // running, and playing the invalid id is silent.
        const std::string resolved = ResolveAsset(path, "audio", LogLevel::Warning);
        if (resolved.empty())
        {
            return SoundId{};
        }

        auto* prototype = new ma_sound();
        if (ma_sound_init_from_file(m_Engine, resolved.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, prototype) !=
            MA_SUCCESS)
        {
            delete prototype;
            LogMessage(LogLevel::Warning, "audio", "Could not decode \"%s\"; it will be silent.", resolved.c_str());
            return SoundId{};
        }

        uint32_t index;
        if (!m_Free.empty())
        {
            index = m_Free.back();
            m_Free.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(m_Sounds.size());
            m_Sounds.emplace_back();
        }

        SoundRecord& record = m_Sounds[index];
        ++record.Generation;
        if (record.Generation == 0)
        {
            record.Generation = 1;
        }
        record.Alive = true;
        record.Prototype = prototype;
        record.Path = path;

        float seconds = 0.0f;
        ma_sound_get_length_in_seconds(prototype, &seconds);
        record.Duration = seconds;

        const SoundId id{ index, record.Generation };
        m_ByPath[path] = id;
        return id;
    }

    void Destroy(const SoundId& id)
    {
        SoundRecord* record = Resolve(id);
        if (record == nullptr)
        {
            return;
        }
        ma_sound_uninit(record->Prototype);
        delete record->Prototype;
        record->Prototype = nullptr;
        record->Alive = false;
        ++record->Generation;
        if (record->Generation == 0)
        {
            record->Generation = 1;
        }
        m_ByPath.erase(record->Path);
        m_Free.push_back(id.Index);
    }

    SoundRecord* Resolve(const SoundId& id)
    {
        if (!id.IsValid() || id.Index >= m_Sounds.size())
        {
            return nullptr;
        }
        SoundRecord& record = m_Sounds[id.Index];
        return (record.Alive && record.Generation == id.Generation) ? &record : nullptr;
    }

private:
    AudioSystem() = default;

    static void ReadForHost(void* user, float* frames, uint32_t frameCount)
    {
        ma_engine_read_pcm_frames(static_cast<ma_engine*>(user), frames, frameCount, nullptr);
    }

    ma_engine* m_Engine = nullptr;
    bool m_Failed = false;
    HostAudio* m_Host = nullptr;
    int m_HostSource = -1;
    bool m_ListenerUsed[MA_ENGINE_MAX_LISTENERS] = {};

    std::vector<SoundRecord> m_Sounds;
    std::vector<uint32_t> m_Free;
    std::unordered_map<std::string, SoundId> m_ByPath;
};

float Distance(const Vec3& a, const Vec3& b)
{
    const float dx = a.X - b.X;
    const float dy = a.Y - b.Y;
    const float dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 ClampSpeed(const Vec3& velocity)
{
    const float speed = std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z);
    if (!std::isfinite(speed) || speed > MaxAudibleSpeed)
    {
        return Vec3{};
    }
    return velocity;
}

} // namespace

void ReleaseAudioForHost()
{
    AudioSystem::Get().DetachFromHost();
}

// ---------------------------------------------------------------------------
// A world's audio
// ---------------------------------------------------------------------------

WorldAudio::WorldAudio()
{
    AudioSystem& system = AudioSystem::Get();
    if (!system.Ensure())
    {
        return;
    }

    m_Group = new ma_sound();
    if (ma_sound_group_init(system.Engine(), 0, nullptr, m_Group) != MA_SUCCESS)
    {
        delete m_Group;
        m_Group = nullptr;
        LogMessage(LogLevel::Warning, "audio", "Could not create a world's volume group; its sound is disabled.");
        return;
    }

    m_Listener = system.AcquireListener();
    ma_spatializer_listener_set_speed_of_sound(&system.Engine()->listeners[m_Listener], Settings.SpeedOfSound);
}

WorldAudio::~WorldAudio()
{
    for (uint32_t index = 0; index < m_Voices.size(); ++index)
    {
        if (m_Voices[index].Alive)
        {
            Release(index);
        }
    }

    for (ma_sound* music : m_FadingMusic)
    {
        ma_sound_uninit(music);
        delete music;
    }
    if (m_Music != nullptr)
    {
        ma_sound_uninit(m_Music);
        delete m_Music;
    }

    if (m_Group != nullptr)
    {
        ma_sound_group_uninit(m_Group);
        delete m_Group;
        AudioSystem::Get().ReleaseListener(m_Listener);
    }
}

VoiceRecord* WorldAudio::Resolve(const VoiceId& voice)
{
    if (!voice.IsValid() || voice.Index >= m_Voices.size())
    {
        return nullptr;
    }
    VoiceRecord& record = m_Voices[voice.Index];
    return (record.Alive && record.Generation == voice.Generation) ? &record : nullptr;
}

const VoiceRecord* WorldAudio::Resolve(const VoiceId& voice) const
{
    return const_cast<WorldAudio*>(this)->Resolve(voice);
}

void WorldAudio::Release(uint32_t index)
{
    VoiceRecord& record = m_Voices[index];
    if (record.Sound != nullptr)
    {
        ma_sound_uninit(record.Sound);
        delete record.Sound;
        record.Sound = nullptr;
    }
    record.Alive = false;
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }
    m_Free.push_back(index);
}

float WorldAudio::AudibilityOf(const VoiceRecord& voice) const
{
    const float base = voice.Settings.Volume * voice.UserVolume * voice.OcclusionGain;
    if (voice.Placement == VoicePlacement::None)
    {
        return base;
    }

    // The inverse-distance model the mixer itself uses, and silence past the
    // far edge of the range.
    const SoundSettings& settings = voice.Settings;
    const float distance = Distance(voice.Position, m_ListenerPosition);
    if (distance > settings.MaxDistance)
    {
        return 0.0f;
    }
    const float minimum = std::max(settings.MinDistance, 1e-3f);
    const float clamped = std::max(distance, minimum);
    const float attenuation = minimum / (minimum + settings.Rolloff * (clamped - minimum));
    return base * attenuation;
}

int WorldAudio::CountPlaying() const
{
    int count = 0;
    for (const VoiceRecord& voice : m_Voices)
    {
        if (voice.Alive && !voice.Virtual && !voice.Stopping)
        {
            ++count;
        }
    }
    return count;
}

void WorldAudio::ApplyGain(VoiceRecord& voice, uint32_t milliseconds)
{
    // The fader carries everything that changes while a voice plays, so every
    // change is a ramp from wherever the level is now.
    ma_sound_set_fade_in_milliseconds(voice.Sound, -1.0f, voice.UserVolume * voice.OcclusionGain, milliseconds);
}

void WorldAudio::Virtualize(VoiceRecord& voice)
{
    ma_uint64 cursor = 0;
    ma_sound_get_cursor_in_pcm_frames(voice.Sound, &cursor);
    voice.VirtualCursor = cursor;
    voice.VirtualSince = m_Clock;
    voice.Virtual = true;
    ma_sound_stop_with_fade_in_milliseconds(voice.Sound, StopFadeMilliseconds);
}

bool WorldAudio::Resume(VoiceRecord& voice)
{
    // Still finishing the fade out from going virtual; a seek now would jump
    // the waveform audibly, so wait a frame.
    if (ma_sound_is_playing(voice.Sound))
    {
        return false;
    }

    // Where it would have got to had it kept playing, so a looping sound comes
    // back in step rather than restarting.
    ma_uint64 length = 0;
    ma_sound_get_length_in_pcm_frames(voice.Sound, &length);
    ma_uint32 sampleRate = 0;
    ma_sound_get_data_format(voice.Sound, nullptr, nullptr, &sampleRate, nullptr, 0);
    if (length > 0 && sampleRate > 0)
    {
        const double elapsed = (m_Clock - voice.VirtualSince) * sampleRate * voice.Settings.Pitch;
        const uint64_t cursor = (voice.VirtualCursor + static_cast<uint64_t>(std::max(0.0, elapsed))) % length;
        ma_sound_seek_to_pcm_frame(voice.Sound, cursor);
    }

    ma_sound_reset_stop_time_and_fade(voice.Sound);
    ma_sound_set_fade_in_milliseconds(voice.Sound, 0.0f, voice.UserVolume * voice.OcclusionGain, StartFadeMilliseconds);
    ma_sound_start(voice.Sound);
    voice.Virtual = false;

    // Whatever was or was not in the way while it waited, ask again now.
    voice.NextOcclusionCheck = m_Clock;
    return true;
}

VoiceId WorldAudio::Play(SoundId sound, const SoundSettings& settings, VoicePlacement placement,
                         const Vec3& position, const ActorId& actor)
{
    AudioSystem& system = AudioSystem::Get();
    SoundRecord* source = system.Resolve(sound);
    if (!IsReady() || source == nullptr)
    {
        return VoiceId{};
    }

    VoiceRecord candidate;
    candidate.Settings = settings;
    candidate.Placement = placement;
    candidate.Position = position;
    candidate.Actor = actor;
    candidate.Audibility = AudibilityOf(candidate);

    const bool inRange = placement == VoicePlacement::None || candidate.Audibility > 0.0f;

    // A one-shot starting out of range would never be heard.
    if (!settings.Looping && !inRange)
    {
        ++m_Culled;
        return VoiceId{};
    }

    // Over the limit: the least important playing voice gives way if this one
    // matters more, and otherwise this one does.
    bool startVirtual = !inRange;
    if (inRange && CountPlaying() >= std::max(1, Settings.MaxVoices))
    {
        VoiceRecord* weakest = nullptr;
        for (VoiceRecord& voice : m_Voices)
        {
            if (!voice.Alive || voice.Virtual || voice.Stopping)
            {
                continue;
            }
            if (weakest == nullptr || voice.Settings.Priority < weakest->Settings.Priority ||
                (voice.Settings.Priority == weakest->Settings.Priority && voice.Audibility < weakest->Audibility))
            {
                weakest = &voice;
            }
        }

        const bool wins = weakest != nullptr &&
                          (settings.Priority > weakest->Settings.Priority ||
                           (settings.Priority == weakest->Settings.Priority && candidate.Audibility > weakest->Audibility));
        if (wins)
        {
            if (weakest->Settings.Looping)
            {
                Virtualize(*weakest);
            }
            else
            {
                ma_sound_stop_with_fade_in_milliseconds(weakest->Sound, StopFadeMilliseconds);
                weakest->Stopping = true;
            }
            ++m_Stolen;
        }
        else if (settings.Looping)
        {
            startVirtual = true;
        }
        else
        {
            ++m_Stolen;
            return VoiceId{};
        }
    }

    auto* voice = new ma_sound();
    const ma_uint32 flags = (placement == VoicePlacement::None) ? MA_SOUND_FLAG_NO_SPATIALIZATION : 0;
    if (ma_sound_init_copy(system.Engine(), source->Prototype, flags, m_Group, voice) != MA_SUCCESS)
    {
        delete voice;
        return VoiceId{};
    }

    ma_sound_set_volume(voice, settings.Volume);
    ma_sound_set_pitch(voice, std::max(0.01f, settings.Pitch));
    ma_sound_set_looping(voice, settings.Looping ? MA_TRUE : MA_FALSE);

    if (placement != VoicePlacement::None)
    {
        ma_sound_set_pinned_listener_index(voice, m_Listener);
        ma_sound_set_attenuation_model(voice, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(voice, std::max(settings.MinDistance, 1e-3f));
        ma_sound_set_max_distance(voice, std::max(settings.MaxDistance, settings.MinDistance));
        ma_sound_set_rolloff(voice, std::max(0.0f, settings.Rolloff));
        ma_sound_set_doppler_factor(voice, std::max(0.0f, settings.Doppler));
        ma_sound_set_cone(voice, settings.ConeInnerDegrees * DegreesToRadians,
                          settings.ConeOuterDegrees * DegreesToRadians, settings.ConeOuterGain);
        ma_sound_set_position(voice, position.X, position.Y, position.Z);
        ma_sound_set_velocity(voice, 0.0f, 0.0f, 0.0f);
    }

    uint32_t index;
    if (!m_Free.empty())
    {
        index = m_Free.back();
        m_Free.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_Voices.size());
        m_Voices.emplace_back();
    }

    VoiceRecord& record = m_Voices[index];
    const uint32_t generation = (record.Generation + 1 == 0) ? 1 : record.Generation + 1;
    record = candidate;
    record.Generation = generation;
    record.Alive = true;
    record.Sound = voice;
    record.Order = m_NextOrder++;
    record.NextOcclusionCheck = m_Clock;

    if (startVirtual)
    {
        record.Virtual = true;
        record.VirtualSince = m_Clock;
        record.VirtualCursor = 0;
    }
    else
    {
        ma_sound_start(voice);
    }

    return VoiceId{ index, record.Generation };
}

void WorldAudio::Stop(const VoiceId& voice, float fadeSeconds)
{
    VoiceRecord* record = Resolve(voice);
    if (record == nullptr || record->Stopping)
    {
        return;
    }
    if (record->Virtual)
    {
        // Already silent; nothing to fade.
        Release(voice.Index);
        return;
    }
    const uint32_t milliseconds = static_cast<uint32_t>(std::max(0.005f, fadeSeconds) * 1000.0f);
    ma_sound_stop_with_fade_in_milliseconds(record->Sound, milliseconds);
    record->Stopping = true;
}

void WorldAudio::StopAll(float fadeSeconds)
{
    for (uint32_t index = 0; index < m_Voices.size(); ++index)
    {
        if (m_Voices[index].Alive)
        {
            Stop(VoiceId{ index, m_Voices[index].Generation }, fadeSeconds);
        }
    }
}

bool WorldAudio::IsPlaying(const VoiceId& voice) const
{
    const VoiceRecord* record = Resolve(voice);
    return record != nullptr && !record->Stopping;
}

bool WorldAudio::IsAudible(const VoiceId& voice) const
{
    const VoiceRecord* record = Resolve(voice);
    return record != nullptr && !record->Stopping && !record->Virtual;
}

bool WorldAudio::IsOccluded(const VoiceId& voice) const
{
    const VoiceRecord* record = Resolve(voice);
    return record != nullptr && !record->Stopping && !record->Virtual && record->Occluded;
}

void WorldAudio::SetVolume(const VoiceId& voice, float volume)
{
    VoiceRecord* record = Resolve(voice);
    if (record == nullptr || !std::isfinite(volume))
    {
        return;
    }
    record->UserVolume = std::max(0.0f, volume);
    if (!record->Virtual && !record->Stopping)
    {
        ApplyGain(*record, GainFadeMilliseconds);
    }
}

void WorldAudio::SetPosition(const VoiceId& voice, const Vec3& position)
{
    VoiceRecord* record = Resolve(voice);
    if (record == nullptr || record->Placement == VoicePlacement::None || !IsFinite(position))
    {
        return;
    }
    record->Placement = VoicePlacement::Point;
    record->Actor = ActorId{};
    record->Position = position;
    ma_sound_set_position(record->Sound, position.X, position.Y, position.Z);
}

void WorldAudio::PlayMusic(const std::string& path, float volume, float fadeSeconds)
{
    if (!IsReady())
    {
        return;
    }

    const std::string resolved = ResolveAsset(path, "audio", LogLevel::Warning);
    if (resolved.empty())
    {
        return;
    }

    auto* music = new ma_sound();
    const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(AudioSystem::Get().Engine(), resolved.c_str(), flags, m_Group, nullptr, music) !=
        MA_SUCCESS)
    {
        delete music;
        LogMessage(LogLevel::Warning, "audio", "Could not stream \"%s\"; the music is silent.", resolved.c_str());
        return;
    }

    const uint32_t milliseconds = static_cast<uint32_t>(std::max(0.005f, fadeSeconds) * 1000.0f);

    // The old track fades out as the new one fades in.
    StopMusic(fadeSeconds);

    ma_sound_set_looping(music, MA_TRUE);
    ma_sound_set_volume(music, std::max(0.0f, volume));
    ma_sound_set_fade_in_milliseconds(music, 0.0f, 1.0f, milliseconds);
    ma_sound_start(music);
    m_Music = music;
}

void WorldAudio::StopMusic(float fadeSeconds)
{
    if (m_Music == nullptr)
    {
        return;
    }
    const uint32_t milliseconds = static_cast<uint32_t>(std::max(0.005f, fadeSeconds) * 1000.0f);
    ma_sound_stop_with_fade_in_milliseconds(m_Music, milliseconds);
    m_FadingMusic.push_back(m_Music);
    m_Music = nullptr;
}

void WorldAudio::SetListener(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    m_ListenerPosition = position;
    m_ListenerForward = forward;
    m_ListenerUp = up;
}

void WorldAudio::LimitVoices()
{
    // Everyone who wants to play, most important first. A voice already
    // playing is counted a little louder, which keeps two near-equals from
    // swapping back and forth every frame.
    std::vector<VoiceRecord*> ranked;
    for (VoiceRecord& voice : m_Voices)
    {
        if (voice.Alive && !voice.Stopping)
        {
            ranked.push_back(&voice);
        }
    }

    auto Weight = [](const VoiceRecord* voice) {
        return voice->Audibility * (voice->Virtual ? 1.0f : PlayingBias);
    };
    std::stable_sort(ranked.begin(), ranked.end(), [&](const VoiceRecord* a, const VoiceRecord* b) {
        if (a->Settings.Priority != b->Settings.Priority)
        {
            return a->Settings.Priority > b->Settings.Priority;
        }
        if (Weight(a) != Weight(b))
        {
            return Weight(a) > Weight(b);
        }
        return a->Order < b->Order;
    });

    int budget = std::max(1, Settings.MaxVoices);
    for (VoiceRecord* voice : ranked)
    {
        const bool inRange = voice->Placement == VoicePlacement::None || voice->Audibility > 0.0f;

        // A one-shot already playing finishes where it is, in range or not;
        // it is short, and cutting it would be the only audible thing here.
        const bool wantsSlot = voice->Settings.Looping ? inRange : true;

        if (wantsSlot && budget > 0)
        {
            --budget;
            if (voice->Virtual)
            {
                Resume(*voice);
            }
            continue;
        }

        if (voice->Virtual)
        {
            continue;
        }

        if (voice->Settings.Looping)
        {
            Virtualize(*voice);
            if (inRange)
            {
                ++m_Stolen;
            }
        }
        else
        {
            ma_sound_stop_with_fade_in_milliseconds(voice->Sound, StopFadeMilliseconds);
            voice->Stopping = true;
            ++m_Stolen;
        }
    }
}

void WorldAudio::Update(const AudioFrame& frame)
{
    LUDIFEX_PROFILE("sound");

    if (!IsReady())
    {
        return;
    }

    ma_engine* engine = AudioSystem::Get().Engine();
    const float dt = frame.DeltaSeconds;
    m_Clock += std::max(0.0f, dt);

    if (Settings.Volume != m_AppliedVolume)
    {
        m_AppliedVolume = std::max(0.0f, Settings.Volume);
        ma_sound_group_set_fade_in_milliseconds(m_Group, -1.0f, m_AppliedVolume, GainFadeMilliseconds);
    }

    ma_spatializer_listener_set_speed_of_sound(&engine->listeners[m_Listener], std::max(1.0f, Settings.SpeedOfSound));

    // The listener, at the camera unless placed by hand, moving at the speed
    // the camera moved this frame.
    if (Settings.ListenerFollowsCamera && frame.HasCamera)
    {
        m_ListenerPosition = frame.CameraPosition;
        m_ListenerForward = frame.CameraForward;
        m_ListenerUp = frame.CameraUp;
    }

    Vec3 listenerVelocity;
    if (m_HasListenerHistory && dt > 0.0f)
    {
        listenerVelocity = ClampSpeed(Vec3{ (m_ListenerPosition.X - m_LastListenerPosition.X) / dt,
                                            (m_ListenerPosition.Y - m_LastListenerPosition.Y) / dt,
                                            (m_ListenerPosition.Z - m_LastListenerPosition.Z) / dt });
    }
    m_LastListenerPosition = m_ListenerPosition;
    m_HasListenerHistory = true;

    ma_engine_listener_set_position(engine, m_Listener, m_ListenerPosition.X, m_ListenerPosition.Y,
                                    m_ListenerPosition.Z);
    ma_engine_listener_set_direction(engine, m_Listener, m_ListenerForward.X, m_ListenerForward.Y,
                                     m_ListenerForward.Z);
    ma_engine_listener_set_world_up(engine, m_Listener, m_ListenerUp.X, m_ListenerUp.Y, m_ListenerUp.Z);
    ma_engine_listener_set_velocity(engine, m_Listener, listenerVelocity.X, listenerVelocity.Y, listenerVelocity.Z);

    for (uint32_t index = 0; index < m_Voices.size(); ++index)
    {
        VoiceRecord& voice = m_Voices[index];
        if (!voice.Alive)
        {
            continue;
        }

        // Faded out, or a one-shot that reached its end: reclaimed.
        if (voice.Stopping || (!voice.Virtual && !voice.Settings.Looping && ma_sound_at_end(voice.Sound)))
        {
            if (!ma_sound_is_playing(voice.Sound))
            {
                Release(index);
            }
            continue;
        }

        if (voice.Placement == VoicePlacement::Actor)
        {
            Vec3 position;
            Vec3 velocity;
            Vec3 forward;
            if (!frame.ResolveActor || !frame.ResolveActor(voice.Actor, position, velocity, forward))
            {
                // The actor is gone, and its sound goes with it.
                if (voice.Virtual)
                {
                    Release(index);
                }
                else
                {
                    ma_sound_stop_with_fade_in_milliseconds(voice.Sound, StopFadeMilliseconds);
                    voice.Stopping = true;
                }
                continue;
            }
            voice.Position = position;
            voice.Velocity = ClampSpeed(velocity);
            voice.Forward = forward;
            ma_sound_set_position(voice.Sound, position.X, position.Y, position.Z);
            ma_sound_set_velocity(voice.Sound, voice.Velocity.X, voice.Velocity.Y, voice.Velocity.Z);
            ma_sound_set_direction(voice.Sound, forward.X, forward.Y, forward.Z);
        }

        // Occlusion: a line from the listener to the sound, checked a few times
        // a second, with the level ramped rather than switched. A virtual voice
        // is not being heard, so it is not worth a ray; it keeps the last
        // answer and asks again as soon as it resumes.
        if (voice.Settings.Occlusion && !voice.Virtual && voice.Placement != VoicePlacement::None &&
            frame.IsBlocked && m_Clock >= voice.NextOcclusionCheck)
        {
            voice.NextOcclusionCheck = m_Clock + OcclusionInterval;
            const bool blocked = frame.IsBlocked(m_ListenerPosition, voice.Position, voice.Actor);
            if (blocked != voice.Occluded)
            {
                voice.Occluded = blocked;
                voice.OcclusionGain = blocked ? std::clamp(Settings.OcclusionGain, 0.0f, 1.0f) : 1.0f;
                if (!voice.Virtual)
                {
                    ApplyGain(voice, GainFadeMilliseconds * 2);
                }
            }
        }

        voice.Audibility = AudibilityOf(voice);
    }

    LimitVoices();

    for (auto music = m_FadingMusic.begin(); music != m_FadingMusic.end();)
    {
        if (!ma_sound_is_playing(*music))
        {
            ma_sound_uninit(*music);
            delete *music;
            music = m_FadingMusic.erase(music);
        }
        else
        {
            ++music;
        }
    }
}

AudioStats WorldAudio::GetStats() const
{
    AudioStats stats;
    for (const VoiceRecord& voice : m_Voices)
    {
        if (!voice.Alive || voice.Stopping)
        {
            continue;
        }
        if (voice.Virtual)
        {
            ++stats.Virtual;
        }
        else
        {
            ++stats.Playing;
        }
        if (voice.Occluded && !voice.Virtual)
        {
            ++stats.Occluded;
        }
    }
    stats.Culled = m_Culled;
    stats.Stolen = m_Stolen;
    return stats;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

SoundId LoadSound(const std::string& path)
{
    return detail::AudioSystem::Get().Load(path);
}

void DestroySound(SoundId sound)
{
    detail::AudioSystem::Get().Destroy(sound);
}

float GetSoundDuration(SoundId sound)
{
    const detail::SoundRecord* record = detail::AudioSystem::Get().Resolve(sound);
    return record != nullptr ? record->Duration : 0.0f;
}

void ConfigureAudio(const AudioConfig& config)
{
    detail::AudioSystem& system = detail::AudioSystem::Get();
    if (system.Engine() != nullptr)
    {
        LogMessage(LogLevel::Warning, "audio",
                   "ConfigureAudio was called after sound had started; it applies only before the first sound.");
        return;
    }
    system.Config = config;
}

bool MixAudio(float* frames, uint32_t frameCount)
{
    detail::AudioSystem& system = detail::AudioSystem::Get();
    if (frames == nullptr || !system.Ensure() || !system.IsManual())
    {
        return false;
    }
    ma_engine_read_pcm_frames(system.Engine(), frames, frameCount, nullptr);
    return true;
}

} // namespace ludifex
