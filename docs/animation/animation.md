# Animation

A model with a skeleton, morph targets, or both, and the clips that drive
them, can be animated. glTF files and the formats Assimp reads (FBX, Collada,
and others) are both supported. Other actors, such as boxes, spheres, and
models without clips, return 0 from `GetAnimationCount` and ignore the other
calls, so you can call them without checking what kind of actor you have.

The skeleton and the clips belong to the model and are shared. A hundred
guards loaded from one file hold one copy of the clips; each guard only stores
where it is in them.

```cpp
ludifex::Actor3D guard = world.AddModel({ .Path = "guard.gltf" });
guard.Play({ .Name = "walk" });
```

`Play` takes a struct with named fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `Name` | none | The clip, by the name in the file |
| `Index` | -1 | The clip by position, used when `Name` is empty |
| `Loop` | true | A clip that does not loop holds its last pose |
| `Speed` | 1 | Multiplies the clip's timing; negative plays it backwards |
| `StartTime` | 0 | Where in the clip to begin, in seconds |
| `Fade` | 0.2 | Seconds to crossfade from whatever is playing |
| `Weight` | 1 | How much of the final pose this clip accounts for |

## Blending

`Play` does not cut to the new clip. It fades the new clip in while everything
already playing fades out, over `Fade` seconds. Pass `.Fade = 0.0f` when you
want a hard cut, for example for a hit reaction that must show on the exact
frame.

```cpp
guard.Play({ .Name = "walk" });
// later: crossfades from walk to run
guard.Play({ .Name = "run", .Fade = 0.15f });
```

Up to four clips mix at once. Starting a fifth removes the one with the lowest
weight.

Weights are normalized across everything playing, so two clips at weight 1
mix half and half rather than adding up to double the motion. Rotations are
blended along the shortest arc between them, not component by component, so a
blend of two turns passes through the turn in between.

```cpp
guard.StopAnimation(0.3f);   // fade everything out over 0.3 seconds
guard.StopAnimation(0.0f);   // stop now and return to the model's rest pose
```

## Asking what it is doing

```cpp
if (door.IsAnimationFinished())
{
    // the non-looping clip has reached its end and is holding there
}
```

| Call | Returns |
| --- | --- |
| `GetAnimationCount()` | How many clips the model has, or 0 |
| `GetAnimationName(i)` | The name of clip `i` |
| `FindAnimation(name)` | Its index, or -1 if there is no clip with that name |
| `GetAnimationDuration(i)` | Its length in seconds at speed 1 |
| `IsAnimating()` | Whether any clip is playing |
| `IsAnimationFinished()` | Whether a non-looping clip has reached its end |
| `GetAnimationTime()` | The time of the clip with the highest weight, in seconds |
| `SetAnimationTime(s)` | Moves that clip to a given time |

`SetAnimationTime` lets something other than the clock drive a clip. For
example, a lever's pose can follow how far the player has pulled it.

## Attaching to a joint

To put a sword in a character's hand, read the hand joint after the world
updates and move the sword there:

```cpp
const int hand = knight.FindJoint("hand.R");

// each frame, after world.Update(dt)
const ludifex::Transform3 grip = knight.GetJointTransform(hand);
sword.SetPosition(grip.Position);
sword.SetRotation(grip.Rotation);
```

`GetJointTransform` returns the joint's place in the world, with the actor's
own transform applied. Read it after `Update`, or you will get last frame's
position. It returns the identity transform for an actor with no skeleton,
and for one that has not been posed yet.

`GetJointCount`, `GetJointName`, and `FindJoint` work like the clip functions.

## Morph targets

Morph targets (blend shapes) are alternative shapes a mesh can be blended
toward: a smile, a blink, a dent in a door. Each has a weight, 0 for the mesh
as built and 1 for the full shape. Values outside 0 to 1 exaggerate or invert
the shape, as glTF allows.

```cpp
ludifex::Actor3D face = world.AddModel({ .Path = "face.gltf" });

face.GetMorphTargetCount();
face.GetMorphTargetName(0);                  // names from the file
const int blink = face.FindMorphTarget("Blink");

face.SetMorphWeight("Smile", 0.8f);
face.SetMorphWeight(blink, 1.0f);
face.GetMorphWeight(blink);                  // the weight being drawn
```

A model starts with the weights its file gives. A weight you set with
`SetMorphWeight` belongs to the actor. A playing clip that animates the same
weight blends over it in proportion to the clip's weight, and hands it back
when the clip stops. A glTF clip animates every weight of each mesh it
targets, so a clip on a face drives all of the face's shapes while it plays
and leaves other meshes alone.

When two nodes show the same mesh, each has its own set of weights, and the
model lists the targets of both.

## How it runs

Skinning happens on the GPU in the same shader as everything else, so custom
materials work on animated models without any extra work. Every vertex has
four joint indices and four weights; meshes that do not bend simply have them
at zero. The joint matrices of every skinned actor go into one buffer each
frame, so many characters in different poses can still be drawn together.

Morph targets are also applied on the GPU, before skinning, as glTF
specifies. The shape data is uploaded once with the mesh. Each frame, only
the weights in use are sent, and a mesh at rest costs nothing extra.

Poses advance with the `deltaSeconds` passed to `Update`, not with the fixed
physics step, so animation stays smooth at any frame rate. Shadows are cast
from the animated pose.

## Limitations

- A model can have at most 255 joints across all its skins, and each vertex
  follows at most four joints.
- Two characters in one file are one actor and animate together. Export one
  character per file to move them separately.
- Translation, rotation, scale, and morph-weight channels are read, with
  step, linear, and cubic spline interpolation. Other animated properties,
  such as material colours, cameras, or lights, are ignored.
- There is no root motion, inverse kinematics, animation state machine,
  additive blending, per-joint masking (such as upper body only), or events
  at points in a clip. Build these in your own code with `Play`, weights,
  `SetAnimationTime`, and `GetAnimationTime`.
- The speed of a playing clip cannot be changed. Call `Play` again with the
  new `Speed` and `StartTime` set to the current time.
- Animation keeps running while physics is stopped. Passing 0 to `Update`
  holds both.
- Motion vectors include the actor's movement but not a limb's own movement
  within the pose, so temporal anti-aliasing can blur fast-moving limbs
  slightly.
- The collider and the bounds used for culling come from the model's rest
  pose. A pose or morph that moves the mesh far outside them can be culled
  at the edge of the view, and does not change the collider.
- Posing a skinned model on the CPU for [ray tracing](../rendering/ray-tracing.md)
  adds cost for each animated character in view.
