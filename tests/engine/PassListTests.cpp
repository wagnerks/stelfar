#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "EngineTestFixture.h"
#include "renderModule/PassList.h"
#include "renderModule/renderPasses/ParticlePass.h"
#include "runtimeBridge/RuntimeBridge.h"
#include "systemsModule/systems/RenderSystem.h"

// The renderer's list of passes (Render::PassList): built by name before the renderer is made, every operation that
// cannot be done refused whole and said, and the engine's own list in today's order with the settings that run it.
namespace SFE::EngineTests {
	using Render::PassEntry;
	using Render::PassList;
	using Names = std::vector<std::string>;

	namespace {
		// Passes that draw nothing, two kinds to tell apart.
		struct NothingPass : Render::RenderPass {
			void render(SystemsModule::RenderData&, Render::CommandListHandle) override {}
		};
		struct OtherPass : Render::RenderPass {
			void render(SystemsModule::RenderData&, Render::CommandListHandle) override {}
		};

		PassEntry entry(std::string name) {
			return PassEntry::of<NothingPass>(std::move(name));
		}

		std::vector<PassEntry> run(std::initializer_list<const char*> names) {
			std::vector<PassEntry> entries;
			for (const char* name : names) {
				entries.push_back(entry(name));
			}
			return entries;
		}

		// The engine's passes in draw order, as the renderer drew them before the list was its own (the old table).
		const Names kTodays = { "AnimationCompute", "CascadedShadow", "PointLight", "Geometry", "Particles",
			"ParticleLiquid", "SceneDepth", "SSAO", "SunShadow", "Lighting", "Sky", "SSS", "ContactGI", "SSR", "Water", "Mist",
			"Glass", "Volumetric", "Underwater", "TAA", "ParticleClearLiquid", "ParticleSprites", "DoF", "Final", "GUI", "Debug",
			"ImGui" };
	}

	// An entry goes right after or right before its anchor, a run in its order (its first right after, its last right
	// before), and append puts it last.
	TEST(PassList, InsertsWhereAsked) {
		PassList list;
		ASSERT_TRUE(list.append(entry("A")));
		ASSERT_TRUE(list.append(entry("D")));
		EXPECT_TRUE(list.insertAfter("A", entry("B")));
		EXPECT_TRUE(list.insertBefore("D", entry("C")));
		EXPECT_EQ(list.names(), (Names{ "A", "B", "C", "D" }));
		EXPECT_TRUE(list.insertAfter("A", run({ "A1", "A2" })));
		EXPECT_TRUE(list.insertBefore("D", run({ "D1", "D2" })));
		EXPECT_TRUE(list.append(entry("E")));
		EXPECT_EQ(list.names(), (Names{ "A", "A1", "A2", "B", "C", "D1", "D2", "D", "E" }));
		EXPECT_TRUE(list.insertAfter("E", std::vector<PassEntry>{})) << "an empty run was refused";
		EXPECT_EQ(list.entries().size(), 9u);
		EXPECT_TRUE(list.errors().empty());
	}

	// An anchor or a name no entry has changes nothing: false, the list as it was, and one error each naming it.
	TEST(PassList, AMissingAnchorChangesNothing) {
		PassList list;
		ASSERT_TRUE(list.append(entry("A")));
		ASSERT_TRUE(list.append(entry("B")));
		EXPECT_FALSE(list.insertAfter("Nope", entry("X")));
		EXPECT_FALSE(list.insertBefore("Nope", entry("X")));
		EXPECT_FALSE(list.insertAfter("Nope", run({ "X", "Y" })));
		EXPECT_FALSE(list.insertBefore("Nope", run({ "X", "Y" })));
		EXPECT_FALSE(list.replace("Nope", entry("X")));
		EXPECT_FALSE(list.disable("Nope"));
		EXPECT_FALSE(list.enable("Nope"));
		EXPECT_FALSE(list.remove("Nope"));
		EXPECT_EQ(list.names(), (Names{ "A", "B" }));
		EXPECT_EQ(list.entries().size(), 2u);
		EXPECT_EQ(list.find("Nope"), nullptr);
		ASSERT_EQ(list.errors().size(), 8u);
		EXPECT_EQ(list.errors().front(), "insertAfter \"Nope\": no pass of that name");
		for (const auto& error : list.errors()) {
			EXPECT_NE(error.find("\"Nope\": no pass of that name"), std::string::npos) << error;
		}
	}

