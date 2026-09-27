# Sound

Each world plays its own sound. A sound placed in a world is heard from where
the camera is, follows the actor it is attached to, is turned down when
something solid is in the way, and changes pitch as it moves past. Sound is
updated by `Update` along with everything else, so there is nothing extra to
call each frame. 2D worlds have the same functions, with `Vec2` positions.

```cpp
const ludifex::SoundId chime = ludifex::LoadSound("sounds/chime.wav");

world.PlaySoundAt(chime, ludifex::Vec3{ 4.0f, 1.0f, -6.0f });
```

WAV, FLAC, MP3, and Ogg Vorbis files are supported, found through the same
[asset roots](../getting-started/finding-files.md) as everything else. A sound
is decoded once and kept in memory, so playing it many times is cheap. Music
is streamed from disk instead. A file that cannot be read gives an invalid id
and a warning, and playing that id does nothing.

```cpp
float seconds = ludifex::GetSoundDuration(chime);
ludifex::DestroySound(chime);   // voices already playing it finish
```

## Playing

There are three ways to play a sound. Each returns a `VoiceId` for that one
playing instance:

```cpp
// Not placed anywhere: interface sounds, narration, a stinger.
world.PlaySound(chime);

// At a fixed point.
world.PlaySoundAt(chime, ludifex::Vec3{ 4.0f, 1.0f, -6.0f });

// On an actor: it follows the actor, the actor's velocity drives the Doppler
// shift, and it fades out if the actor is destroyed.
world.PlaySoundAt(engine, car, { .Looping = true });
```

An invalid `VoiceId` is returned when nothing was started, for example a
one-shot too far away to hear, or one that lost to more important sounds.
This is normal, not an error.

```cpp
const ludifex::VoiceId voice = world.PlaySoundAt(siren, tower, { .Looping = true });

world.SetVoiceVolume(voice, 0.4f);              // changed smoothly, not in a step
world.SetVoicePosition(voice, { 0.0f, 3.0f, 0.0f });
world.IsPlaying(voice);                         // being mixed, or waiting silently
world.IsAudible(voice);                         // being mixed
world.IsOccluded(voice);                        // being mixed, with something in the way
world.StopSound(voice, 0.25f);                  // fade out over a quarter second
world.StopAllSounds();
```

`SoundSettings` describes one voice:

| Field | Default | Meaning |
| --- | --- | --- |
| `Volume` | 1 | Level of this voice |
| `Pitch` | 1 | Playback rate; 2 is an octave up |
| `Looping` | false | Repeats until stopped |
| `MinDistance` | 1 m | Full volume within this distance |
| `MaxDistance` | 60 m | Silent beyond this distance |
| `Rolloff` | 1 | How steeply it falls off between the two |
| `ConeInnerDegrees` | 360 | Full volume within this angle of where it faces |
| `ConeOuterDegrees` | 360 | `ConeOuterGain` beyond this angle, blended in between |
| `ConeOuterGain` | 0 | Level outside the cone |
| `Doppler` | 1 | How strongly motion shifts the pitch; 0 turns it off |
| `Occlusion` | false | Whether solid things in the way turn it down |
| `Priority` | 0 | Higher wins when there are too many voices |

A cone points down the actor's -Z axis, the way the actor faces. A loudspeaker
might use `{ .ConeInnerDegrees = 40.0f, .ConeOuterDegrees = 110.0f, .ConeOuterGain = 0.2f }`.

## The listener

By default the listener is at the camera and faces the way the camera faces,
which suits first-person and chase cameras. To place it yourself, turn that
off:

```cpp
ludifex::AudioSettings& audio = world.GetAudioSettings();
audio.ListenerFollowsCamera = false;

// each frame, at the character's head
world.SetListener(headPosition, facing);
```

Each world has its own listener. Up to four worlds can play sound at the same
time; any more share the first world's listener, and a warning is logged.

The listener's velocity is worked out from how it moves between frames, so a
moving camera produces its own Doppler shift. A sudden jump, such as a cut or
a teleport, is not treated as motion; speeds beyond what a listener could
plausibly move are ignored.

## Too many voices

