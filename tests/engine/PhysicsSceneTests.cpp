#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include "EngineTestFixture.h"
#include "assetsModule/modelModule/Terrain.h"
#include "componentsModule/HeightfieldComponent.h"
#include "componentsModule/Hierarchy.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/TransformComponent.h"
#include "propertiesModule/PropertiesSystem.h"
#include "propertiesModule/SceneEvents.h"
#include "systemsModule/TerrainCollider.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/PhysicsSystem.h"

namespace SFE::EngineTests {
	namespace {
		// Where the cube stands: far from anything another test puts in the world.
		constexpr float kAt = 2000.f;

		// A scene of one cube at (kAt, 0, kAt), its triangles a static collider, under a root of nothing else.
		std::filesystem::path writeCubeScene() {
			Json::Value cube;
			cube["id"] = "cube";
			auto& transform = cube["Properties"]["TransformComponent"];
			for (const float v : { kAt, 0.f, kAt }) {
				transform["Pos"].append(v);
			}
			for (int i = 0; i < 3; ++i) {
				transform["Scale"].append(1.f);
			}
			cube["Properties"]["ModelComponent"]["ModelPath"] = "models/cube.fbx";
			cube["Properties"]["ModelComponent"]["Collider"] = true;
			Json::Value scene;
			scene["id"] = "scene";
			scene["Children"].append(cube);
			const auto path = std::filesystem::temp_directory_path() / "sfe_physics_scene_test.json";
			std::ofstream out(path);
			out << scene.toStyledString();
			return path;
		}

		// Whether a ray straight down through where the cube stands meets anything in `world`; and the same a kilometre to
		// the side, where nothing does.
		bool hitsTheCube(const SystemsModule::PhysicsSystem& world) {
			return world.castRay({ kAt, 500.f, kAt }, { 0.f, -1000.f, 0.f }).has_value();
		}
		bool hitsBesideIt(const SystemsModule::PhysicsSystem& world) {
			return world.castRay({ kAt + 1000.f, 500.f, kAt }, { 0.f, -1000.f, 0.f }).has_value();
		}

		// Where a heightfield stands, far from anything else: 17 samples over 32 m, a tilted plane, at (kFieldAt, 1, kFieldAt).
		constexpr float kFieldAt = 3000.f;
		constexpr int kFieldSamples = 17;
		constexpr float kFieldSize = 32.f;
		// How near the collider's height is to the field's: Jolt keeps the heights in 8 bits a block of 2 x 2 cells
		// (TerrainCollider.cpp), 1.2 m of this plane over a block, so a few millimetres -- the tolerance the game's
		// TerrainCollider tests hold the same collider to.
		constexpr float kFieldHeightNear = 0.008f;
		std::shared_ptr<const std::vector<float>> fieldHeights() {
			std::vector<float> heights(static_cast<size_t>(kFieldSamples) * kFieldSamples);
			const float step = kFieldSize / static_cast<float>(kFieldSamples - 1);
			for (int j = 0; j < kFieldSamples; ++j) {
				for (int i = 0; i < kFieldSamples; ++i) {
					const float x = -kFieldSize * 0.5f + static_cast<float>(i) * step;
					const float z = -kFieldSize * 0.5f + static_cast<float>(j) * step;
					heights[static_cast<size_t>(j) * kFieldSamples + i] = 0.1f * x + 0.2f * z + 1.f;
				}
			}
			return std::make_shared<const std::vector<float>>(std::move(heights));
		}

		// Where a ray straight down at (x, z) first meets something in `world`: the height there, if anything.
		std::optional<float> groundUnder(const SystemsModule::PhysicsSystem& world, const float x, const float z) {
			const auto hit = world.castRay({ x, 50.f, z }, { 0.f, -100.f, 0.f });
			if (!hit) {
				return std::nullopt;
			}
			return 50.f - 100.f * *hit;
		}
	}