	// A name is one entry: one already in the list, one twice in a run, an empty name or no way to make the pass is
	// refused, and a run with any of them goes in not at all.
	TEST(PassList, ANameIsOneEntry) {
		PassList list;
		ASSERT_TRUE(list.append(entry("A")));
		EXPECT_FALSE(list.append(entry("A")));
		EXPECT_EQ(list.errors().back(), "append \"A\": already in the list");
		EXPECT_FALSE(list.insertAfter("A", entry("A")));
		EXPECT_FALSE(list.insertAfter("A", run({ "B", "C", "B" })));
		EXPECT_EQ(list.errors().back(), "insertAfter \"B\": twice in the run");
		EXPECT_FALSE(list.insertBefore("A", run({ "B", "A" })));
		EXPECT_FALSE(list.append(entry("")));
		EXPECT_EQ(list.errors().back(), "append \"\": an entry with no name");
		PassEntry unmade;
		unmade.name = "U";
		EXPECT_FALSE(list.append(unmade));
		EXPECT_EQ(list.errors().back(), "append \"U\": no way to make it");
		EXPECT_EQ(list.names(), (Names{ "A" })) << "a refused run went in in part";
		EXPECT_EQ(list.errors().size(), 6u);
	}

	// A replacement stands in the old entry's place, under its own name, made and run as it is; one named as another
	// entry is refused.
	TEST(PassList, ReplaceKeepsThePlace) {
		PassList list;
		ASSERT_TRUE(list.append(entry("A")));
		ASSERT_TRUE(list.append(entry("B")));
		ASSERT_TRUE(list.append(entry("C")));
		EXPECT_TRUE(list.replace("B", PassEntry::of<OtherPass>("B2", [](const SystemsModule::RenderData&) { return false; })));
		EXPECT_EQ(list.names(), (Names{ "A", "B2", "C" }));
		EXPECT_EQ(list.find("B"), nullptr);
		const auto* replaced = list.find("B2");
		ASSERT_NE(replaced, nullptr);
		ASSERT_TRUE(replaced->runs);
		const auto made = replaced->make();
		EXPECT_NE(dynamic_cast<OtherPass*>(made.get()), nullptr) << "the replacement is not made as it was given";
		EXPECT_TRUE(list.replace("B2", entry("B2"))) << "a replacement under the name it stands in for was refused";
		EXPECT_FALSE(list.replace("B2", entry("C")));
		EXPECT_EQ(list.errors().back(), "replace \"B2\": \"C\" is another pass's name");
		EXPECT_EQ(list.names(), (Names{ "A", "B2", "C" }));
	}

	// A disabled entry is not made (not in names()), but is still found and still an anchor; enabled, it is back.
	// Removed, it is gone and anchors nothing.
	TEST(PassList, DisabledStaysAnAnchor) {
		PassList list;
		ASSERT_TRUE(list.append(entry("A")));
		ASSERT_TRUE(list.append(entry("B")));
		EXPECT_TRUE(list.disable("B"));
		EXPECT_EQ(list.names(), (Names{ "A" }));
		ASSERT_NE(list.find("B"), nullptr);
		EXPECT_FALSE(list.find("B")->enabled);
		EXPECT_TRUE(list.insertAfter("B", entry("C")));
		EXPECT_TRUE(list.insertBefore("B", entry("A2")));
		EXPECT_EQ(list.names(), (Names{ "A", "A2", "C" }));
		EXPECT_TRUE(list.enable("B"));
		EXPECT_EQ(list.names(), (Names{ "A", "A2", "B", "C" }));
		EXPECT_TRUE(list.remove("B"));
		EXPECT_EQ(list.find("B"), nullptr);
		EXPECT_FALSE(list.insertAfter("B", entry("D"))) << "a removed entry is still an anchor";
		EXPECT_EQ(list.names(), (Names{ "A", "A2", "C" }));
	}

