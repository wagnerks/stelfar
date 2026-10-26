#include <iterator>
#include <vector>

#include "EngineTestFixture.h"
#include "systemsModule/systems/PhysicsSystem.h"

namespace SFE::EngineTests {
	namespace {
		using SystemsModule::PhysicsSystem;

		constexpr JPH::ObjectLayer kLayers[] = { PhysicsSystem::kStatic, PhysicsSystem::kMoving, PhysicsSystem::kHeld, PhysicsSystem::kCharacter };
		// Which of kLayers meet which, in that order down and across.
		constexpr bool kMeet[4][4] = {
			{ false, true, false, true },   // static: only what moves
			{ true, true, false, true },    // moving
			{ false, false, false, false }, // held: nothing
			{ true, true, false, true },    // a character's capsule
		};
		// The broad phase's two layers, as the physics numbers them.
		constexpr JPH::BroadPhaseLayer kStaticPhase{ 0 };
		constexpr JPH::BroadPhaseLayer kMovingPhase{ 1 };

		// Both worlds, the characters' and the loose bodies'.
		std::vector<PhysicsSystem*> worlds() {
			return { ECSHandler::getSystem<PhysicsSystem>(), ECSHandler::getSystem<SystemsModule::LoosePhysicsSystem>() };
		}
	}

	// What never moves meets only what does, what is carried meets nothing, and all else meets all else.
	TEST(PhysicsLayers, WhichLayersMeet) {
		for (auto* physics : worlds()) {
			ASSERT_NE(physics, nullptr);
			const auto& pairs = physics->layerPairFilter();
			for (size_t a = 0; a < std::size(kLayers); ++a) {
				for (size_t b = 0; b < std::size(kLayers); ++b) {
					EXPECT_EQ(pairs.ShouldCollide(kLayers[a], kLayers[b]), kMeet[a][b]) << "layers " << kLayers[a] << " and " << kLayers[b];
				}
			}
		}
	}

	// Two broad-phase layers, what never moves in one and all else in the other. What never moves is looked for only
	// among what moves, and what is carried nowhere.
	TEST(PhysicsLayers, TheBroadPhase) {
		for (auto* physics : worlds()) {
			ASSERT_NE(physics, nullptr);
			const auto& layers = physics->world().GetBroadPhaseLayerInterface();
			EXPECT_EQ(layers.GetNumBroadPhaseLayers(), 2u);
			EXPECT_TRUE(layers.GetBroadPhaseLayer(PhysicsSystem::kStatic) == kStaticPhase);
			for (const auto layer : { PhysicsSystem::kMoving, PhysicsSystem::kHeld, PhysicsSystem::kCharacter }) {
				EXPECT_TRUE(layers.GetBroadPhaseLayer(layer) == kMovingPhase) << "layer " << layer;
			}
			const auto& against = physics->broadPhaseFilter();
			EXPECT_FALSE(against.ShouldCollide(PhysicsSystem::kStatic, kStaticPhase));
			EXPECT_TRUE(against.ShouldCollide(PhysicsSystem::kStatic, kMovingPhase));
			EXPECT_FALSE(against.ShouldCollide(PhysicsSystem::kHeld, kStaticPhase));
			EXPECT_FALSE(against.ShouldCollide(PhysicsSystem::kHeld, kMovingPhase));
			for (const auto layer : { PhysicsSystem::kMoving, PhysicsSystem::kCharacter }) {
				EXPECT_TRUE(against.ShouldCollide(layer, kStaticPhase)) << "layer " << layer;
				EXPECT_TRUE(against.ShouldCollide(layer, kMovingPhase)) << "layer " << layer;
			}
		}
	}

	// Hair hung off a character meets all a moving body meets but the character's own capsule.
	TEST(PhysicsLayers, HairMeetsAllButTheCapsule) {
		for (auto* physics : worlds()) {
			ASSERT_NE(physics, nullptr);
			const auto& pairs = physics->layerPairFilter();
			for (const auto other : { PhysicsSystem::kStatic, PhysicsSystem::kMoving, PhysicsSystem::kHair }) {
				EXPECT_TRUE(pairs.ShouldCollide(PhysicsSystem::kHair, other)) << "layer " << other;
				EXPECT_TRUE(pairs.ShouldCollide(other, PhysicsSystem::kHair)) << "layer " << other;
			}
			for (const auto other : { PhysicsSystem::kHeld, PhysicsSystem::kCharacter }) {
				EXPECT_FALSE(pairs.ShouldCollide(PhysicsSystem::kHair, other)) << "layer " << other;
				EXPECT_FALSE(pairs.ShouldCollide(other, PhysicsSystem::kHair)) << "layer " << other;
			}
			EXPECT_TRUE(physics->world().GetBroadPhaseLayerInterface().GetBroadPhaseLayer(PhysicsSystem::kHair) == kMovingPhase);
			EXPECT_TRUE(physics->broadPhaseFilter().ShouldCollide(PhysicsSystem::kHair, kStaticPhase));
			EXPECT_TRUE(physics->broadPhaseFilter().ShouldCollide(PhysicsSystem::kHair, kMovingPhase));
		}
	}
}
