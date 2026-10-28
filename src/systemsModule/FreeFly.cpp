#include "FreeFly.h"

#include <algorithm>

namespace SFE::FreeFly {
	Math::Vec3 offset(Move move, const Math::Vec3& forward, const Math::Vec3& right, float velocity) {
		Math::Vec3 dif = {};
		switch (move) {
		case Move::Forward:
			dif += forward * velocity;
			break;
		case Move::Backward:
			dif -= forward * velocity;
			break;
		case Move::Left:
			dif -= right * velocity;
			break;
		case Move::Right:
			dif += right * velocity;
			break;
		case Move::Up:
			dif += Math::Vec3{ 0.f, 1.f, 0.f } * velocity;
			break;
		case Move::Down:
			dif -= Math::Vec3{ 0.f, 1.f, 0.f } * velocity;
			break;
		}
		return dif;
	}

	Math::Vec3 look(const Math::Vec3& rotate, float dx, float dy, float sensitivity, bool constrainPitch) {
		dx *= sensitivity;
		dy *= sensitivity;
		auto pitch = rotate.x;
		auto yaw = rotate.y;
		yaw -= dx;

		pitch += dy;

		if (constrainPitch) {
			pitch = (std::min)((std::max)(pitch, -90.f), 90.f);
		}
		return { pitch, yaw, 0.f };
	}

	float zoom(float fov, float wheel) {
		fov -= wheel;
		return (std::min)((std::max)(1.f, fov), 90.f);
	}
}