	// PassEntry::of<T> makes a new T each time it is asked, on in the list, run as given (none: every frame).
	TEST(PassList, OfMakesItsType) {
		const auto other = PassEntry::of<OtherPass>("O", [](const SystemsModule::RenderData& data) { return data.graphics.dof; });
		EXPECT_EQ(other.name, "O");
		EXPECT_TRUE(other.enabled);
		ASSERT_TRUE(other.make);
		const auto first = other.make();
		const auto second = other.make();
		EXPECT_NE(dynamic_cast<OtherPass*>(first.get()), nullptr);
		EXPECT_NE(first.get(), second.get()) << "a factory gave the same pass twice";
		SystemsModule::RenderData data;
		ASSERT_TRUE(other.runs);
		data.graphics.dof = true;
		EXPECT_TRUE(other.runs(data));
		data.graphics.dof = false;
		EXPECT_FALSE(other.runs(data));
		EXPECT_FALSE(PassEntry::of<OtherPass>("P").runs) << "an entry given no predicate has one";
	}

	// The engine's list is today's: its passes in the old table's order, TextOverlay in it disabled between Debug and
	// ImGui, nothing refused building it.
	TEST(DefaultPasses, AreTodaysOrder) {
		const auto passes = Render::defaultPasses();
		EXPECT_TRUE(passes.errors().empty());
		EXPECT_EQ(passes.names(), kTodays);
		const auto& entries = passes.entries();
		ASSERT_EQ(entries.size(), kTodays.size() + 1);
		const auto overlay = std::find_if(entries.begin(), entries.end(), [](const PassEntry& e) { return e.name == "TextOverlay"; });
		ASSERT_NE(overlay, entries.end());
		EXPECT_FALSE(overlay->enabled);
		ASSERT_NE(overlay, entries.begin());
		ASSERT_NE(overlay + 1, entries.end());
		EXPECT_EQ((overlay - 1)->name, "Debug");
		EXPECT_EQ((overlay + 1)->name, "ImGui");
		for (const auto& entry : entries) {
			EXPECT_TRUE(entry.make) << entry.name << " has no way to be made";
		}
	}

	// Each setting makes its own pass and no other (PassEntry::on); ImGui runs on a frame shown on the screen, Debug on
	// all but a frame that goes to an editor alone, both always made; the core always exists, the scene's features by
	// what it holds, and nothing but ImGui and Debug is run by a predicate of the frame.
	TEST(DefaultPasses, MadeByTheirSettings) {
		using SystemsModule::GraphicsSettings;
		const auto passes = Render::defaultPasses();
		const auto onOf = [&](const char* name) {
			const auto* found = passes.find(name);
			return found ? found->on : PassEntry::On{};
		};
		const auto runsOf = [&](const char* name) {
			const auto* found = passes.find(name);
			return found ? found->runs : PassEntry::Runs{};
		};
		const std::pair<const char*, bool GraphicsSettings::*> settings[] = {
			{ "SSAO", &GraphicsSettings::ssao }, { "SSR", &GraphicsSettings::ssr }, { "Volumetric", &GraphicsSettings::volumetric },
			{ "TAA", &GraphicsSettings::taa }, { "DoF", &GraphicsSettings::dof }, { "SSS", &GraphicsSettings::sss },
			{ "ContactGI", &GraphicsSettings::contactGi },
		};
		for (const auto& [pass, setting] : settings) {
			for (const bool on : { false, true }) {
				SystemsModule::RenderData data;
				for (const auto& other : settings) {
					data.graphics.*other.second = !on;
				}
				data.graphics.*setting = on;
				for (const auto& other : settings) {
					const auto made = onOf(other.first);
					ASSERT_TRUE(made) << other.first << " is made by no setting";
					EXPECT_EQ(made(data), other.first == std::string(pass) ? on : !on)
						<< other.first << " with " << pass << (on ? " on" : " off");
				}
			}
		}

		auto* engine = Engine::instance();
		auto* bridge = RuntimeBridge::RuntimeBridge::instance();
		const bool wasPresent = engine->presentToScreen();
		const bool wasPublish = bridge->publishEditorDisplay();
		const Finally restore([&] {
			engine->setPresentToScreen(wasPresent);
			bridge->setPublishEditorDisplay(wasPublish);
		});
		const auto imgui = runsOf("ImGui");
		const auto debug = runsOf("Debug");
		ASSERT_TRUE(imgui);
		ASSERT_TRUE(debug);
		SystemsModule::RenderData data;
		for (const bool present : { false, true }) {
			for (const bool publish : { false, true }) {
				engine->setPresentToScreen(present);
				bridge->setPublishEditorDisplay(publish);
				EXPECT_EQ(imgui(data), present) << "present " << present << ", publish " << publish;
				EXPECT_EQ(debug(data), !(publish && !present)) << "present " << present << ", publish " << publish;
			}
		}

		const Names core = { "AnimationCompute", "CascadedShadow", "PointLight", "Geometry", "SceneDepth", "SunShadow",
			"Lighting", "Final", "GUI", "Debug", "TextOverlay", "ImGui" };
		for (const auto& entry : passes.entries()) {
			if (entry.name != "ImGui" && entry.name != "Debug") {
				EXPECT_FALSE(entry.runs) << entry.name << " is run by a predicate of the frame";
			}
			const bool isCore = std::find(core.begin(), core.end(), entry.name) != core.end();
			EXPECT_EQ(static_cast<bool>(entry.on), !isCore) << entry.name << (isCore ? " is a feature" : " is core");
		}
		// What the particle passes give one another, each reader after its giver.
		const auto* sprites = passes.find("ParticleSprites");
		const auto* clear = passes.find("ParticleClearLiquid");
		ASSERT_TRUE(sprites && clear);
		EXPECT_EQ(sprites->needs, (Names{ Render::FrameId::ParticleBuffer }));
		EXPECT_EQ(clear->needs, (Names{ Render::FrameId::LiquidHow, Render::FrameId::LiquidWhere, Render::FrameId::LiquidFacing }));
		EXPECT_EQ(passes.find("ParticleLiquid")->gives, clear->needs);
	}

