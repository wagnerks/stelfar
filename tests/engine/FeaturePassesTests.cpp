#include <array>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include <json/value.h>

#include "EngineTestFixture.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/SkyComponent.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/WaterComponent.h"
#include "componentsModule/Hierarchy.h"
#include "propertiesModule/PropertiesSystem.h"
#include "renderModule/Particles.h"
#include "rhi/NullRHI.h"
#include "systemsModule/TasksManager.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"

// The renderer's features (PassEntry::on): a pass exists while its setting is on or the scene has what it draws, and
// going it lets go of everything it made. Counted on the None backend by kind (NullRHI::live). The engine is one for
// every test and the particles let go are the process's, so what is asserted holds in any order the tests run in.
namespace SFE::EngineTests {
	using Render::NullRHI;

	namespace {
		using Counts = std::array<size_t, static_cast<size_t>(NullRHI::Kind::Count)>;

		// What the RHI holds now, by kind.
		Counts live() {
			Counts counts{};
			auto* rhi = dynamic_cast<NullRHI*>(Engine::instance()->rhi());
			EXPECT_NE(rhi, nullptr) << "the tests' engine is not on the None backend";
			for (size_t kind = 0; rhi && kind < counts.size(); ++kind) {
				counts[kind] = rhi->live(static_cast<NullRHI::Kind>(kind));
			}
			return counts;
		}

		size_t total(const Counts& counts) {
			return std::accumulate(counts.begin(), counts.end(), size_t{ 0 });
		}

		std::string said(const Counts& counts) {
			std::string text;
			for (const auto count : counts) {
				text += (text.empty() ? "" : " ") + std::to_string(count);
			}
			return text;
		}

		SystemsModule::RenderSystem& renderer() {
			return *ECSHandler::getSystem<SystemsModule::RenderSystem>();
		}

		// What `cycle` leaves once what stays the engine's (a texture loaded through TextureHandler, the sky's lookup
		// ring of three) has been made: the cycle run until it leaves what it left the time before, at most five times.
		Counts settled(const std::function<Counts()>& cycle) {
			auto after = cycle();
			for (int i = 0; i < 5; ++i) {
				const auto again = cycle();
				if (again == after) {
					break;
				}
				after = again;
			}
			return after;
		}

		// An entity of the world with a water and a sky, as a scene would give them.
		ecss::EntityId lakeUnderSky() {
			auto& registry = ECSHandler::registry();
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			registry.addComponent<ComponentsModule::WaterComponent>(id);
			registry.addComponent<ComponentsModule::SkyComponent>(id);
			return id;
		}

		// An entity of the world with a sky alone.
		ecss::EntityId sky() {
			auto& registry = ECSHandler::registry();
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			registry.addComponent<ComponentsModule::SkyComponent>(id);
			return id;
		}

		// A scene file of a root with a sky and a lake under it, as a game's scene has them, loaded: its root.
		ecss::EntityId loadLake() {
			Json::Value file;
			file["id"] = "lake scene";
			file["Properties"]["SkyComponent"]["sun_elevation"] = 40.f;
			Json::Value lake;
			lake["id"] = "lake";
			lake["Properties"]["TransformComponent"]["Pos"] = Json::arrayValue;
			for (int i = 0; i < 3; ++i) {
				lake["Properties"]["TransformComponent"]["Pos"].append(0.f);
			}
			lake["Properties"]["WaterComponent"]["size"] = 40.f;
			file["Children"].append(lake);
			const auto path = std::filesystem::temp_directory_path() / "sfe_feature_passes_lake.json";
			{
				std::ofstream out(path);
				out << file.toStyledString();
			}
			const auto root = PropertiesModule::PropertiesSystem::loadScene(path.string());
			std::filesystem::remove(path);
			return root;
		}

		void takeOutScene(const ecss::EntityId root) {
			if (root == ecss::INVALID_ID) {
				return;
			}
			auto made = Hierarchy::descendants(root);
			made.insert(made.begin(), root);
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(made);
			}
			ECSHandler::destroyEntities(made);
		}

