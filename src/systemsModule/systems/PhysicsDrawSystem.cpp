#include "PhysicsDrawSystem.h"

#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "core/ECSHandler.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/PhysicsSystem.h"

namespace SFE::SystemsModule {
	namespace {
		// Jolt hands triangles over in batches, and asks for at least this many at a time.
		constexpr int kBatch = 64;

		Math::Vec3 point(const JPH::Float3& from) {
			return { from.x, from.y, from.z };
		}
	}

	PhysicsDrawSystem::PhysicsDrawSystem() {
		onKeyEvent = [](CoreModule::InputKey key, CoreModule::InputEventType type) {
			if (key == CoreModule::InputKey::KEY_F3 && type == CoreModule::InputEventType::PRESS) {
				sShow.store(!sShow.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
		};
	}

	void PhysicsDrawSystem::drawWorld(PhysicsSystem* physics, const Math::Vec4& colour) const {
		if (!physics) {
			return;
		}
		auto& world = physics->world();
		JPH::BodyIDVector ids;
		world.GetBodies(ids);
		const auto& locks = world.GetBodyLockInterfaceNoLock();
		JPH::Float3 corners[kBatch * 3];
		for (const auto id : ids) {
			JPH::BodyLockRead hold(locks, id);
			if (!hold.Succeeded()) {
				continue;
			}
			const JPH::Body& body = hold.GetBody();
			const JPH::Shape* shape = body.GetShape();
			if (!shape) {
				continue;
			}
			// The triangles come back already standing where the body stands.
			JPH::Shape::GetTrianglesContext walk;
			shape->GetTrianglesStart(walk, JPH::AABox::sBiggest(), body.GetCenterOfMassPosition(),
				body.GetRotation(), JPH::Vec3::sReplicate(1.f));
			for (;;) {
				const int got = shape->GetTrianglesNext(walk, kBatch, corners);
				if (got <= 0) {
					break;
				}
				for (int i = 0; i < got; ++i) {
					const auto a = point(corners[i * 3 + 0]);
					const auto b = point(corners[i * 3 + 1]);
					const auto c = point(corners[i * 3 + 2]);
					Render::Utils::renderLine(a, b, colour, 1.f);
					Render::Utils::renderLine(b, c, colour, 1.f);
					Render::Utils::renderLine(c, a, colour, 1.f);
				}
			}
		}
	}

	void PhysicsDrawSystem::update(float) {
		if (!sShow.load(std::memory_order_relaxed)) {
			return;
		}
		drawWorld(ECSHandler::getSystem<PhysicsSystem>(), { 0.35f, 0.8f, 1.f, 0.6f });
		drawWorld(ECSHandler::getSystem<LoosePhysicsSystem>(), { 1.f, 0.55f, 0.15f, 0.6f });
	}
}
