# The transform hierarchy

An actor can be parented to another. The child keeps its place relative to the
parent, and whatever moves the parent (physics, a setter, or its own parent)
carries the child with it: a lamp on a cart, a turret on a tank, a sign on a
wall.

```cpp
lamp.SetParent(cart);       // applied at the next sync point
flame.SetParent(lamp);      // parents can have parents

cart.SetPosition({ 20.0f, 0.5f, 0.0f });   // the lamp and its flame move too
```

Parenting does not move anything. The offset between child and parent is
measured at the sync point and kept from then on. Detaching leaves the child
where it is in the world.

```cpp
lamp.GetParent();
cart.GetChildren();

lamp.GetLocalPosition();                   // its place on the cart
lamp.SetLocalPosition({ 0.8f, 0.7f, 0.0f });
lamp.GetLocalRotation();
lamp.SetLocalRotation(ludifex::Quat::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, angle));

lamp.ClearParent();
```

Without a parent, the local position and rotation are the world position and
rotation.

## Destroying

Destroying a parent destroys everything parented to it, deepest first, so
nothing is left floating where the parent used to be.

## Dynamic bodies

Physics owns a dynamic body's transform, so a dynamic child is **not**
carried: a warning is logged once, and the child is left to the solver. Use
the hierarchy for static and kinematic actors. To hold two dynamic bodies
together, use a [weld joint](../physics/joints.md).

## Cycles

Parenting an actor to one of its own descendants is refused with a message,
and nothing changes.

## Limitations

- The hierarchy is for 3D worlds only.
- Children follow their parent's position and rotation, not its scale.
- Dynamic children are not carried; see above.
