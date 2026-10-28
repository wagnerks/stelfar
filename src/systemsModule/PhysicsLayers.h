#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

// The layers the physics worlds (PhysicsSystem) put their bodies on and which of them meet, on their own -- Jolt and
// nothing else -- so that a world made elsewhere, a test's, stands its bodies by the same rules.
namespace SFE::SystemsModule::PhysicsLayers {
	// What never moves, and what does (characters, rigid bodies); what touches nothing while it is moved by hand (a
	// thing carried), which still moves and is still found by what asks for kHeld; and a character's own standing
	// capsule, which moves like kMoving and meets all it meets.
	inline constexpr JPH::ObjectLayer kStatic = 0;
	inline constexpr JPH::ObjectLayer kMoving = 1;
	inline constexpr JPH::ObjectLayer kHeld = 2;
	inline constexpr JPH::ObjectLayer kCharacter = 3;
	// The broad phase's two layers: what never moves in one, all else in the other.
	inline constexpr JPH::BroadPhaseLayer kStaticPhase{ 0 };
	inline constexpr JPH::BroadPhaseLayer kMovingPhase{ 1 };

	struct BroadPhase final : JPH::BroadPhaseLayerInterface {
		JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
			return layer == kStatic ? kStaticPhase : kMovingPhase;
		}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
			return layer == kStaticPhase ? "static" : "moving";
		}
#endif
	};
	// Static against static is never tested, and what is held against nothing.
	struct ObjectVsBroadPhase final : JPH::ObjectVsBroadPhaseLayerFilter {
		bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer phase) const override {
			return layer != kHeld && (layer != kStatic || phase == kMovingPhase);
		}
	};
	// What is held meets nothing, and what never moves meets only what does.
	struct ObjectPairs final : JPH::ObjectLayerPairFilter {
		bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
			return a != kHeld && b != kHeld && (a != kStatic || b != kStatic);
		}
	};
	// What a query through the world touches: what stands and what moves -- not what is held, nor a character's capsule.
	struct StaticOrMoving final : JPH::ObjectLayerFilter {
		bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer == kStatic || layer == kMoving; }
	};
}
