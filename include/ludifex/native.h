// ludifex: access to the underlying physics libraries.
// Copyright (c) 2026 Cresmar Mat-an. MIT License; see LICENSE.
//
// Returns the raw Box2D and Box3D handles behind worlds and actors, for
// features ludifex does not wrap.
//
// Using it ties your code to the Box2D and Box3D versions ludifex is built
// with. Box3D is at v0.1.0 and its API is still changing; ludifex's own API
// hides those changes, but code using this header may need updating.

#pragma once

#include <ludifex/ludifex.h>

#include <box2d/box2d.h>
#include <box3d/box3d.h>

namespace ludifex::native
{

// Returns a zero-initialized (invalid) id when the world or actor is invalid.
// Test with b3World_IsValid / b3Body_IsValid before use.
b3WorldId GetNativeWorld(const World3D& world);
b3BodyId GetNativeBody(const Actor3D& actor);

b2WorldId GetNativeWorld(const World2D& world);
b2BodyId GetNativeBody(const Actor2D& actor);

} // namespace ludifex::native
