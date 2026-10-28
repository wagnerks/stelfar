#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include "EngineTestFixture.h"
#include "assetsModule/modelModule/ModelLoader.h"
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

		// Where the order test's colliders stand, far from anything another test puts in the world, and how far round.
		constexpr float kOrderAt = 4000.f;
		constexpr float kOrderReach = 40.f;

		// A static body of a world: the kind of its shape, whether that is a heightfield's field, and its bounds in the world
		// -- where it stands. Not the shape's own bounds: those are round its centre of mass, which for a field (a shape
		// moved to where the field lies, its body at the world's origin, PhysicsSystem.cpp) is not that origin.
		struct Laid {
			JPH::EShapeSubType type{};
			bool field = false;
			JPH::AABox bounds;
		};
		// The static bodies of `physics` within kOrderReach of (kOrderAt, kOrderAt), in the order of their ids: the order
		// they were made in.
		std::vector<Laid> laidAtTheOrderTest(SystemsModule::PhysicsSystem& physics) {
			JPH::BodyIDVector ids;
			physics.world().GetBodies(ids);
			std::sort(ids.begin(), ids.end());
			std::vector<Laid> laid;
			auto& bodies = physics.world().GetBodyInterface();
			for (const auto& id : ids) {
				const auto shape = bodies.GetShape(id);
				if (!shape) {
					continue;
				}
				const auto bounds = shape->GetWorldSpaceBounds(bodies.GetCenterOfMassTransform(id), JPH::Vec3::sReplicate(1.f));
				const auto centre = bounds.GetCenter();
				if (std::abs(centre.GetX() - kOrderAt) > kOrderReach || std::abs(centre.GetZ() - kOrderAt) > kOrderReach) {
					continue;
				}
				laid.push_back({ shape->GetSubType(), SystemsModule::TerrainCollider::field(*shape) != nullptr, bounds });
			}
			return laid;
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

	// A scene told of after a world is made is laid in as a world made once the scene is there lays it, body for body,
	// whatever order its entities are told in: the models' bodies by id -- a child's where its parent puts it -- then the
	// heightfields no model draws, by id. Told here with the field among the models and the child before the second
	// model; then a world is made and takes the same from the registry.
	TEST(Physics, ASceneToldLaterLaysWhatOneThereLays) {
		auto& registry = ECSHandler::registry();
		auto* physics = ECSHandler::getSystem<SystemsModule::PhysicsSystem>();
		auto* loose = ECSHandler::getSystem<SystemsModule::LoosePhysicsSystem>();
		ASSERT_NE(physics, nullptr);
		ASSERT_NE(loose, nullptr);
		constexpr const char* kCube = "models/cube.fbx";
		ASSERT_NE(AssetsModule::ModelLoader::instance()->load(kCube), nullptr) << kCube << " did not load";
		step();

		// Made before the scene, so it is told of it; no SystemManager steps it, so it lays the scene in at once.
		SystemsModule::PhysicsSystem told;
		ASSERT_TRUE(laidAtTheOrderTest(told).empty()) << "something stands where the test's colliders go";

		// Taken out however the test ends: the bodies the engine's two worlds were told of, then the entities, off the
		// octree.
		std::vector<ecss::EntityId> made;
		const Finally takeOut([&] {
			for (const auto entity : made) {
				physics->replaceStatic(entity, {});
				loose->replaceStatic(entity, {});
			}
			step();
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(made);
			}
			ECSHandler::destroyEntities(made);
			step();
		});
		const auto cube = [&](const Math::Vec3& pos, const Math::Vec3& rotate) {
			const auto entity = PropertiesModule::PropertiesSystem::spawnModel(kCube, pos, Math::Vec3{ 1.f }, rotate);
			if (auto model = registry.pinComponent<ModelComponent>(entity)) {
				model->collider = true;
				made.push_back(entity);
				return entity;
			}
			return ecss::INVALID_ID;
		};
		const auto first = cube({ kOrderAt, 0.f, kOrderAt }, { 0.f, 30.f, 0.f });
		const auto second = cube({ kOrderAt + 12.f, 0.f, kOrderAt - 6.f }, {});
		const auto child = cube({ 0.f, 0.f, 6.f }, { 0.f, 0.f, 15.f });
		ASSERT_NE(first, ecss::INVALID_ID) << kCube << " not spawned";
		ASSERT_NE(second, ecss::INVALID_ID) << kCube << " not spawned";
		ASSERT_NE(child, ecss::INVALID_ID) << kCube << " not spawned";
		const auto field = registry.takeEntity();
		made.push_back(field);
		registry.addComponent<TransformComponent>(field, field);
		if (auto transform = registry.pinComponent<TransformComponent>(field)) {
			transform->setPos({ kOrderAt, -3.f, kOrderAt + 20.f });
		}
		registry.addComponent<ComponentsModule::HeightfieldComponent>(field, ComponentsModule::HeightfieldComponent{ kFieldSamples, kFieldSize, fieldHeights(), 1 });
		// The child hung under the first once that stands still, so it is worked out from where the first stands, and the
		// render copies of all four written.
		step();
		Hierarchy::setParent(child, first);
		if (auto transform = registry.pinComponent<TransformComponent>(child)) {
			transform->markDirty();
		}
		step(3);

		PropertiesModule::emitSceneLoaded({ "", first, { first, field, child, second } });
		SystemsModule::PhysicsSystem there;

		const auto toldLaid = laidAtTheOrderTest(told);
		const auto thereLaid = laidAtTheOrderTest(there);
		ASSERT_GE(thereLaid.size(), 4u) << "not laid in by a world made once the scene is there";
		EXPECT_TRUE(thereLaid.back().field) << "the heightfield not after the models";
		ASSERT_EQ(toldLaid.size(), thereLaid.size()) << "not all laid in when told of";
		for (size_t i = 0; i < thereLaid.size(); ++i) {
			EXPECT_EQ(toldLaid[i].type, thereLaid[i].type) << "body " << i << ": laid in another order";
			EXPECT_EQ(toldLaid[i].field, thereLaid[i].field) << "body " << i << ": laid in another order";
			EXPECT_TRUE(toldLaid[i].bounds == thereLaid[i].bounds) << "body " << i << ": laid in another order";
		}
		// And they stand alike: a grid of rays down over the place meets the same heights in both.
		int hits = 0;
		for (int i = 0; i < 5; ++i) {
			for (int j = 0; j < 5; ++j) {
				const float x = kOrderAt - 10.f + 7.5f * static_cast<float>(i);
				const float z = kOrderAt - 10.f + 7.5f * static_cast<float>(j);
				const auto inThere = groundUnder(there, x, z);
				EXPECT_EQ(groundUnder(told, x, z), inThere) << "at " << x << ", " << z;
				hits += inThere ? 1 : 0;
			}
		}
		EXPECT_GT(hits, 0) << "no ray met anything: the heights above prove nothing";
	}
}
