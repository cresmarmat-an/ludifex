// World audio: sounds, voices, the listener, and the one audio engine.
// Not installed and not part of the public API.
//
// The engine is process-wide. With opane present it runs without a device and
// opane pulls its mix into its own output, so the process has one audio device
// and one final mix; without opane it opens the device itself. Each world owns
// its voices, a listener slot of its own, and a volume group, so two worlds
// never hear each other's sounds through the wrong ears.

#pragma once

#include "Internal.h"

#include <functional>
#include <string>
#include <vector>

struct ma_sound;

namespace ludifex::detail
{

enum class VoicePlacement
{
    None,   // heard as it is
    Point,  // at a fixed position
    Actor   // following an actor
};

// What a world tells its audio each frame.
struct AudioFrame
{
    float DeltaSeconds = 0.0f;

    bool HasCamera = false;
    Vec3 CameraPosition;
    Vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
    Vec3 CameraUp{ 0.0f, 1.0f, 0.0f };

    // Where an actor is, how fast it moves, and which way it faces. False when
    // the actor is gone.
    std::function<bool(const ActorId& actor, Vec3& position, Vec3& velocity, Vec3& forward)> ResolveActor;

    // Whether something solid lies between two points, not counting the
    // given actor.
    std::function<bool(const Vec3& from, const Vec3& to, const ActorId& ignore)> IsBlocked;
};

struct VoiceRecord
{
    uint32_t Generation = 0;
    bool Alive = false;

    ma_sound* Sound = nullptr;
    SoundSettings Settings;
    VoicePlacement Placement = VoicePlacement::None;
    Vec3 Position;
    Vec3 Velocity;
    Vec3 Forward{ 0.0f, 0.0f, -1.0f };
    ActorId Actor;

    // Playing through the mix, versus waiting silently (a looping voice that
    // is out of range or gave way). A virtual voice keeps track of where it
    // would be, so it resumes in step rather than from the start.
    bool Virtual = false;
    double VirtualSince = 0.0;
    uint64_t VirtualCursor = 0;

    // Fading out, to be reclaimed once silent.
    bool Stopping = false;

    float UserVolume = 1.0f;
    float OcclusionGain = 1.0f;
    bool Occluded = false;
    double NextOcclusionCheck = 0.0;

    // How loud it is at the listener, for deciding who plays.
    float Audibility = 0.0f;
    uint64_t Order = 0;
};

class WorldAudio
{
public:
    WorldAudio();
    ~WorldAudio();

    WorldAudio(const WorldAudio&) = delete;
    WorldAudio& operator=(const WorldAudio&) = delete;

    bool IsReady() const { return m_Group != nullptr; }

    VoiceId Play(SoundId sound, const SoundSettings& settings, VoicePlacement placement, const Vec3& position,
                 const ActorId& actor);
    void Stop(const VoiceId& voice, float fadeSeconds);
    void StopAll(float fadeSeconds);
    bool IsPlaying(const VoiceId& voice) const;
    bool IsAudible(const VoiceId& voice) const;
    bool IsOccluded(const VoiceId& voice) const;
    void SetVolume(const VoiceId& voice, float volume);
    void SetPosition(const VoiceId& voice, const Vec3& position);

    void PlayMusic(const std::string& path, float volume, float fadeSeconds);
    void StopMusic(float fadeSeconds);

    void SetListener(const Vec3& position, const Vec3& forward, const Vec3& up);

    void Update(const AudioFrame& frame);

    AudioSettings Settings;
    AudioStats GetStats() const;

private:
    VoiceRecord* Resolve(const VoiceId& voice);
    const VoiceRecord* Resolve(const VoiceId& voice) const;
    void Release(uint32_t index);
    float AudibilityOf(const VoiceRecord& voice) const;
    void Virtualize(VoiceRecord& voice);
    bool Resume(VoiceRecord& voice);
    void ApplyGain(VoiceRecord& voice, uint32_t milliseconds);
    void LimitVoices();
    int CountPlaying() const;

    std::vector<VoiceRecord> m_Voices;
    std::vector<uint32_t> m_Free;

    ma_sound* m_Group = nullptr;
    float m_AppliedVolume = 1.0f;
    uint32_t m_Listener = 0;

    Vec3 m_ListenerPosition;
    Vec3 m_ListenerForward{ 0.0f, 0.0f, -1.0f };
    Vec3 m_ListenerUp{ 0.0f, 1.0f, 0.0f };
    Vec3 m_LastListenerPosition;
    bool m_HasListenerHistory = false;

    ma_sound* m_Music = nullptr;
    std::vector<ma_sound*> m_FadingMusic;

    double m_Clock = 0.0;
    uint64_t m_NextOrder = 0;

    int m_Culled = 0;
    int m_Stolen = 0;
};

// Lets go of the host's audio output while it still exists. Called from the
// host's release hook, before the host shuts its audio engine down.
void ReleaseAudioForHost();

} // namespace ludifex::detail
