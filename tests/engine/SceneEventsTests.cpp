#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "EngineTestFixture.h"
#include "componentsModule/Hierarchy.h"
#include "componentsModule/TransformComponent.h"
#include "propertiesModule/PropertiesSystem.h"
#include "propertiesModule/SceneEvents.h"
#include "systemsModule/systems/OcTreeSystem.h"

namespace SFE::EngineTests {
	using PropertiesModule::SceneLoaded;

	namespace {
		// A scene told of by name alone, with nothing in it: what the listeners hear is all that is tried.
		SceneLoaded named(const char* path) {
			return { path, ecss::INVALID_ID, {} };
		}

		Json::Value vec3(float x, float y, float z) {
			Json::Value value = Json::arrayValue;
			value.append(x);
			value.append(y);
			value.append(z);
			return value;
		}
	}

	// Listeners are told in the order they subscribed, and one forgotten is told no more while the others still are.
	TEST(SceneEvents, ListenersAreToldInTheOrderTheySubscribed) {
		// The owners stand for a system or a pass: only their addresses count.
		int first = 0;
		int second = 0;
		int third = 0;
		std::vector<std::string> told;
		PropertiesModule::onSceneLoaded(&first, [&](const SceneLoaded& scene) { told.push_back("first " + scene.path); });
		PropertiesModule::onSceneLoaded(&second, [&](const SceneLoaded& scene) { told.push_back("second " + scene.path); });
		PropertiesModule::onSceneLoaded(&third, [&](const SceneLoaded& scene) { told.push_back("third " + scene.path); });
		PropertiesModule::emitSceneLoaded(named("a"));
		PropertiesModule::forgetSceneListeners(&second);
		PropertiesModule::emitSceneLoaded(named("b"));
		PropertiesModule::forgetSceneListeners(&first);
		PropertiesModule::forgetSceneListeners(&third);
		PropertiesModule::emitSceneLoaded(named("c"));
		EXPECT_EQ(told, (std::vector<std::string>{ "first a", "second a", "third a", "first b", "third b" }));
	}

	// A listener subscribed while the listeners are being told hears from the next scene on, and one forgotten meanwhile
	// is not told of this one.
	TEST(SceneEvents, ListenersChangedWhileTellingCountFromTheNextScene) {
		int outer = 0;
		int inner = 0;
		int later = 0;
		std::vector<std::string> told;
		PropertiesModule::onSceneLoaded(&outer, [&](const SceneLoaded& scene) {
			told.push_back("outer " + scene.path);
			if (scene.path == "a") {
				PropertiesModule::onSceneLoaded(&inner, [&](const SceneLoaded& next) { told.push_back("inner " + next.path); });
				PropertiesModule::forgetSceneListeners(&later);
			}
		});
		PropertiesModule::onSceneLoaded(&later, [&](const SceneLoaded& scene) { told.push_back("later " + scene.path); });
		PropertiesModule::emitSceneLoaded(named("a"));
		PropertiesModule::emitSceneLoaded(named("b"));
		PropertiesModule::forgetSceneListeners(&outer);
		PropertiesModule::forgetSceneListeners(&inner);
		EXPECT_EQ(told, (std::vector<std::string>{ "outer a", "outer b", "inner b" }));
	}

	// loadScene tells of what it made once it stands where the file puts it: the file, the root, and every entity, the
	// root first and each after the one it hangs under.
	TEST(SceneEvents, LoadSceneTellsWhatItMade) {
		Json::Value leaf;
		leaf["id"] = "leaf";
		leaf["Properties"]["TransformComponent"]["Pos"] = vec3(1.f, 0.f, 0.f);
		Json::Value branch;
		branch["id"] = "branch";
		branch["Properties"]["TransformComponent"]["Pos"] = vec3(0.f, 2.f, 0.f);
		branch["Children"].append(leaf);
		Json::Value file;
		file["id"] = "scene";
		file["Children"].append(branch);
		const auto path = std::filesystem::temp_directory_path() / "sfe_scene_events_test.json";
		{
			std::ofstream out(path);
			out << file.toStyledString();
		}

		int owner = 0;
		std::vector<SceneLoaded> told;
		Math::Vec3 leafAt{ -1.f };
		PropertiesModule::onSceneLoaded(&owner, [&](const SceneLoaded& scene) {
			told.push_back(scene);
			if (scene.entities.empty()) {
				return;
			}
			if (const auto matrix = ECSHandler::registry().pinComponent<const ComponentsModule::TransformMatComp>(scene.entities.back())) {
				leafAt = { matrix->mTransform[3][0], matrix->mTransform[3][1], matrix->mTransform[3][2] };
			}
		});
		const auto root = PropertiesModule::PropertiesSystem::loadScene(path.string());
		PropertiesModule::forgetSceneListeners(&owner);
		std::filesystem::remove(path);
		const Finally takeOut([root] {
			if (root == ecss::INVALID_ID) {
				return;
			}
			auto made = Hierarchy::descendants(root);
			made.insert(made.begin(), root);
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(made);
			}
			ECSHandler::destroyEntities(made);
			step();
		});

		ASSERT_EQ(told.size(), 1u) << "not told once";
		const auto& heard = told.front();
		EXPECT_EQ(heard.path, path.string());
		EXPECT_EQ(heard.root, root);
		ASSERT_EQ(heard.entities.size(), 3u);
		EXPECT_EQ(heard.entities[0], root) << "the root not first";
		EXPECT_EQ(Hierarchy::parent(heard.entities[2]), heard.entities[1]) << "the leaf not after its branch";
		EXPECT_FLOAT_EQ(leafAt.x, 1.f) << "told before the leaf stood where the file puts it";
		EXPECT_FLOAT_EQ(leafAt.y, 2.f) << "told before the leaf stood where the file puts it";
	}
}
