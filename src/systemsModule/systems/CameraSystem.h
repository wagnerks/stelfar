#pragma once

#include "mathModule/Forward.h"
#include "systemsModule/SystemBase.h"

namespace SFE::SystemsModule {
	// The current camera, which everything draws and hears from: a default one made, placeCamera to put it.
	// It moves nothing by itself: FreeCameraController flies it, an application's controller (a character) carries it.
	class CameraSystem : public ecss::System {
	public:
		CameraSystem();

		void setCurrentCamera(ecss::EntityId camera);
		ecss::EntityId getCurrentCamera() const;

		// Puts the current camera at `pos` turned by `rotate` (Euler degrees) and tells everyone who follows it.
		void placeCamera(const Math::Vec3& pos, const Math::Vec3& rotate);

	private:
		ecss::EntityId mCurrentCamera = ecss::INVALID_ID;

		ecss::EntityId mDefaultCamera = ecss::INVALID_ID;
	};
}
