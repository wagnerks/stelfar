#pragma once

#include "mathModule/Forward.h"

// How the free camera (FreeCameraController) moves for the keys held, the mouse and the wheel. Pure: no ECS, no input.
namespace SFE::FreeFly {
	enum class Move { Forward, Backward, Left, Right, Up, Down };

	// How far one held key moves a camera looking along `forward` (right: `right`) at `velocity` (speed x step):
	// from a zero offset, as CameraSystem did; world up for Up/Down.
	Math::Vec3 offset(Move move, const Math::Vec3& forward, const Math::Vec3& right, float velocity);

	// The angles (pitch, yaw, 0) after the mouse moved (dx, dy): each times `sensitivity`, yaw less dx, pitch plus dy,
	// the pitch held within +-90 when `constrainPitch`.
	Math::Vec3 look(const Math::Vec3& rotate, float dx, float dy, float sensitivity, bool constrainPitch = true);

	// The field of view after the wheel turned by `wheel`, within [1, 90] degrees.
	float zoom(float fov, float wheel);
}
