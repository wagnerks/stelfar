#include <atomic>
#include <cmath>
#include <vector>

#include "EngineTestFixture.h"
#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/Animation.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "componentsModule/Hierarchy.h"
#include "componentsModule/TransformComponent.h"
#include "core/EngineConfig.h"
#include "core/Params.h"
#include "ecss/Registry.h"
#include "propertiesModule/PropertiesSystem.h"
#include "systemsModule/TasksManager.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::EngineTests {
	using ComponentsModule::AnimationComponent;
	using ComponentsModule::TransformComponent;
	using ComponentsModule::TransformMatComp;

	// Position and rotation set on new entities reach the matrix the renderer reads, through the transform events
	// and TransformSystem's worker. Not a reproducer for the markDirty race fixed alongside this suite: that window
	// is too narrow to hit on demand, and the test passes on the old code too.
	TEST(EngineTest, SpawnedTransformsReachTheRenderCopy) {
		auto& registry = ECSHandler::registry();
		std::vector<ecss::EntityId> ids;
		for (int i = 0; i < 256; ++i) {
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			ids.push_back(id);
		}
		for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
			auto transform = registry.pinComponent<TransformComponent>(ids[i]);
			ASSERT_TRUE(transform);
			transform->setPos({ static_cast<float>(i), 2.f, 3.f });
			transform->setRotate({ 0.f, 90.f, 0.f });
		}

		step(3);

		for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
			const auto render = registry.pinComponent<const TransformMatComp>(ids[i]);
			ASSERT_TRUE(render) << "entity " << ids[i] << " never got a render transform";
			const auto& m = render->mTransform;
			EXPECT_FLOAT_EQ(m[3][0], static_cast<float>(i)) << "entity " << ids[i];
			EXPECT_FLOAT_EQ(m[3][1], 2.f) << "entity " << ids[i];
			// A 90 degree yaw leaves nothing of X on X; the unrotated matrix has 1 there.
			EXPECT_NEAR(m[0][0], 0.f, 1e-5f) << "entity " << ids[i] << " kept its unrotated matrix";
		}
	}

	// A transform added and never set again is reloaded and gets its render copy. Its event used to be sent from the
	// constructor, before the registry had stored it, and a worker that took it at once found nothing to reload and
	// dropped it: now and then one entity in thousands was never drawn. The probe asks, of every transform event, whether
	// its transform is stored by then, which no event sent from the constructor was -- so it fails on every add, not
	// now and then.
	TEST(EngineTest, AddedTransformsAreToldOfOnceStored) {
		// Subscribed for the rest of the run, the tasks manager having no unsubscribe, and listening only while armed.
		struct Probe : SystemsModule::TaskWorker {
			Probe() : TaskWorker({ SystemsModule::TRANSFORM_UPDATE }) { bindWorkers(); }
			void notify(SystemsModule::Task task) override {
				if (!armed) {
					return;
				}
				++told;
				if (!ECSHandler::registry().hasComponent<TransformComponent>(task.entity)) {
					++toldEarly;
				}
			}
			std::atomic_bool armed = false;
			std::atomic<size_t> told = 0;
			std::atomic<size_t> toldEarly = 0;
		};
		static Probe probe;
		probe.told = 0;
		probe.toldEarly = 0;
		probe.armed = true;

		auto& registry = ECSHandler::registry();
		std::vector<ecss::EntityId> ids;
		const Finally takeOut([&ids] {
			probe.armed = false;
			ECSHandler::destroyEntities(ids);
			step();
		});
		for (int round = 0; round < 8; ++round) {
			for (int i = 0; i < 1000; ++i) {
				const auto id = registry.takeEntity();
				registry.addComponent<TransformComponent>(id, id);
				ids.push_back(id);
			}
			step(3);
		}

		EXPECT_GE(probe.told.load(), ids.size());
		EXPECT_EQ(probe.toldEarly.load(), 0u) << "of " << probe.told.load() << " transform events, sent before the transform was stored";
		size_t missing = 0;
		for (const auto id : ids) {
			missing += registry.hasComponent<TransformMatComp>(id) ? 0 : 1;
		}
		EXPECT_EQ(missing, 0u) << "of " << ids.size() << " entities with a transform alone, never given a render transform";
	}

	// With a fixed frame step an animation moves by exactly frames * step * ticks per second.
	TEST(EngineTest, AnimationAdvancesByTheFrameStep) {
		auto& registry = ECSHandler::registry();
		// An animated model of the test's own, the world being empty: a cube on one bone, with a clip of a single key and
		// one of 40 frames. The longest clip is played, so the time has somewhere to go.
		constexpr const char* kPath = "models/test_anim.fbx";
		const auto* model = AssetsModule::ModelLoader::instance()->load(kPath);
		ASSERT_NE(model, nullptr) << kPath << " did not load";
		const AssetsModule::Animation* clip = nullptr;
		for (const auto& candidate : model->getAnimations()) {
			if (!clip || candidate.getDuration() > clip->getDuration()) {
				clip = &candidate;
			}
		}
		ASSERT_NE(clip, nullptr) << kPath << " has no clip";
		ASSERT_GT(clip->getDuration(), 0.f) << kPath << " has no clip that moves";

		const auto animated = PropertiesModule::PropertiesSystem::spawnModel(kPath, {}, Math::Vec3{ 1.f }, {});
		const Finally takeOut([animated] {
			ECSHandler::destroyEntity(animated);
			step();
		});
		{
			auto animation = registry.pinComponent<AnimationComponent>(animated);
			ASSERT_TRUE(animation) << kPath << " was spawned without an animation";
			animation->mCurrentAnimation = clip;
			animation->mLoop = true;
			animation->play(0.f);
		}
		SystemsModule::TasksManager::instance()->notify({ animated, SystemsModule::ANIMATION_UPDATED });
		// The clip starts on the first frame that draws it.
		step();
		ASSERT_FALSE(std::isnan(registry.pinComponent<const AnimationComponent>(animated)->mStart)) << "the clip never started";

		auto* engine = Engine::instance();
		const auto clockBefore = engine->animationClock();
		constexpr int kFrames = 30;
		step(kFrames);
		EXPECT_DOUBLE_EQ(engine->animationClock() - clockBefore, kFrames * static_cast<double>(kFrameTime));

		{
			const auto animation = registry.pinComponent<const AnimationComponent>(animated);
			const auto ticksPerSecond = AnimationComponent::ticksPerSecond(*animation->mCurrentAnimation);
			const auto duration = animation->mCurrentAnimation->getDuration();
			float expected = animation->time(clockBefore) + kFrames * kFrameTime * ticksPerSecond;
			if (duration > 0.f) {
				expected = std::fmod(expected, duration);
			}
			EXPECT_NEAR(animation->time(engine->animationClock()), expected, 1e-3f);
		}
	}

	// GpuScene under the load its incremental layout is worst at: thousands of meshes with one instance each, then
	// rounds of mass destroy and respawn. Every reference must still lead to its own instance, and once the churn
	// stops no frame may pack the lists again (the check used to count compact's own spare as holes and repack
	// every frame past about a thousand batches).
	TEST(EngineTest, GpuSceneStaysConsistentUnderChurn) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		auto& scene = renderSystem->scene();
		scene.trackUploads(true);

		// Mesh ids past anything the world loads, so the batches are ours alone. Nothing is drawn on this backend,
		// so a mesh only has to look uploaded.
		constexpr uint32_t kFirstMesh = 1u << 20;
		const size_t materialsBefore = renderSystem->materials().liveEntries();
		const auto spawn = [&](uint32_t mesh) {
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			auto meshComp = registry.addComponent<MeshComponent>(id);
			meshComp->meshGraph.root().value = { kFirstMesh + mesh, 0, 0, 3, 3 };
			// A distinct material per mesh, the way dragging a slider makes a new one every step.
			auto material = registry.addComponent<ComponentsModule::MaterialComponent>(id);
			material->roughness = static_cast<float>(mesh % 4096) / 4096.f;
			SystemsModule::TasksManager::instance()->notify({ id, SystemsModule::MESH_UPDATED });
			return id;
		};

		std::vector<ecss::EntityId> ids;
		for (uint32_t i = 0; i < 3000; ++i) {
			ids.push_back(spawn(i));
		}
		// As many frames as the spawned meshes take to reach the scene -- through the transform, AABB and octree workers
		// first, a frame or more each, more while the pool is busy -- counted alone, in a world that holds nothing else.
		// Then as many more as the lists take to settle once the last is in. A scene that packed its lists again every
		// frame would never settle, so the check below would still catch it.
		size_t batches = 0;
		for (int frame = 0; frame < 300 && batches < 3000u; ++frame) {
			step();
			batches = 0;
			for (const auto& group : scene.groups(Render::GpuScene::List::GBuffer)) {
				batches += group.batchCount;
			}
		}
		ASSERT_TRUE(scene.validate());
		ASSERT_GE(batches, 3000u) << "the spawned meshes never reached the scene, so nothing below is tested";
		auto compactions = scene.compactions();
		for (int frame = 0, quiet = 0; frame < 300 && quiet < 10; ++frame) {
			step();
			quiet = scene.compactions() == compactions ? quiet + 1 : 0;
			compactions = scene.compactions();
		}

		step(30);
		EXPECT_EQ(scene.compactions(), compactions) << "the lists were packed again with nothing changing";

		uint32_t seed = 12345;
		const auto next = [&] {
			seed = seed * 1664525u + 1013904223u;
			return seed >> 8;
		};
		for (int round = 0; round < 20; ++round) {
			for (int i = 0; i < 200 && !ids.empty(); ++i) {
				const size_t victim = next() % ids.size();
				ECSHandler::destroyEntity(ids[victim]);
				ids[victim] = ids.back();
				ids.pop_back();
			}
			for (int i = 0; i < 200; ++i) {
				ids.push_back(spawn(next() % 4000));
			}
			// Outlines come and go with no event of their own.
			for (int i = 0; i < 20; ++i) {
				const auto id = ids[next() % ids.size()];
				if (registry.hasComponent<OutlineComponent>(id)) {
					registry.destroyComponent<OutlineComponent>(id);
				}
				else {
					registry.addComponent<OutlineComponent>(id);
				}
			}
			step();
			ASSERT_TRUE(scene.validate()) << "round " << round;
			size_t outlinedInstances = 0;
			for (const auto& group : scene.groups(Render::GpuScene::List::Outline)) {
				outlinedInstances += group.batchCount;
			}
			size_t outlined = 0;
			for (auto [id, outline] : registry.view<const OutlineComponent>()) {
				++outlined;
			}
			EXPECT_EQ(outlinedInstances > 0, outlined > 0) << "round " << round;
		}

		step(2);
		compactions = scene.compactions();
		step(30);
		EXPECT_EQ(scene.compactions(), compactions) << "the lists kept being packed after the churn stopped";
		EXPECT_TRUE(scene.validate());

		EXPECT_LT(renderSystem->materials().liveEntries(), materialsBefore + 4000) << "materials of destroyed entities were kept";
		for (const auto id : ids) {
			ECSHandler::destroyEntity(id);
		}
		step(2);
		EXPECT_TRUE(scene.validate());
		EXPECT_EQ(renderSystem->materials().liveEntries(), materialsBefore) << "materials outlived every instance using them";
		scene.trackUploads(false);
	}

	// Thousands of copies of one mesh are split into batches of at most kMaxBatchInstances, so the cull, one
	// invocation per batch, stays parallel for foliage-like scenes; removing most of them keeps the layout valid.
	TEST(EngineTest, GpuSceneSplitsLargeBatches) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		auto& scene = renderSystem->scene();
		scene.trackUploads(true);

		constexpr uint32_t kMesh = (1u << 21) + 7;
		std::vector<ecss::EntityId> ids;
		for (int i = 0; i < 5000; ++i) {
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			registry.addComponent<MeshComponent>(id)->meshGraph.root().value = { kMesh, 0, 0, 3, 3 };
			SystemsModule::TasksManager::instance()->notify({ id, SystemsModule::MESH_UPDATED });
			ids.push_back(id);
		}
		step(3);
		ASSERT_TRUE(scene.validate());
		EXPECT_GE(scene.batchesOfMesh(Render::GpuScene::List::GBuffer, kMesh), 5u);
		EXPECT_LE(scene.largestBatch(Render::GpuScene::List::GBuffer), Render::GpuScene::kMaxBatchInstances);

		for (size_t i = 0; i < ids.size(); ++i) {
			if (i % 5 != 0) {
				ECSHandler::destroyEntity(ids[i]);
			}
		}
		step(2);
		EXPECT_TRUE(scene.validate());
		for (size_t i = 0; i < ids.size(); i += 5) {
			ECSHandler::destroyEntity(ids[i]);
		}
		step(2);
		EXPECT_TRUE(scene.validate());
		EXPECT_EQ(scene.batchesOfMesh(Render::GpuScene::List::GBuffer, kMesh), 0u);
		scene.trackUploads(false);
	}

	// An entity destroyed a frame or two after it was spawned, the way spawnModel builds one, leaves no instance
	// behind, whether it is destroyed through ECSHandler or straight on the registry.
	TEST(EngineTest, EntityDestroyedRightAfterSpawnLeavesNoInstance) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		auto& scene = renderSystem->scene();

		constexpr uint32_t kMesh = (1u << 21) + 11;
		const auto spawn = [&] {
			const auto id = registry.takeEntity();
			auto transform = registry.addComponent<TransformComponent>(id, id);
			transform->setPos({ 1.f, 2.f, 3.f });
			registry.addComponent<TransformMatComp>(id, Math::Mat4{ 1.f });
			registry.addComponent<MeshComponent>(id)->meshGraph.root().value = { kMesh, 0, 0, 3, 3 };
			SystemsModule::TasksManager::instance()->notify({ id, SystemsModule::MESH_UPDATED });
			return id;
		};
		for (const bool raw : { false, true }) {
			for (int framesAlive = 0; framesAlive <= 3; ++framesAlive) {
				const auto id = spawn();
				step(framesAlive);
				if (raw) {
					registry.destroyEntity(id);
				}
				else {
					ECSHandler::destroyEntity(id);
				}
				step(2);
				EXPECT_TRUE(scene.validate());
				EXPECT_EQ(scene.batchesOfMesh(Render::GpuScene::List::GBuffer, kMesh), 0u)
					<< (raw ? "registry" : "ECSHandler") << " destroy after " << framesAlive << " frames left a ghost";
			}
		}
	}

	// Bone matrices are pooled by armature: ranges never overlap, a destroyed entity's range is reused instead of
	// the pool growing, an id taken again in the same frame gets a range of its own, and an armature past the old
	// 256-bone limit gets all of its bones.
	TEST(EngineTest, BonePoolHandsOutDisjointRangesAndReusesThem) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);

		const auto spawn = [&](size_t bones) {
			const auto id = registry.takeEntity();
			registry.addComponent<ComponentsModule::ArmatureBonesComponent>(id)->boneMatrices.resize(bones, Math::Mat4{ 1.f });
			return id;
		};
		const auto disjoint = [&](const std::vector<ecss::EntityId>& ids) {
			std::vector<std::pair<uint32_t, uint32_t>> ranges;
			for (const auto id : ids) {
				const auto range = renderSystem->boneRange(id);
				ranges.emplace_back(range.offset - range.bounds, range.offset + range.count);
			}
			std::sort(ranges.begin(), ranges.end());
			for (size_t i = 1; i < ranges.size(); ++i) {
				if (ranges[i].first < ranges[i - 1].second) {
					return false;
				}
			}
			return true;
		};

		std::vector<ecss::EntityId> ids;
		for (int i = 0; i < 200; ++i) {
			ids.push_back(spawn(20 + i % 50));
		}
		ids.push_back(spawn(600));
		for (const auto id : ids) {
			EXPECT_GT(renderSystem->boneRange(id).count, 0u);
		}
		EXPECT_EQ(renderSystem->boneRange(ids.back()).count, 600u);
		EXPECT_TRUE(disjoint(ids));
		const auto highest = [&] {
			uint32_t end = 0;
			for (const auto id : ids) {
				const auto range = renderSystem->boneRange(id);
				end = (std::max)(end, range.offset + range.count);
			}
			return end;
		};
		const auto endBefore = highest();

		// Rounds of destroying half and spawning as many again: freed ranges are reused, so the pool settles instead
		// of growing with every round.
		uint32_t seed = 777;
		for (int round = 0; round < 20; ++round) {
			std::vector<ecss::EntityId> kept;
			for (size_t i = 0; i < ids.size(); ++i) {
				if (i % 2 == static_cast<size_t>(round % 2)) {
					ECSHandler::destroyEntity(ids[i]);
				}
				else {
					kept.push_back(ids[i]);
				}
			}
			step();
			const size_t respawn = ids.size() - kept.size();
			ids = std::move(kept);
			for (size_t i = 0; i < respawn; ++i) {
				seed = seed * 1664525u + 1013904223u;
				ids.push_back(spawn(20 + (seed >> 8) % 50));
			}
			ASSERT_TRUE(disjoint(ids)) << "round " << round;
		}
		EXPECT_LE(highest(), endBefore * 2) << "freed ranges were not reused";

		// Destroyed and taken again with no frame between: the new entity must not keep the old range.
		const auto old = ids.back();
		const auto oldRange = renderSystem->boneRange(old);
		ECSHandler::destroyEntity(old);
		ids.pop_back();
		const auto reused = spawn(oldRange.count + 30);
		EXPECT_EQ(renderSystem->boneRange(reused).count, oldRange.count + 30);
		ids.push_back(reused);
		EXPECT_TRUE(disjoint(ids));
		step();
		EXPECT_TRUE(disjoint(ids)) << "the release of the destroyed entity freed its successor's range";

		for (const auto id : ids) {
			ECSHandler::destroyEntity(id);
		}
		step();
	}

	// Tick systems step from the frame under a fixed step (Engine::start settled it from the config): one ticked once a
	// second moves in whole ticks, two in 125 frames of 1/60 s.
	TEST(EngineTest, ATickSystemStepsFromTheFrameStep) {
		struct Counting : ecss::System {
			void update(float) override { ++ticks; }
			std::atomic<int> ticks{ 0 };
		};
		auto& systems = ECSHandler::systemManager();
		auto* counting = systems.getSystem<Counting>();
		if (!counting) {
			// Made once for the run (a system stays as long as the engine): a repeated run counts on from where it was.
			counting = systems.createSystem<Counting>();
			systems.addTickSystems<Counting>(1);
		}
		const int before = counting->ticks.load();
		step(125);
		const int advanced = counting->ticks.load() - before;
		// 125 frames are 2.08 s; what the accumulator held before adds at most one more tick.
		EXPECT_TRUE(advanced == 2 || advanced == 3) << "advanced " << advanced;
	}

	// What the run is is given once: the fixture's config stands, and a second init changes nothing -- not the step, not
	// the numbers file (none named), not the RHI.
	TEST(EngineTest, InitTakesTheConfigOnce) {
		auto* engine = Engine::instance();
		EXPECT_EQ(engine->config().fixedDeltaTime, kFrameTime);
		EXPECT_FALSE(engine->config().capture.enabled);
		EXPECT_FALSE(engine->capturingFrame());
		EXPECT_TRUE(Params::file().empty()) << "the fixture names no numbers file, yet one is named: " << Params::file();

		auto* rhi = engine->rhi();
		EngineConfig other;
		other.fixedDeltaTime = 0.5f;
		other.maxFps = 30;
		other.capture.enabled = true;
		other.numbersFile = "other.json";
		engine->init(other);
		EXPECT_EQ(engine->config().fixedDeltaTime, kFrameTime) << "a second init took its config";
		EXPECT_EQ(engine->config().maxFps, EngineConfig{}.maxFps);
		EXPECT_FALSE(engine->capturingFrame());
		EXPECT_TRUE(Params::file().empty()) << "a second init named its numbers file";
		EXPECT_EQ(engine->rhi(), rhi) << "a second init made another RHI";
	}

	// The hierarchy is an ecss relation, not a component: a child's transform is its parent's times its own, moving
	// the parent moves the child, and destroying an entity takes its edges with it.
	TEST(EngineTest, ChildTransformFollowsItsParent) {
		auto& registry = ECSHandler::registry();
		const auto parent = registry.takeEntity();
		const auto child = registry.takeEntity();
		const auto grandChild = registry.takeEntity();
		for (const auto id : { parent, child, grandChild }) {
			registry.addComponent<TransformComponent>(id, id);
		}
		Hierarchy::setParent(child, parent);
		Hierarchy::setParent(grandChild, child);

		EXPECT_EQ(Hierarchy::parent(child), parent);
		EXPECT_EQ(Hierarchy::children(parent), std::vector<ecss::EntityId>{ child });
		EXPECT_EQ(Hierarchy::descendants(parent).size(), 2u);

		registry.pinComponent<TransformComponent>(parent)->setPos({ 10.f, 0.f, 0.f });
		registry.pinComponent<TransformComponent>(child)->setPos({ 0.f, 5.f, 0.f });
		registry.pinComponent<TransformComponent>(grandChild)->setPos({ 0.f, 0.f, 2.f });
		step(3);

		const auto world = registry.pinComponent<const TransformComponent>(grandChild)->copyTransform();
		EXPECT_FLOAT_EQ(world[3][0], 10.f);
		EXPECT_FLOAT_EQ(world[3][1], 5.f);
		EXPECT_FLOAT_EQ(world[3][2], 2.f);

		// Moving the parent alone reaches the whole subtree.
		registry.pinComponent<TransformComponent>(parent)->setPos({ 20.f, 0.f, 0.f });
		step(3);
		EXPECT_FLOAT_EQ(registry.pinComponent<const TransformComponent>(grandChild)->copyTransform()[3][0], 20.f);

		// A destroyed entity leaves no edges behind for the id that takes its place.
		ECSHandler::destroyEntity(child);
		EXPECT_EQ(Hierarchy::parent(child), ecss::INVALID_ID);
		EXPECT_TRUE(Hierarchy::children(parent).empty());
		EXPECT_EQ(Hierarchy::parent(grandChild), ecss::INVALID_ID);

		ECSHandler::destroyEntity(parent);
		ECSHandler::destroyEntity(grandChild);
	}

	// Children come back in the order they were attached, so a scene saved and loaded again keeps its order.
	// Reparenting replaces the old parent, and a parent that would close a cycle is refused.
	TEST(EngineTest, HierarchyKeepsOrderAndRefusesCycles) {
		auto& registry = ECSHandler::registry();
		const auto root = registry.takeEntity();
		const auto a = registry.takeEntity();
		const auto b = registry.takeEntity();
		const auto c = registry.takeEntity();

		Hierarchy::setParent(a, root);
		Hierarchy::setParent(b, root);
		Hierarchy::setParent(c, root);
		EXPECT_EQ(Hierarchy::children(root), (std::vector<ecss::EntityId>{ a, b, c }));

		Hierarchy::setParent(b, a);
		EXPECT_EQ(Hierarchy::parent(b), a);
		EXPECT_EQ(Hierarchy::children(root), (std::vector<ecss::EntityId>{ a, c }));

		// root -> a -> b: neither root nor a may become a child of b, nor b of itself.
		Hierarchy::setParent(root, b);
		Hierarchy::setParent(a, b);
		Hierarchy::setParent(b, b);
		EXPECT_EQ(Hierarchy::parent(root), ecss::INVALID_ID);
		EXPECT_EQ(Hierarchy::parent(a), root);
		EXPECT_EQ(Hierarchy::parent(b), a);
		EXPECT_EQ(Hierarchy::descendants(root).size(), 3u);

		for (const auto id : { root, a, b, c }) {
			ECSHandler::destroyEntity(id);
		}
	}
	// The mip chain the texture loader builds. It exists because OpenGL and Vulkan build different chains
	// from the same image, and the difference shows at the horizon, where a pixel on the alpha test
	// threshold flips from one backend to the other. Two properties carry that: the sizes are the usual
	// halving down to 1x1, and a level is the 2x2 average of the one above, so a flat area keeps its exact
	// value all the way down instead of drifting by a rounding step per level.
	TEST(EngineTest, TextureMipChainHalvesAndAveragesTwoByTwo) {
		constexpr int kSize = 4;
		std::vector<unsigned char> image(kSize * kSize * 4);
		for (int y = 0; y < kSize; ++y) {
			for (int x = 0; x < kSize; ++x) {
				unsigned char* px = image.data() + (static_cast<size_t>(y) * kSize + x) * 4;
				px[0] = static_cast<unsigned char>(x * 20);       // varies along x only
				px[1] = static_cast<unsigned char>(y * 20);       // along y only
				px[2] = 77;                                       // flat
				px[3] = 255;
			}
		}

		const auto levels = AssetsModule::TextureHandler::buildMipChain(image.data(), kSize, kSize);
		ASSERT_EQ(levels.size(), 2u) << "4x4 reduces to 2x2 and then 1x1";
		EXPECT_EQ(levels[0].size(), static_cast<size_t>(2 * 2 * 4));
		EXPECT_EQ(levels[1].size(), static_cast<size_t>(4));

		// Level 1 texel (0,0) covers source columns 0,1 and rows 0,1: red averages 0 and 20, green the same.
		EXPECT_EQ(levels[0][0], 10);
		EXPECT_EQ(levels[0][1], 10);
		EXPECT_EQ(levels[0][2], 77) << "a flat channel must not drift";
		// Texel (1,0) covers columns 2,3: red averages 40 and 60.
		EXPECT_EQ(levels[0][4], 50);
		EXPECT_EQ(levels[0][5], 10) << "green does not change along x";

		// The last level is the average of the whole image: red and green both run 0..60 over four texels.
		EXPECT_EQ(levels[1][0], 30);
		EXPECT_EQ(levels[1][1], 30);
		EXPECT_EQ(levels[1][2], 77);
		EXPECT_EQ(levels[1][3], 255) << "alpha is averaged like any other channel";
	}

}