		const std::vector<std::string> kSky = { "Water", "Underwater", "Sky", "Mist" };
	}

	// With nothing in the world to draw and the settings as they are, no pass of the scene's features exists, nor
	// ContactGI and DoF, which are off; the particles' passes exist as far as anything was let go; the core always. The
	// list is all there, and the frames go on.
	TEST(FeaturePasses, NoDataNoPass) {
		auto& renderSystem = renderer();
		const int frames = renderSystem.renderedFrames();
		step(3);
		EXPECT_EQ(renderSystem.renderedFrames(), frames + 3);
		for (const char* name : { "Water", "Underwater", "Glass", "Sky", "Mist", "ParticleSprites", "ParticleClearLiquid" }) {
			EXPECT_FALSE(renderSystem.passOn(name)) << name << " exists with nothing to draw";
		}
		const auto& graphics = renderSystem.getRenderData().graphics;
		const std::pair<const char*, bool> settings[] = { { "SSAO", graphics.ssao }, { "SSS", graphics.sss },
			{ "ContactGI", graphics.contactGi }, { "SSR", graphics.ssr }, { "Volumetric", graphics.volumetric },
			{ "TAA", graphics.taa }, { "DoF", graphics.dof } };
		for (const auto& [name, on] : settings) {
			EXPECT_EQ(renderSystem.passOn(name), on) << name;
		}
		EXPECT_FALSE(graphics.contactGi);
		EXPECT_FALSE(graphics.dof);
		EXPECT_EQ(renderSystem.passOn("Particles"), Render::Particles::emitted() > 0);
		EXPECT_EQ(renderSystem.passOn("ParticleLiquid"), Render::Particles::emitted(Render::Particles::Look::Drop) > 0);
		for (const char* name : { "AnimationCompute", "CascadedShadow", "PointLight", "Geometry", "SceneDepth", "SunShadow",
			"Lighting", "Final", "GUI", "Debug", "ImGui" }) {
			EXPECT_TRUE(renderSystem.passOn(name)) << name << " does not exist";
		}
		EXPECT_EQ(renderSystem.renderPassNames(), Render::defaultPasses().names());
	}

	// A water under a sky makes the water's passes and the sky's on the next frame, in their places; gone, they go,
	// everything they made with them: a second time round leaves the RHI as the first left it.
	TEST(FeaturePasses, AWaterUnderASkyMakesThemAndTakesThemAway) {
		auto& renderSystem = renderer();
		ecss::EntityId lake = ecss::INVALID_ID;
		const Finally takeOut([&] {
			if (lake != ecss::INVALID_ID) {
				ECSHandler::destroyEntity(lake);
			}
			step(3);
		});
		std::vector<bool> madeOn;
		const auto cycle = [&] {
			lake = lakeUnderSky();
			step(1);
			for (const auto& name : kSky) {
				madeOn.push_back(renderSystem.passOn(name));
			}
			ECSHandler::destroyEntity(lake);
			lake = ecss::INVALID_ID;
			step(3);
			for (const auto& name : kSky) {
				EXPECT_FALSE(renderSystem.passOn(name)) << name << " outlived the water and the sky";
			}
			return live();
		};
		const auto before = settled(cycle);
		madeOn.clear();
		const auto after = cycle();
		EXPECT_EQ(after, before) << "a water and a sky made and taken away left behind (by kind): " << said(after) << " against "
			<< said(before);
		ASSERT_EQ(madeOn.size(), kSky.size());
		for (size_t i = 0; i < kSky.size(); ++i) {
			EXPECT_TRUE(madeOn[i]) << kSky[i] << " was not made by a water under a sky";
		}
		const auto names = renderSystem.renderPassNames();
		EXPECT_EQ(names, Render::defaultPasses().names()) << "a pass made later is not in its place";
	}

	// The same through a scene loaded, and its entities destroyed.
	TEST(FeaturePasses, ALoadedLakeMakesThemAndTakesThemAway) {
		auto& renderSystem = renderer();
		ecss::EntityId root = ecss::INVALID_ID;
		const Finally takeOut([&] {
			takeOutScene(root);
			step(3);
		});
		root = loadLake();
		ASSERT_NE(root, ecss::INVALID_ID) << "the scene did not load";
		step(1);
		for (const auto& name : kSky) {
			EXPECT_TRUE(renderSystem.passOn(name)) << name << " was not made by the scene loaded";
		}
		takeOutScene(root);
		root = ecss::INVALID_ID;
		step(3);
		for (const auto& name : kSky) {
			EXPECT_FALSE(renderSystem.passOn(name)) << name << " outlived the scene";
		}
	}

	// Glass in the scene makes the glass pass on the next frame, its pipelines as it draws; the last glass gone, the
	// pass goes, and all it made with it.
	TEST(FeaturePasses, GlassGoesWithTheLastOfIt) {
		auto& renderSystem = renderer();
		const auto& scene = renderSystem.scene();
		ecss::EntityId glass = ecss::INVALID_ID;
		const Finally takeOut([&] {
			if (glass != ecss::INVALID_ID) {
				ECSHandler::destroyEntity(glass);
			}
			step(2);
		});
		constexpr uint32_t kMesh = (1u << 21) + 41;
		const auto spawnGlass = [&] {
			auto& registry = ECSHandler::registry();
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			registry.addComponent<MeshComponent>(id)->meshGraph.root().value = { kMesh, 0, 0, 3, 3 };
			ComponentsModule::MaterialComponent material;
			material.transparent = true;
			*registry.addComponent<ComponentsModule::MaterialComponent>(id) = material;
			SystemsModule::TasksManager::instance()->notify({ id, SystemsModule::MESH_UPDATED });
			return id;
		};
		bool drawn = true;
		const auto cycle = [&] {
			glass = spawnGlass();
			for (int frame = 0; frame < 300 && scene.instances(Render::GpuScene::List::Glass) == 0; ++frame) {
				step();
			}
			step(1);
			drawn = drawn && scene.instances(Render::GpuScene::List::Glass) > 0 && renderSystem.passOn("Glass");
			ECSHandler::destroyEntity(glass);
			glass = ecss::INVALID_ID;
			step(2);
			EXPECT_EQ(scene.instances(Render::GpuScene::List::Glass), 0u);
			EXPECT_FALSE(renderSystem.passOn("Glass")) << "the glass pass outlived the last glass";
			return live();
		};
		const auto before = settled(cycle);
		const auto after = cycle();
		EXPECT_TRUE(drawn) << "glass in the scene did not make the glass pass";
		EXPECT_EQ(after, before) << "glass made and taken away left behind (by kind): " << said(after) << " against " << said(before);
	}

	// Every feature of the engine's list lets go of all it made: a copy of each, switched on and off by the test, holds
	// more while it exists, and its second time round leaves the RHI by kind as its first did. A raw handle left in any
	// of them shows here.
	TEST(FeaturePasses, EveryFeatureLetsGoOfAllItMade) {
		auto& renderSystem = renderer();
		std::vector<std::string> added;
		const Finally remove([&] {
			for (const auto& name : added) {
				renderSystem.removeRenderPass(name);
			}
			step(1);
		});
		size_t features = 0;
		for (const auto& entry : Render::defaultPasses().entries()) {
			if (!entry.on) {
				continue;
			}
			++features;
			bool on = false;
			Render::PassEntry copy{ "Test" + entry.name, entry.make };
			copy.on = [&on](const SystemsModule::RenderData&) { return on; };
			ASSERT_TRUE(renderSystem.addRenderPass(std::move(copy), entry.name)) << entry.name;
			added.push_back("Test" + entry.name);
			EXPECT_FALSE(renderSystem.passOn(added.back()));
			Counts whileOn{};
			const auto cycle = [&] {
				on = true;
				step(2);
				whileOn = live();
				EXPECT_TRUE(renderSystem.passOn(added.back())) << entry.name << " was not made";
				on = false;
				step(2);
				EXPECT_FALSE(renderSystem.passOn(added.back())) << entry.name << " was not let go of";
				return live();
			};
			const auto before = settled(cycle);
			const auto after = cycle();
			EXPECT_GT(total(whileOn), total(before)) << entry.name << " held nothing while it existed";
			EXPECT_EQ(after, before) << entry.name << " left behind (by kind): " << said(after) << " against " << said(before);
			renderSystem.removeRenderPass(added.back());
			added.pop_back();
		}
		EXPECT_EQ(features, 16u) << "the engine's features are not the sixteen of the list";
	}

	// Particles let go make the particles' pass on the frame that takes them, and the motes' with a sky; a drop makes the
	// liquid's, and the clear liquid's with a sky. Neither goes when what was let go has gone: what comes and goes with
	// play does not make and destroy a pass with every burst.
	TEST(FeaturePasses, ParticlesMakeTheirPassesForGood) {
		auto& renderSystem = renderer();
		ecss::EntityId heaven = ecss::INVALID_ID;
		const Finally takeOut([&] {
			if (heaven != ecss::INVALID_ID) {
				ECSHandler::destroyEntity(heaven);
			}
			step(2);
		});
		using Render::Particles::Burst;
		using Render::Particles::Look;
		Render::Particles::emit(Burst{ .look = Look::Chip, .count = 8, .life = 0.2f });
		step(1);
		EXPECT_TRUE(renderSystem.passOn("Particles")) << "a chip let go made no particles' pass";
		EXPECT_FALSE(renderSystem.passOn("ParticleSprites")) << "the motes' pass was made with no sky";
		heaven = sky();
		step(1);
		EXPECT_TRUE(renderSystem.passOn("ParticleSprites")) << "the motes' pass was not made with a sky";

		Render::Particles::emit(Burst{ .look = Look::Drop, .count = 8, .life = 0.2f });
		step(1);
		EXPECT_TRUE(renderSystem.passOn("ParticleLiquid")) << "a drop let go made no liquid's pass";
		EXPECT_TRUE(renderSystem.passOn("ParticleClearLiquid")) << "the clear liquid's pass was not made with a sky";

		step(90);
		EXPECT_FALSE(Render::Particles::alive(Engine::instance()->animationClock())) << "what was let go lives on";
		for (const char* name : { "Particles", "ParticleSprites", "ParticleLiquid", "ParticleClearLiquid" }) {
			EXPECT_TRUE(renderSystem.passOn(name)) << name << " went when what it draws had gone";
		}
		ECSHandler::destroyEntity(heaven);
		heaven = ecss::INVALID_ID;
		step(1);
		EXPECT_FALSE(renderSystem.passOn("ParticleSprites"));
		EXPECT_FALSE(renderSystem.passOn("ParticleClearLiquid"));
		EXPECT_TRUE(renderSystem.passOn("Particles"));
		EXPECT_TRUE(renderSystem.passOn("ParticleLiquid"));
	}
}
