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

	// Both worlds stand their bodies by the rules of PhysicsLayers.h, the ones a world made elsewhere (a test's) can be
	// given too: its layer pairs, its broad phase and what it looks for in it are those types.
	TEST(PhysicsLayers, TheWorldsKeepTheseRules) {
		for (auto* physics : worlds()) {
			ASSERT_NE(physics, nullptr);
			EXPECT_NE(dynamic_cast<const SystemsModule::PhysicsLayers::ObjectPairs*>(&physics->layerPairFilter()), nullptr);
			EXPECT_NE(dynamic_cast<const SystemsModule::PhysicsLayers::ObjectVsBroadPhase*>(&physics->broadPhaseFilter()), nullptr);
			EXPECT_NE(dynamic_cast<const SystemsModule::PhysicsLayers::BroadPhase*>(&physics->world().GetBroadPhaseLayerInterface()), nullptr);
		}
		EXPECT_EQ(PhysicsSystem::kStatic, SystemsModule::PhysicsLayers::kStatic);
		EXPECT_EQ(PhysicsSystem::kMoving, SystemsModule::PhysicsLayers::kMoving);
		EXPECT_EQ(PhysicsSystem::kHeld, SystemsModule::PhysicsLayers::kHeld);
		EXPECT_EQ(PhysicsSystem::kCharacter, SystemsModule::PhysicsLayers::kCharacter);
		EXPECT_TRUE(SystemsModule::PhysicsLayers::kStaticPhase == kStaticPhase);
		EXPECT_TRUE(SystemsModule::PhysicsLayers::kMovingPhase == kMovingPhase);
	}

	// A query through the world (a ray, a shape cast) by StaticOrMoving touches what stands and what moves, and looks through
	// what a hand holds and a character's own capsule.
	TEST(PhysicsLayers, AQueryTouchesWhatStandsAndWhatMoves) {
		const SystemsModule::PhysicsLayers::StaticOrMoving touched;
		EXPECT_TRUE(touched.ShouldCollide(PhysicsSystem::kStatic));
		EXPECT_TRUE(touched.ShouldCollide(PhysicsSystem::kMoving));
		EXPECT_FALSE(touched.ShouldCollide(PhysicsSystem::kHeld));
		EXPECT_FALSE(touched.ShouldCollide(PhysicsSystem::kCharacter));
	}
}