	// A scene loaded once the engine is up stands in the physics as static colliders, by the rule every system goes by --
	// take what is there, listen for what is loaded after: at once in a world that has not stepped, taken from the
	// registry by one made after, and by the next frame in the engine's two, which have stepped (replaceStatic). Its
	// bodies and entities are taken out after.
	TEST(Physics, StaticCollidersFromALoadedScene) {
		auto* physics = ECSHandler::getSystem<SystemsModule::PhysicsSystem>();
		auto* loose = ECSHandler::getSystem<SystemsModule::LoosePhysicsSystem>();
		ASSERT_NE(physics, nullptr);
		ASSERT_NE(loose, nullptr);
		// The engine's worlds have stepped, whatever ran before.
		step();
		ASSERT_FALSE(hitsTheCube(*physics)) << "something stands where the cube goes";
		ASSERT_FALSE(hitsTheCube(*loose)) << "something stands where the cube goes";

		std::vector<ecss::EntityId> loaded;
		// Taken out however the test ends, unless its own last checks did it: the bodies first -- an entity destroyed leaves
		// its own standing, as nothing tells the physics of it -- then the entities, off the octree as reloadScene takes
		// them out.
		const auto takeBodiesOut = [&] {
			for (const auto entity : loaded) {
				physics->replaceStatic(entity, {});
				loose->replaceStatic(entity, {});
			}
			step();
		};
		const auto takeEntitiesOut = [&] {
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(loaded);
			}
			ECSHandler::destroyEntities(loaded);
			step();
			loaded.clear();
		};
		const Finally takeOut([&] {
			if (!loaded.empty()) {
				takeBodiesOut();
				takeEntitiesOut();
			}
		});
		{
			// A world no SystemManager steps, made before the scene is loaded: nothing has stepped it.
			SystemsModule::PhysicsSystem unstepped;
			ASSERT_FALSE(hitsTheCube(unstepped));

			const auto path = writeCubeScene();
			const auto root = PropertiesModule::PropertiesSystem::loadScene(path.string());
			std::filesystem::remove(path);
			loaded = Hierarchy::descendants(root);
			loaded.insert(loaded.begin(), root);
			ASSERT_EQ(loaded.size(), 2u) << "the scene did not load";

			EXPECT_TRUE(hitsTheCube(unstepped)) << "not laid in at once in a world that has not stepped";
			EXPECT_FALSE(hitsBesideIt(unstepped));
			EXPECT_FALSE(hitsTheCube(*physics)) << "laid in at once in a world that steps, not between two of its steps";

			// And one made once the scene is there takes it from the registry.
			SystemsModule::PhysicsSystem later;
			EXPECT_TRUE(hitsTheCube(later)) << "not taken from what the registry held when the world was made";
		}

		step();
		EXPECT_TRUE(hitsTheCube(*physics)) << "not in the main world by the next frame";
		EXPECT_TRUE(hitsTheCube(*loose)) << "not in the loose world by the next frame";
		EXPECT_FALSE(hitsBesideIt(*physics));