	// A key a pass needs is given by an enabled pass before it, and by one alone: a reader before its writer, one whose
	// writer is disabled and a key two give are each said in errors(), after what was refused; nothing is refused for it.
	TEST(PassList, AKeyNeededHasOneGiverBeforeIt) {
		PassList list;
		ASSERT_TRUE(list.append(PassEntry::feature<NothingPass>("Reader", {}, {}, { "test.key" })));
		ASSERT_TRUE(list.append(PassEntry::feature<NothingPass>("Writer", {}, { "test.key" })));
		ASSERT_EQ(list.errors().size(), 1u);
		EXPECT_EQ(list.errors().front(), "needs \"Reader\": \"test.key\" is given by no pass before it");

		ASSERT_TRUE(list.append(PassEntry::feature<NothingPass>("Second", {}, {}, { "test.key" })));
		EXPECT_EQ(list.errors().size(), 1u) << "a reader after its writer was refused";
		ASSERT_TRUE(list.append(PassEntry::feature<NothingPass>("Again", {}, { "test.key" })));
		ASSERT_EQ(list.errors().size(), 2u);
		EXPECT_EQ(list.errors().back(), "gives \"Again\": \"test.key\" is given by \"Writer\" too");

		EXPECT_TRUE(list.remove("Reader"));
		EXPECT_TRUE(list.remove("Again"));
		EXPECT_TRUE(list.errors().empty());
		EXPECT_TRUE(list.disable("Writer"));
		ASSERT_EQ(list.errors().size(), 1u) << "a disabled writer gave its key";
		EXPECT_EQ(list.errors().front(), "needs \"Second\": \"test.key\" is given by no pass before it");
		EXPECT_FALSE(list.append(entry("Writer")));
		ASSERT_EQ(list.errors().size(), 2u);
		EXPECT_EQ(list.errors().front(), "append \"Writer\": already in the list") << "what was refused is said first";
	}

	// The engine the tests run is drawn by the engine's own list, as it was built.
	TEST(EngineTest, TheRendererDrawsTheListItWasGiven) {
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		EXPECT_EQ(renderSystem->renderPassNames(), Render::defaultPasses().names());
	}

