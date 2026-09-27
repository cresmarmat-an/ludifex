# Characters

A player or a creature that walks is usually better as a character than as a
dynamic body. A dynamic body has to fight its own momentum: it slides down
slopes it should stand on, catches on seams between floor tiles, and tips over.
A character is a capsule that you move directly:

```cpp
ludifex::Character3D walker = world.AddCharacter({
    .Position = { 0.0f, 1.2f, 0.0f },   // the middle of the capsule
    .Radius = 0.35f,
    .Height = 1.8f,                     // total, including both rounded ends
    .StepHeight = 0.4f,                 // a ledge this tall is stepped over
    .SlopeLimitDegrees = 50.0f,         // anything steeper is treated as a wall
    .Name = "Player",
});

// Once a frame: where it should go.
ludifex::Vec3 moved = walker.Move({ walkX * dt, fallSpeed * dt, walkZ * dt });
```

`Move` slides along whatever is in the way, steps over ledges no taller than
`StepHeight`, treats slopes steeper than the limit as walls, and returns how far
the character actually moved. Use that return value to correct your own
velocity: walking into a wall returns no movement along the wall's normal, and
a jump that hits a ceiling returns no upward movement, so you know to stop
pushing in that direction.

```cpp
walker.IsOnGround();        // standing on ground no steeper than the slope limit
walker.GetGroundNormal();   // which way that ground faces
walker.GetPosition();
walker.SetPosition(spawn);  // moves it directly, without sliding
walker.GetActor();          // the capsule actor the rest of the world sees
walker.Destroy();
```

`IsOnGround` and `GetGroundNormal` describe the result of the last `Move`.

## Gravity and jumping

The character does not apply gravity. Jump height, fall speed, and how
jumping feels differ from game to game, so they are left to your program:

```cpp
if (walker.IsOnGround())
{
    fallSpeed = jumping ? JumpSpeed : -2.0f;   // a small downward push keeps it on slopes
}
else
{
    fallSpeed += Gravity * dt;
}
```

## How the world sees it

The character carries a kinematic capsule actor. It is drawn, can be given a
colour or a material, and pushes dynamic bodies it walks into. Dynamic bodies
land on it and bounce off it, and other characters block it like walls.

2D worlds have `Character2D`, which works the same way; see
[2D worlds](../worlds/2d-worlds.md#characters-in-2d).

## Limitations

- The shape is always a capsule, and its size cannot change after creation. To
  crouch, destroy the character and add a shorter one.
- The character is not pushed by dynamic bodies or carried by moving
  platforms; move it yourself.
- There is no built-in movement logic such as acceleration, air control, or
  jumping. `Move` only resolves where a requested movement ends up.
