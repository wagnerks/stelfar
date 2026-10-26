#include <filesystem>
#include <fstream>
#include <vector>

#include "EngineTestFixture.h"
#include "componentsModule/Hierarchy.h"
#include "propertiesModule/PropertiesSystem.h"
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
}
