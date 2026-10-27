#pragma once

#include <atomic>

#include "core/InputHandler.h"
#include "mathModule/Forward.h"
#include "systemsModule/SystemBase.h"

namespace SFE::SystemsModule {
	class PhysicsSystem;

	// What the physics worlds actually hold, drawn over the picture: every body's own shape in wireframe, fed to the
	// engine's debug pass. F3 turns it on and off.
	//
	// The shapes are asked for their triangles rather than guessed at, so a hull, a box, a sphere and a capsule all
	// come out right, whatever the world was given. The main world is drawn in one colour and the loose one -- the many
	// small bodies strewn about (LoosePhysicsSystem) -- in another, so it is plain which world a body belongs to.
	class PhysicsDrawSystem : public ecss::System, CoreModule::InputObserver {
	public:
		PhysicsDrawSystem();
		void update(float dt) override;

		// Whether the drawing is on, for anything else that wants to show its own workings alongside.
		static bool shown() { return sShow.load(std::memory_order_relaxed); }

	private:
		void drawWorld(PhysicsSystem* physics, const Math::Vec4& colour) const;

		inline static std::atomic_bool sShow{ false };
	};
}