	// A pass from outside the engine goes where it is asked to, right after its anchor, runs every frame from then on and
	// can be removed; one under a name taken, after no pass, disabled, or whose predicate says no is not drawn.
	TEST(EngineTest, CustomRenderPassRunsWhereItWasInserted) {
		struct CountingPass : Render::RenderPass {
			int* inits = nullptr;
			int* renders = nullptr;
			void init(SystemsModule::RenderData&) override { ++*inits; }
			void render(SystemsModule::RenderData&, Render::CommandListHandle) override { ++*renders; }
		};
		const auto counting = [](std::string name, int* inits, int* renders, PassEntry::Runs runs = {}) {
			return PassEntry{ std::move(name), [inits, renders] {
				auto pass = std::make_unique<CountingPass>();
				pass->inits = inits;
				pass->renders = renders;
				return std::unique_ptr<Render::RenderPass>(std::move(pass));
			}, std::move(runs) };
		};
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		const Finally remove([&] {
			renderSystem->removeRenderPass("TestCounting");
			renderSystem->removeRenderPass("TestIdle");
		});

		int inits = 0;
		int renders = 0;
		ASSERT_TRUE(renderSystem->addRenderPass(counting("TestCounting", &inits, &renders), "Geometry"));
		EXPECT_EQ(inits, 1);
		EXPECT_FALSE(renderSystem->addRenderPass(counting("TestCounting", &inits, &renders), "Geometry"))
			<< "a second pass took a name already in use";
		EXPECT_FALSE(renderSystem->addRenderPass(counting("TestElsewhere", &inits, &renders), "NoSuchPass"))
			<< "a pass went in after a pass there is not";
		EXPECT_FALSE(renderSystem->addRenderPass(counting("TestElsewhere", &inits, &renders), "")) << "a pass went in after none";
		auto disabled = counting("TestDisabled", &inits, &renders);
		disabled.enabled = false;
		EXPECT_FALSE(renderSystem->addRenderPass(std::move(disabled), "Geometry")) << "a disabled pass was made";
		EXPECT_EQ(inits, 1) << "a refused pass was made";

		int idleInits = 0;
		int idleRenders = 0;
		ASSERT_TRUE(renderSystem->addRenderPass(counting("TestIdle", &idleInits, &idleRenders,
			[](const SystemsModule::RenderData&) { return false; }), "TestCounting"));
		EXPECT_EQ(idleInits, 1) << "a pass that never runs was not made and initialised";

		const auto names = renderSystem->renderPassNames();
		const auto geometry = std::find(names.begin(), names.end(), "Geometry");
		ASSERT_NE(geometry, names.end());
		ASSERT_GE(names.end() - geometry, 3);
		EXPECT_EQ(*(geometry + 1), "TestCounting");
		EXPECT_EQ(*(geometry + 2), "TestIdle");

		step(3);
		EXPECT_EQ(renders, 3);
		EXPECT_EQ(idleRenders, 0) << "a pass whose predicate says no was drawn";

		EXPECT_TRUE(renderSystem->removeRenderPass("TestCounting"));
		EXPECT_FALSE(renderSystem->removeRenderPass("TestCounting"));
		EXPECT_TRUE(renderSystem->removeRenderPass("TestIdle"));
		step(2);
		EXPECT_EQ(renders, 3) << "a removed pass kept running";
		EXPECT_EQ(renderSystem->renderPassNames(), Render::defaultPasses().names()) << "a pass was left behind";
	}

