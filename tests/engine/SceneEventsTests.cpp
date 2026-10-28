#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "EngineTestFixture.h"
#include "componentsModule/DebugDataComponent.h"
#include "componentsModule/Hierarchy.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/SkyComponent.h"
#include "componentsModule/TransformComponent.h"
#include "propertiesModule/ComponentSerializer.h"
#include "propertiesModule/PropertiesSystem.h"
#include "propertiesModule/SceneEvents.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/FrameBlackboard.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"

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

		// `file` written where the tests write their scenes and loaded: its root, or INVALID_ID when it did not load.
		ecss::EntityId loadWritten(const Json::Value& file, const char* name) {
			const auto path = std::filesystem::temp_directory_path() / name;
			{
				std::ofstream out(path);
				out << file.toStyledString();
			}
			if (!std::filesystem::exists(path)) {
				return ecss::INVALID_ID;
			}
			const auto root = PropertiesModule::PropertiesSystem::loadScene(path.string());
			std::filesystem::remove(path);
			return root;
		}

		// What a scene made, taken out: off the octree, destroyed, and `frames` frames run after.
		void takeOutScene(const ecss::EntityId root, const int frames) {
			if (root == ecss::INVALID_ID) {
				return;
			}
			auto made = Hierarchy::descendants(root);
			made.insert(made.begin(), root);
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(made);
			}
			ECSHandler::destroyEntities(made);
			step(frames);
		}

		// A scene of a root with a sky and nothing else, its sun `elevation` degrees up and `azimuth` round.
		ecss::EntityId loadSky(const float elevation, const float azimuth) {
			Json::Value file;
			file["id"] = "sky";
			file["Properties"]["SkyComponent"]["sun_elevation"] = elevation;
			file["Properties"]["SkyComponent"]["sun_azimuth"] = azimuth;
			return loadWritten(file, "sfe_scene_events_sky_test.json");
		}
		// Taken out a few frames before the next test: the sky light baked again of no sky by then.
		constexpr int kSkySettles = 3;

		// The sun the shadow pass casts from, the entity it names "cascadeShadows".
		ecss::EntityId theSun() {
			for (const auto& [id, debug] : ECSHandler::registry().view<const DebugDataComponent>()) {
				if (debug && debug->stringId == "cascadeShadows") {
					return id;
				}
			}
			return ecss::INVALID_ID;
		}

		// A component of these tests' own, read once a node's model is bound (AfterBind) -- while fillTree has not hung
		// the node under its parent yet: the node's transform worked out there and then, and every worker's job let
		// finish. Each node of a scene that carries it then stands parentless and clean, its render copy written from
		// that, before the scene is told of: the timing a scene loaded after the renderer may have, made certain.
		constexpr const char* kWorkedOutAlone = "SceneEventsTestWorkedOutAlone";
		void addWorkedOutAlone() {
			static std::once_flag once;
			std::call_once(once, [] {
				PropertiesModule::ComponentRegistry::add({ kWorkedOutAlone, PropertiesModule::LoadPhase::AfterBind,
					[](ecss::EntityId, Json::Value&) {},
					[](const ecss::EntityId entity, const Json::Value&) {
						if (auto transform = ECSHandler::registry().pinComponent<TransformComponent>(entity)) {
							transform->markDirty();
							transform->reloadTransform();
						}
						ThreadPool::instance()->waitIdle();
					} });
			});
		}

		// A node of the scene the transform test writes: its id, its parent (an index into kNested, -1 for the root),
		// and where it stands under that parent. Values a float holds exactly, so the file gives them back as they are.
		struct Node {
			const char* id;
			int parent;
			Math::Vec3 pos;
			Math::Vec3 rotate;
			Math::Vec3 scale;
		};
		constexpr size_t kNestedNodes = 5;
		const std::array<Node, kNestedNodes> kNested{ {
			{ "scene", -1, { 3.f, 1.f, -2.f }, { 10.f, 30.f, 5.f }, { 2.f, 2.f, 2.f } },
			{ "A", 0, { 1.f, 0.5f, 0.f }, { 0.f, 45.f, 0.f }, { 1.f, 1.5f, 1.f } },
			{ "A1", 1, { 0.f, 2.f, 1.f }, { 15.f, 0.f, 0.f }, { 1.f, 1.f, 1.f } },
			{ "A2", 1, { -1.f, 0.f, 0.25f }, { 0.f, 0.f, 20.f }, { 0.5f, 0.5f, 0.5f } },
			{ "B", 0, { 4.f, 0.f, 4.f }, { 0.f, -60.f, 0.f }, { 1.f, 1.f, 1.f } },
		} };

		// kNested[index] and the nodes under it, as a scene file has them.
		Json::Value nestedNode(const int index) {
			const auto& node = kNested[static_cast<size_t>(index)];
			Json::Value value;
			value["id"] = node.id;
			auto& transform = value["Properties"]["TransformComponent"];
			transform["Pos"] = vec3(node.pos.x, node.pos.y, node.pos.z);
			transform["Rotate"] = vec3(node.rotate.x, node.rotate.y, node.rotate.z);
			transform["Scale"] = vec3(node.scale.x, node.scale.y, node.scale.z);
			value["Properties"][kWorkedOutAlone] = Json::Value(Json::objectValue);
			for (size_t i = 0; i < kNested.size(); ++i) {
				if (kNested[i].parent == index) {
					value["Children"].append(nestedNode(static_cast<int>(i)));
				}
			}
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

	// A scene loaded once the renderer is made has its sun placed when it is told of, as the shadow pass places the sun
	// of a scene there when it is made: before any frame, turned to where the scene's sky has it and coloured by the air
	// it shines through (CascadedShadowPass).
	TEST(SceneEvents, ASceneToldLaterPlacesTheSun) {
		auto& registry = ECSHandler::registry();
		step();
		ASSERT_FALSE(Render::Atmosphere::sceneSky(false)) << "a sky in the world before the test's";
		const auto sun = theSun();
		ASSERT_NE(sun, ecss::INVALID_ID) << "no sun the shadows are cast from";
		// A sun that stands elsewhere than the sun does now, so the test cannot pass by its standing there already.
		ComponentsModule::SkyComponent file;
		file.sunAzimuth = 133.f;
		{
			const auto transform = registry.pinComponent<const TransformComponent>(sun);
			ASSERT_TRUE(transform);
			for (const float elevation : { 21.5f, 33.25f }) {
				file.sunElevation = elevation;
				if (!(transform->getRotate() == file.sunRotation())) {
					break;
				}
			}
			ASSERT_FALSE(transform->getRotate() == file.sunRotation());
		}

		const auto root = loadSky(file.sunElevation, file.sunAzimuth);
		const Finally takeOut([root] { takeOutScene(root, kSkySettles); });
		ASSERT_TRUE(registry.hasComponent<ComponentsModule::SkyComponent>(root)) << "the scene did not load";
		// The scene's sky as the passes see it.
		const auto sky = Render::Atmosphere::sceneSky();
		ASSERT_TRUE(sky);
		const auto rotation = sky->sunRotation();
		const auto colour = Render::Atmosphere::sunTransmittance(sky->toSun());
		const auto transform = registry.pinComponent<const TransformComponent>(sun);
		const auto light = registry.pinComponent<const LightSourceComponent>(sun);
		ASSERT_TRUE(transform && light);
		EXPECT_EQ(transform->getRotate().x, rotation.x) << "not turned to the scene's sun when told of";
		EXPECT_EQ(transform->getRotate().y, rotation.y) << "not turned to the scene's sun when told of";
		EXPECT_EQ(transform->getRotate().z, rotation.z) << "not turned to the scene's sun when told of";
		EXPECT_EQ(light->getLightColor().x, colour.x) << "not coloured by the scene's air when told of";
		EXPECT_EQ(light->getLightColor().y, colour.y) << "not coloured by the scene's air when told of";
		EXPECT_EQ(light->getLightColor().z, colour.z) << "not coloured by the scene's air when told of";
	}

	// A scene loaded once the renderer is made is lit by its own sky from its first frame: the sky light baked when it is
	// told of, as the lighting pass bakes the sky of a scene there when it is made, and not a bake behind it -- the maps
	// the lighting binds are still the ones it had. The frames do bake a sky that changes, into the other maps
	// (LightingPass::updateSkyLight), so that check is one that can fail.
	TEST(SceneEvents, ASceneToldLaterIsLitByItsSkyAtOnce) {
		auto& registry = ECSHandler::registry();
		auto* render = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(render, nullptr);
		// Whatever ran before settled: no bake under way, the maps baked of the world as it is.
		step(kSkySettles);
		ASSERT_FALSE(Render::Atmosphere::sceneSky(false)) << "a sky in the world before the test's";
		const auto irradiance = [render] { return render->getRenderData().frame.texture(Render::FrameId::IblIrradiance).id; };
		const auto before = irradiance();
		ASSERT_NE(before, Render::TextureHandle{}.id) << "no sky light";

		const auto root = loadSky(21.5f, 133.f);
		const Finally takeOut([root] { takeOutScene(root, kSkySettles); });
		ASSERT_TRUE(registry.hasComponent<ComponentsModule::SkyComponent>(root)) << "the scene did not load";
		step(3);
		EXPECT_EQ(irradiance(), before) << "the scene's sky baked a frame behind, not when told of";

		if (auto sky = registry.pinComponent<ComponentsModule::SkyComponent>(root)) {
			sky->sunElevation += 20.f;
		}
		step(3);
		EXPECT_NE(irradiance(), before) << "a sky that changed not baked again";
	}

	// The scene told of is the scene as its file has it: every entity in the order the file lists them, depth first
	// (fillTree's order), and every transform where the file puts it under its parents -- the render copy and the
	// component's own matrix both, bit for bit what the file's values give -- even where a node's transform was worked out
	// before fillTree hung it under its parent and the renderer's copy written from that (kWorkedOutAlone).
	TEST(SceneEvents, ASceneIsToldAsItsFileHasIt) {
		addWorkedOutAlone();
		int owner = 0;
		int told = 0;
		std::vector<std::string> order;
		// By id: its own matrix and its render copy, as they were when the scene was told of.
		std::unordered_map<std::string, std::pair<Math::Mat4, Math::Mat4>> stood;
		PropertiesModule::onSceneLoaded(&owner, [&](const SceneLoaded& scene) {
			++told;
			auto& registry = ECSHandler::registry();
			for (const auto id : scene.entities) {
				std::string name = "?";
				if (const auto debug = registry.pinComponent<const DebugDataComponent>(id)) {
					name = debug->stringId;
				}
				order.push_back(name);
				Math::Mat4 own{ 0.f };
				Math::Mat4 copy{ 0.f };
				if (const auto transform = registry.pinComponent<const TransformComponent>(id)) {
					own = transform->copyTransform();
				}
				if (const auto matrix = registry.pinComponent<const ComponentsModule::TransformMatComp>(id)) {
					copy = matrix->mTransform;
				}
				stood[name] = { own, copy };
			}
		});
		const auto root = loadWritten(nestedNode(0), "sfe_scene_events_nested_test.json");
		PropertiesModule::forgetSceneListeners(&owner);
		const Finally takeOut([root] { takeOutScene(root, 1); });
		ASSERT_NE(root, ecss::INVALID_ID) << "the scene did not load";

		// Where the file puts each node: its own values as PropertiesSystem works them out, under its parent's.
		std::array<Math::Mat4, kNestedNodes> expected{};
		for (size_t i = 0; i < kNested.size(); ++i) {
			const auto& node = kNested[i];
			const auto local = Math::translate(Math::Mat4{ 1.f }, node.pos)
				* Math::Quaternion<float>::fromEulerDegrees(node.rotate).toMat4()
				* Math::scale(Math::Mat4{ 1.f }, node.scale);
			expected[i] = node.parent < 0 ? local : expected[static_cast<size_t>(node.parent)] * local;
		}

		ASSERT_EQ(told, 1) << "not told once";
		EXPECT_EQ(order, (std::vector<std::string>{ "scene", "A", "A1", "A2", "B" })) << "not told in the order the file lists them";
		for (size_t i = 0; i < kNested.size(); ++i) {
			const auto found = stood.find(kNested[i].id);
			ASSERT_TRUE(found != stood.end()) << kNested[i].id << " not told of";
			EXPECT_TRUE(found->second.first == expected[i]) << kNested[i].id << ": its own matrix not where the file puts it";
			EXPECT_TRUE(found->second.second == expected[i]) << kNested[i].id << ": its render copy not where the file puts it";
		}
	}
}