		takeBodiesOut();
		EXPECT_FALSE(hitsTheCube(*physics)) << "its bodies outlived it";
		EXPECT_FALSE(hitsTheCube(*loose)) << "its bodies outlived it";
		takeEntitiesOut();
	}

	// A heightfield collides with no model to draw it, by the same rule as the scenes' colliders: taken by a world made
	// once it is there, a field of its heights (TerrainCollider) where its transform puts it, the height the field's
	// triangles have there; and by the engine's two worlds, which have stepped, when they are told of it as a scene
	// loaded after them, by the next frame.
	TEST(Physics, AHeightfieldCollidesWithoutAModel) {
		auto& registry = ECSHandler::registry();
		auto* physics = ECSHandler::getSystem<SystemsModule::PhysicsSystem>();
		auto* loose = ECSHandler::getSystem<SystemsModule::LoosePhysicsSystem>();
		ASSERT_NE(physics, nullptr);
		ASSERT_NE(loose, nullptr);
		step();
		ASSERT_FALSE(groundUnder(*physics, kFieldAt + 3.f, kFieldAt + 5.f)) << "something stands where the heightfield goes";

		const auto entity = registry.takeEntity();
		const Finally takeOut([&] {
			physics->replaceStatic(entity, {});
			loose->replaceStatic(entity, {});
			step();
			ECSHandler::destroyEntity(entity);
			step();
		});
		registry.addComponent<TransformComponent>(entity, entity);
		if (auto transform = registry.pinComponent<TransformComponent>(entity)) {
			transform->setPos({ kFieldAt, 1.f, kFieldAt });
		}
		const auto heights = fieldHeights();
		registry.addComponent<ComponentsModule::HeightfieldComponent>(entity, ComponentsModule::HeightfieldComponent{ kFieldSamples, kFieldSize, heights, 1 });
		step(3);
		Math::Mat4 world{ 1.f };
		{
			const auto matrix = registry.pinComponent<const ComponentsModule::TransformMatComp>(entity);
			ASSERT_TRUE(matrix) << "no render transform";
			world = matrix->mTransform;
		}
		ASSERT_FALSE(registry.hasComponent<ModelComponent>(entity));
		const float expected = 1.f + AssetsModule::heightfieldHeight(*heights, kFieldSamples, kFieldSize, 3.f, 5.f);

		const auto shapes = SystemsModule::PhysicsSystem::staticShapes(entity, world);
		ASSERT_EQ(shapes.size(), 1u);
		EXPECT_NE(SystemsModule::TerrainCollider::field(*shapes[0]), nullptr) << "not a field of its heights";
		{
			SystemsModule::PhysicsSystem later;
			const auto ground = groundUnder(later, kFieldAt + 3.f, kFieldAt + 5.f);
			ASSERT_TRUE(ground) << "not taken from what the registry held when the world was made";
			EXPECT_NEAR(*ground, expected, kFieldHeightNear);
		}
		EXPECT_FALSE(groundUnder(*physics, kFieldAt + 3.f, kFieldAt + 5.f)) << "taken by a world nothing told of it";

		PropertiesModule::emitSceneLoaded({ "", entity, { entity } });
		step();
		for (const auto* each : { static_cast<const SystemsModule::PhysicsSystem*>(physics), static_cast<const SystemsModule::PhysicsSystem*>(loose) }) {
			const auto ground = groundUnder(*each, kFieldAt + 3.f, kFieldAt + 5.f);
			ASSERT_TRUE(ground) << "not taken when told of by the next frame";
			EXPECT_NEAR(*ground, expected, kFieldHeightNear);
		}
	}

	// An application's upright capsules go by the scenes' rule too: what a source reports is in the world at once while
	// it has not stepped, and asked for again when a scene is loaded after -- at once before the world steps, between two
	// steps once it does -- the capsules then standing where the source now says, none where it said before.
	TEST(Physics, StaticCapsulesTakeWhatStandsAndListen) {
		Math::Vec3 at{ 5000.f, 0.f, 5000.f };
		const auto standsAt = [](const SystemsModule::PhysicsSystem& world, const Math::Vec3& place) {
			return groundUnder(world, place.x, place.z).has_value();
		};
		SystemsModule::PhysicsSystem made;
		ASSERT_FALSE(standsAt(made, at)) << "something stands where the capsule goes";
		made.addStaticCapsules([&at](std::vector<SystemsModule::PhysicsSystem::StaticCapsule>& out) { out.push_back({ at, 0.5f, 3.f }); });
		EXPECT_TRUE(standsAt(made, at)) << "not in at once in a world that has not stepped";

		const auto first = at;
		at = { 5010.f, 0.f, 5000.f };
		PropertiesModule::emitSceneLoaded({ "", ecss::INVALID_ID, {} });
		EXPECT_TRUE(standsAt(made, at)) << "not asked for again when a scene was loaded";
		EXPECT_FALSE(standsAt(made, first)) << "the capsule before still stands";

		made.update(kFrameTime);
		const auto second = at;
		at = { 5020.f, 0.f, 5000.f };
		PropertiesModule::emitSceneLoaded({ "", ecss::INVALID_ID, {} });
		EXPECT_TRUE(standsAt(made, second)) << "changed in a world that steps, not between two of its steps";
		EXPECT_FALSE(standsAt(made, at));
		made.update(kFrameTime);
		EXPECT_TRUE(standsAt(made, at)) << "not in by the next step";
		EXPECT_FALSE(standsAt(made, second)) << "the capsule before still stands";
	}
}