	// A pass exists while its `on` holds: not made while it does not, made, initialised and drawn the first frame it does,
	// destroyed the frame it no longer does -- the key it gave emptied, and a pass that needs that key gone in the same
	// frame -- and made afresh when it holds again. An entry not made is an anchor all the same.
	TEST(EngineTest, APassExistsWhileItIsOn) {
		struct Counts {
			int inits = 0;
			int renders = 0;
			int deaths = 0;
		};
		// Its deaths counted once it was initialised: addRenderPass makes one at once only to know it can be.
		struct CountingPass : Render::RenderPass {
			Counts* counts = nullptr;
			const char* gives = nullptr;
			bool inited = false;
			~CountingPass() override {
				if (inited) {
					++counts->deaths;
				}
			}
			void init(SystemsModule::RenderData&) override {
				inited = true;
				++counts->inits;
			}
			void render(SystemsModule::RenderData& data, Render::CommandListHandle) override {
				++counts->renders;
				if (gives) {
					data.frame.set(gives, { .texture = { 7 } });
				}
			}
		};
		const auto counting = [](std::string name, Counts* counts, const char* gives = nullptr) {
			return PassEntry{ std::move(name), [counts, gives] {
				auto pass = std::make_unique<CountingPass>();
				pass->counts = counts;
				pass->gives = gives;
				return std::unique_ptr<Render::RenderPass>(std::move(pass));
			} };
		};
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		auto& frame = renderSystem->getRenderData().frame;
		const auto& version = renderSystem->getRenderData().targetsVersion;
		// What the passes write through, before what takes them out: they outlive the test's last line.
		bool on = false;
		Counts switched;
		Counts reader;
		Counts afterOff;
		const Finally remove([&] {
			renderSystem->removeRenderPass("TestReader");
			renderSystem->removeRenderPass("TestSwitched");
			renderSystem->removeRenderPass("TestAfterOff");
		});

		auto entry = counting("TestSwitched", &switched, "test.switched");
		entry.on = [&on](const SystemsModule::RenderData&) { return on; };
		entry.gives = { "test.switched" };
		ASSERT_TRUE(renderSystem->addRenderPass(std::move(entry), "Geometry"));
		auto readerEntry = counting("TestReader", &reader);
		readerEntry.needs = { "test.switched" };
		ASSERT_TRUE(renderSystem->addRenderPass(std::move(readerEntry), "TestSwitched"));
		EXPECT_TRUE(renderSystem->addRenderPass(counting("TestAfterOff", &afterOff), "TestSwitched"))
			<< "an entry that is off is no anchor";
		EXPECT_EQ(afterOff.inits, 1) << "a pass with no `on` was not made at once";
		EXPECT_EQ(switched.inits, 0) << "a pass was made while its `on` did not hold";
		EXPECT_EQ(reader.inits, 0) << "a pass was made with no giver of the key it needs";
		EXPECT_FALSE(renderSystem->passOn("TestSwitched"));
		const auto names = renderSystem->renderPassNames();
		EXPECT_NE(std::find(names.begin(), names.end(), "TestSwitched"), names.end()) << "an entry not made is not in the list";

		step(2);
		EXPECT_EQ(switched.inits, 0);
		EXPECT_EQ(switched.renders, 0);

		// The frame's targets change with every pass made or let go (RenderData::targetsVersion), and only then.
		const auto before = version;
		step(1);
		EXPECT_EQ(version, before) << "the targets were said to change with no pass made or let go";
		on = true;
		step(1);
		EXPECT_GT(version, before) << "a pass was made with the targets said to be as they were";
		const auto made = version;
		EXPECT_EQ(switched.inits, 1);
		EXPECT_EQ(switched.renders, 1) << "a pass made was not drawn the frame it was made";
		EXPECT_EQ(reader.inits, 1) << "a reader was not made with its giver";
		EXPECT_EQ(reader.renders, 1);
		EXPECT_TRUE(renderSystem->passOn("TestSwitched"));
		EXPECT_TRUE(frame.texture("test.switched"));

		on = false;
		step(1);
		EXPECT_GT(version, made) << "a pass was let go of with the targets said to be as they were";
		EXPECT_EQ(switched.deaths, 1) << "a pass whose `on` no longer holds was not destroyed";
		EXPECT_EQ(reader.deaths, 1) << "a reader outlived the giver of the key it needs";
		EXPECT_EQ(switched.renders, 1);
		EXPECT_EQ(reader.renders, 1);
		EXPECT_FALSE(renderSystem->passOn("TestSwitched"));
		EXPECT_FALSE(renderSystem->passOn("TestReader"));
		EXPECT_FALSE(frame.texture("test.switched")) << "the key of a pass let go of was left as it gave it";

		on = true;
		step(1);
		EXPECT_EQ(switched.inits, 2) << "a pass on again was not made afresh";
		EXPECT_EQ(switched.renders, 2);
		EXPECT_EQ(reader.inits, 2);
		EXPECT_EQ(afterOff.deaths, 0) << "a pass with no `on` was let go of";
	}
}