`MaxVoices` sets how many voices are mixed at once. Beyond that, voices are
ranked by priority first, then by how loud each one is where the listener
stands, and the lowest give way. A voice that is already playing gets a small
advantage, so two sounds of nearly equal loudness do not keep swapping.

Giving way is not always stopping. A **looping** voice that loses, or that
moves out of range, becomes **virtual**: it is not mixed and costs nothing,
but it keeps track of where it would be. When it comes back, it continues from
that point and fades in. `IsPlaying` stays true the whole time; `IsAudible`
says whether it is being heard.

A **one-shot** is handled differently. One that starts out of range is never
started, and one that loses to a more important sound is stopped.

```cpp
ludifex::AudioStats stats = world.GetAudioStats();
std::printf("%d playing, %d waiting, %d gave way, %d culled, %d occluded\n",
            stats.Playing, stats.Virtual, stats.Stolen, stats.Culled, stats.Occluded);
```

| `AudioStats` | Meaning |
| --- | --- |
| `Playing` | Voices being mixed now |
| `Virtual` | Looping voices waiting silently |
| `Culled` | One-shots never started because they were out of range |
| `Stolen` | Voices that gave way to more important ones |
| `Occluded` | Voices being mixed with something in the way |

`Culled` and `Stolen` count from when the world was created; the other three
describe the current frame.

## Occlusion

A voice that asks for occlusion is turned down when something solid is
between it and the listener:

```cpp
world.PlaySoundAt(radio, room, { .Looping = true, .Occlusion = true });
```

A ray is cast from the listener to the sound ten times a second, and the level
changes smoothly, so walking past a doorway opens the sound up gradually.
Sensors are ignored, so trigger volumes never block sound. How far a blocked
voice is turned down is `AudioSettings::OcclusionGain`. A virtual voice is not
tested until it resumes.

## World settings

```cpp
ludifex::AudioSettings& audio = world.GetAudioSettings();
audio.MaxVoices = 24;
audio.Volume = 0.8f;
```

| `AudioSettings` | Default | Meaning |
| --- | --- | --- |
| `Volume` | 1 | Level of everything this world plays |
| `MaxVoices` | 32 | Voices mixed at once |
| `SpeedOfSound` | 343 m/s | Used for the Doppler shift |
| `ListenerFollowsCamera` | true | Listener at the camera, or placed with `SetListener` |
| `OcclusionGain` | 0.35 | Level of a blocked voice, 0 to 1 |

Changes take effect on the next `Update`, and level changes are ramped so they
do not click.

## Music

Music is streamed from disk, loops, and is not placed in the world:

```cpp
world.PlayMusic("music/rain.ogg", 0.6f, 2.0f);   // fade in over two seconds
world.StopMusic(1.5f);
```

Starting a new track fades the old one out while the new one fades in.

## Where the sound goes

When opane is running, a world's sound goes into opane's mix: one audio device
for the process, one final mix, and opane's master volume over everything.
Nothing has to be connected by hand. Without opane, ludifex opens an audio
device of its own.

For tests or recording, there is a third option. With manual output, nothing
goes to a device and your program pulls the mixed samples itself:

```cpp
ludifex::ConfigureAudio({ .Output = ludifex::AudioOutput::Manual,
                          .SampleRate = 48000, .Channels = 2 });

std::vector<float> block(480 * 2);              // 10 ms, interleaved stereo
ludifex::MixAudio(block.data(), 480);
```

Choose the output before the first sound plays.

## Avoiding clicks

Every change to a voice is ramped: starting, stopping, going virtual and
coming back, volume changes, and occlusion. No level jumps from one value to
another between two samples, which is what causes a click.

## Limitations

- Sound is positioned by stereo panning and volume. There is no HRTF, so
  above, below, and behind are not distinguishable, and there is no
  surround output.
- There is no reverb, echo, or filtering. Occlusion lowers the volume but
  does not muffle the high frequencies.
- Occlusion tests a single straight line, so sound does not travel around
  corners or through doorways that are off that line.
- A voice cannot be paused, sought, or have its pitch changed after it
  starts. Stop it and play it again instead.
- There are no mixer groups (such as separate music, effects, and voice
  levels) beyond the per-voice volume, the world's volume, and opane's
  master volume.
- Each world plays one music track at a time.
- At most four worlds have their own listener.
- There is no microphone input or audio recording from a device.
