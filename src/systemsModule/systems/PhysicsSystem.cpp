#include "PhysicsSystem.h"

#include <algorithm>
#include <execution>
#include <mutex>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <thread>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include "debugModule/Profiler.h"
#include "assetsModule/modelModule/Model.h"
#include "componentsModule/HeightfieldComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/Clocks.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "propertiesModule/SceneEvents.h"
#include "systemsModule/TerrainCollider.h"
#include "logsModule/logger.h"

namespace SFE::SystemsModule {
	namespace {
		constexpr float kStep = 1.f / 60.f;
		// The main world's workers: a few characters and what they hold move in it, two are plenty. Each world had half
		// the machine's threads, and the main world's sat idle beside the loose one's on the same cores.
		constexpr int kMainThreads = 2;

		// Jolt's allocator, factory and type registry are process-wide and set up once.
		void initJolt() {
			static std::once_flag once;
			std::call_once(once, [] {
				JPH::RegisterDefaultAllocator();
				JPH::Factory::sInstance = new JPH::Factory();
				JPH::RegisterTypes();
			});
		}

		// Jolt's scratch memory for a step: a stack of kTempBytes, and past that the heap. Jolt's own stack stops the
		// program when a step needs more than it has, which a heap of many bodies in contact can.
		class StackThenHeap final : public JPH::TempAllocator {
		public:
			explicit StackThenHeap(JPH::uint size) : mBase(static_cast<JPH::uint8*>(JPH::AlignedAllocate(size, JPH_RVECTOR_ALIGNMENT))), mSize(size) {}
			~StackThenHeap() override { JPH::AlignedFree(mBase); }

			void* Allocate(JPH::uint size) override {
				if (size == 0) {
					return nullptr;
				}
				const JPH::uint top = mTop + JPH::AlignUp(size, JPH_RVECTOR_ALIGNMENT);
				if (top > mSize) {
					return JPH::AlignedAllocate(size, JPH_RVECTOR_ALIGNMENT);
				}
				void* address = mBase + mTop;
				mTop = top;
				return address;
			}

			void Free(void* address, JPH::uint size) override {
				if (!address) {
					return;
				}
				const auto* at = static_cast<JPH::uint8*>(address);
				if (at < mBase || at >= mBase + mSize) {
					JPH::AlignedFree(address);
					return;
				}
				mTop -= JPH::AlignUp(size, JPH_RVECTOR_ALIGNMENT);
			}

		private:
			JPH::uint8* mBase = nullptr;
			JPH::uint mSize = 0;
			JPH::uint mTop = 0;
		};
		constexpr JPH::uint kTempBytes = 16 * 1024 * 1024;
		// What a world holds at most: bodies, pairs of them the broad phase finds close, and contacts. A heap of
		// litter has many more contacts than bodies, and a contact over the limit is dropped: what it held up falls
		// through.
		constexpr JPH::uint kMaxBodies = 65536;
		constexpr JPH::uint kMaxBodyPairs = 65536;
		constexpr JPH::uint kMaxContacts = 65536;
		// How far round a static collider replaced what moves is woken (metres): what lay on it, or now lies in it.
		constexpr float kWake = 0.5f;

		// A model's finest level of detail where `world` puts it, as static mesh shapes. Cut into a grid of pieces by
		// where each triangle is, each its own shape built on its own thread: one shape over a big terrain takes
		// seconds to build.
		std::vector<JPH::ShapeRefC> meshShapes(const ModelComponent& model, const Math::Mat4& world) {
			JPH::VertexList vertices;
			JPH::IndexedTriangleList triangles;
			for (const auto* meshObject : model.getModel().meshes) {
				if (!meshObject) {
					continue;
				}
				const auto base = static_cast<JPH::uint32>(vertices.size());
				for (const auto& v : meshObject->mesh.vertices) {
					const auto p = world * Math::Vec4{ v.position.x, v.position.y, v.position.z, 1.f };
					vertices.push_back(JPH::Float3(p.x, p.y, p.z));
				}
				const auto& indices = meshObject->mesh.indices;
				for (size_t i = 0; i + 2 < indices.size(); i += 3) {
					triangles.push_back(JPH::IndexedTriangle(base + indices[i], base + indices[i + 1], base + indices[i + 2], 0));
				}
			}
			if (triangles.empty()) {
				return {};
			}

			constexpr int kPieces = 8;
			float minX = vertices[0].x;
			float maxX = vertices[0].x;
			float minZ = vertices[0].z;
			float maxZ = vertices[0].z;
			for (const auto& v : vertices) {
				minX = (std::min)(minX, v.x);
				maxX = (std::max)(maxX, v.x);
				minZ = (std::min)(minZ, v.z);
				maxZ = (std::max)(maxZ, v.z);
			}
			const auto pieceOf = [&](const JPH::IndexedTriangle& triangle) {
				float cx = 0.f;
				float cz = 0.f;
				for (const auto index : triangle.mIdx) {
					cx += vertices[index].x;
					cz += vertices[index].z;
				}
				const int px = std::clamp(static_cast<int>((cx / 3.f - minX) / (maxX - minX + 1e-3f) * kPieces), 0, kPieces - 1);
				const int pz = std::clamp(static_cast<int>((cz / 3.f - minZ) / (maxZ - minZ + 1e-3f) * kPieces), 0, kPieces - 1);
				return pz * kPieces + px;
			};
			std::vector<JPH::IndexedTriangleList> pieces(kPieces * kPieces);
			for (const auto& triangle : triangles) {
				pieces[pieceOf(triangle)].push_back(triangle);
			}
			std::vector<JPH::ShapeRefC> shapes(pieces.size());
			std::vector<size_t> order(pieces.size());
			std::iota(order.begin(), order.end(), size_t{ 0 });
			std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t p) {
				if (pieces[p].empty()) {
					return;
				}
				// Only this piece's vertices, renumbered.
				std::unordered_map<JPH::uint32, JPH::uint32> remap;
				JPH::VertexList pieceVertices;
				for (auto& triangle : pieces[p]) {
					for (auto& index : triangle.mIdx) {
						const auto [it, added] = remap.try_emplace(index, static_cast<JPH::uint32>(pieceVertices.size()));
						if (added) {
							pieceVertices.push_back(vertices[index]);
						}
						index = it->second;
					}
				}
				JPH::MeshShapeSettings shape(std::move(pieceVertices), std::move(pieces[p]));
				const auto result = shape.Create();
				if (result.HasError()) {
					LogsModule::Logger::LOG_ERROR("mesh collider: %s", result.GetError().c_str());
					return;
				}
				shapes[p] = result.Get();
			});
			std::erase_if(shapes, [](const JPH::ShapeRefC& shape) { return !shape; });
			return shapes;
		}

		// An entity's static collider: a heightfield's heights as a field (TerrainCollider; it stands unturned and
		// unscaled), a prop's mesh.
		std::vector<JPH::ShapeRefC> colliderShapes(const ecss::EntityId entity, const ModelComponent* model, const Math::Mat4& world) {
			if (const auto field = ECSHandler::registry().pinComponent<const ComponentsModule::HeightfieldComponent>(entity)) {
				const int n = field->samples;
				if (n >= 2 && field->heights && field->heights->size() >= static_cast<size_t>(n) * static_cast<size_t>(n)) {
					const float half = field->size * 0.5f;
					const Math::Vec3 corner{ world[3][0] - half, world[3][1], world[3][2] - half };
					if (auto shape = TerrainCollider::shape(*field->heights, n, field->size / static_cast<float>(n - 1), corner)) {
						return { shape };
					}
				}
				LogsModule::Logger::LOG_ERROR("terrain collider: no height field, its mesh instead");
			}
			if (!model) {
				return {};
			}
			return meshShapes(*model, world);
		}

		// The static colliders of `entities` given to `physics` to take (replaceStatic), each where its render transform
		// puts it -- seeded for every entity of a scene by the time the scene is told of (PropertiesSystem::loadScene).
		// One with no collider is left out.
		void queueStatic(PhysicsSystem& physics, const std::vector<ecss::EntityId>& entities) {
			auto& registry = ECSHandler::registry();
			for (const auto entity : entities) {
				Math::Mat4 world{ 1.f };
				{
					const auto matrix = registry.pinComponent<const ComponentsModule::TransformMatComp>(entity);
					if (!matrix) {
						continue;
					}
					world = matrix->mTransform;
				}
				if (auto shapes = PhysicsSystem::staticShapes(entity, world); !shapes.empty()) {
					physics.replaceStatic(entity, std::move(shapes));
				}
			}
		}

		// The entities of `candidates` that stand in a world as static colliders, in the one order every way of taking them
		// lays them: what has a model -- with a collider, or drawing a heightfield -- by id, then the heightfields no model
		// draws, by id. A scene told of after a world is made then lays what one there before it would, body for body.
		std::vector<ecss::EntityId> staticOrder(const std::vector<ecss::EntityId>& candidates) {
			auto& registry = ECSHandler::registry();
			std::vector<ecss::EntityId> modelled;
			std::vector<ecss::EntityId> fields;
			for (const auto entity : candidates) {
				if (!registry.hasComponent<ComponentsModule::TransformMatComp>(entity)) {
					continue;
				}
				const bool field = registry.hasComponent<ComponentsModule::HeightfieldComponent>(entity);
				if (const auto model = registry.pinComponent<const ModelComponent>(entity)) {
					if (model->collider || field) {
						modelled.push_back(entity);
					}
				}
				else if (field) {
					fields.push_back(entity);
				}
			}
			std::ranges::sort(modelled);
			std::ranges::sort(fields);
			modelled.insert(modelled.end(), fields.begin(), fields.end());
			return modelled;
		}
	}

	PhysicsSystem::PhysicsSystem() : PhysicsSystem(kMainThreads) {}

	PhysicsSystem::PhysicsSystem(const int threads) {
		initJolt();
		mTempAllocator = std::make_unique<StackThenHeap>(kTempBytes);
		mJobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
		mWorld = std::make_unique<JPH::PhysicsSystem>();
		mWorld->Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, mBroadPhase, mObjectVsBroadPhase, mObjectPairs);
		mWorld->SetGravity({ 0.f, -9.81f, 0.f });
		// Each step listener (a vehicle: its wheels met) a job of its own, so the vehicles go side by side.
		auto settings = mWorld->GetPhysicsSettings();
		settings.mStepListenersBatchSize = 1;
		mWorld->SetPhysicsSettings(settings);
		addStaticColliders();
		mWorld->OptimizeBroadPhase();

		// Take what is there (above), and listen for what is loaded after, the same way: replaceStatic, in the same order
		// (staticOrder). A scene loaded before the world first steps -- a game loading its scene once its systems are made --
		// is laid in at once, on the main thread, for whatever is made on it next; one loaded after goes in between two
		// steps, at the start of the next update. The capsule sources are asked again with it: what stands may have come
		// with the scene.
		PropertiesModule::onSceneLoaded(this, [this](const PropertiesModule::SceneLoaded& scene) {
			std::unique_lock lock(mStepping);
			const bool atOnce = !mStepped.load(std::memory_order_relaxed);
			if (!atOnce) {
				lock.unlock();
			}
			queueStatic(*this, staticOrder(scene.entities));
			std::optional<std::vector<JPH::ShapeRefC>> capsules;
			if (!mCapsuleSources.empty()) {
				capsules = capsuleShapes();
			}
			if (atOnce) {
				swapStatic();
				if (capsules) {
					placeCapsules(std::move(*capsules));
				}
				mWorld->OptimizeBroadPhase();
			}
			else if (capsules) {
				std::lock_guard replaced(mReplacedMutex);
				mReplacedCapsules = std::move(*capsules);
			}
		});
	}

	PhysicsSystem::~PhysicsSystem() {
		// Told of no scene once it is going, made by a SystemManager or not.
		PropertiesModule::forgetSceneListeners(this);
	}

	LoosePhysicsSystem::LoosePhysicsSystem()
		: PhysicsSystem((std::max)(1, static_cast<int>(std::thread::hardware_concurrency()) / 2 - 1)) {}

	const JPH::ObjectVsBroadPhaseLayerFilter& PhysicsSystem::broadPhaseFilter() const {
		return mObjectVsBroadPhase;
	}

	const JPH::ObjectLayerPairFilter& PhysicsSystem::layerPairFilter() const {
		return mObjectPairs;
	}

	void PhysicsSystem::addStepCallback(std::function<void(float)> callback) {
		std::lock_guard lock(mStepCallbacksMutex);
		mNewStepCallbacks.push_back(std::move(callback));
	}

	void PhysicsSystem::update(float dt) {
		// From here on the world steps, and a scene loaded after goes in through replaceStatic; one being laid in at once
		// (the constructor's listener) is finished first.
		if (!mStepped.load(std::memory_order_acquire)) {
			std::lock_guard lock(mStepping);
			mStepped.store(true, std::memory_order_release);
		}
		{
			SFE_PROFILE_ZONE("Physics swap static");
			swapStatic();
		}
		{
			SFE_PROFILE_ZONE("Physics swap fine");
			swapFine();
		}
		{
			std::lock_guard lock(mStepCallbacksMutex);
			for (auto& callback : mNewStepCallbacks) {
				mStepCallbacks.push_back(std::move(callback));
			}
			mNewStepCallbacks.clear();
		}
		// A long frame (a hitch, a breakpoint) is not replayed in full: the world just runs slow for a moment.
		mAccumulated = (std::min)(mAccumulated + dt, kStep * 4.f);
		const double now = Clocks::simulation();
		while (mAccumulated >= kStep) {
			// What is left over after this step is time the world has not reached yet.
			mStepTime = now - (mAccumulated - kStep);
			const double started = Engine::clockSeconds();
			for (const auto& callback : mStepCallbacks) {
				callback(kStep);
			}
			{
				SFE_PROFILE_ZONE("Jolt step");
				mWorld->Update(kStep, 1, mTempAllocator.get(), mJobs.get());
			}
			mAccumulated -= kStep;
			const float ms = static_cast<float>((Engine::clockSeconds() - started) * 1000.0);
			mStepMs.store(mStepMs.load(std::memory_order_relaxed) * 0.95f + ms * 0.05f, std::memory_order_relaxed);
			mBodyCount.store(mWorld->GetNumBodies(), std::memory_order_relaxed);
			mActiveCount.store(mWorld->GetNumActiveBodies(JPH::EBodyType::RigidBody), std::memory_order_relaxed);
		}
	}

	std::optional<float> PhysicsSystem::castRay(const Math::Vec3& from, const Math::Vec3& direction) const {
		JPH::RRayCast ray{ JPH::RVec3(from.x, from.y, from.z), JPH::Vec3(direction.x, direction.y, direction.z) };
		JPH::RayCastResult hit;
		if (mWorld->GetNarrowPhaseQuery().CastRay(ray, hit)) {
			return hit.mFraction;
		}
		return std::nullopt;
	}

	void PhysicsSystem::addStaticColliders() {
		// The heightfields, and the props set in the scene with a collider: taken as a scene loaded later is (the
		// constructor's listener), from every entity with a render transform, in the same order (staticOrder: the models'
		// bodies first, then the heightfields drawn by no model of their own), and laid in at once, as nothing steps a
		// world still being made.
		std::vector<ecss::EntityId> entities;
		for (const auto& [id, matrix] : ECSHandler::registry().view<const ComponentsModule::TransformMatComp>()) {
			if (matrix) {
				entities.push_back(id);
			}
		}
		queueStatic(*this, staticOrder(entities));
		swapStatic();
	}

	std::vector<JPH::ShapeRefC> PhysicsSystem::staticShapes(ecss::EntityId entity, const Math::Mat4& world) {
		auto& registry = ECSHandler::registry();
		const bool field = registry.hasComponent<ComponentsModule::HeightfieldComponent>(entity);
		const auto model = registry.pinComponent<const ModelComponent>(entity);
		if (!field && !(model && model->collider)) {
			return {};
		}
		return colliderShapes(entity, model ? model.get() : nullptr, world);
	}

	void PhysicsSystem::setFineGround(const int x, const int z, const int cells, JPH::ShapeRefC field, const JPH::AABox& lower) {
		std::lock_guard lock(mFineMutex);
		mFineGiven.push_back({ x, z, cells, std::move(field), lower });
	}

	void PhysicsSystem::setFineHeights(const int x, const int z, const int u, const int v, const int width, const int height, std::vector<float> heights) {
		std::lock_guard lock(mFineMutex);
		mFineGiven.push_back({ x, z, 0, nullptr, {}, u, v, width, height, std::move(heights) });
	}

	void PhysicsSystem::noteTerrain(const ecss::EntityId entity, const std::vector<JPH::ShapeRefC>& shapes, const std::vector<JPH::BodyID>& made) {
		if (entity == mTerrainEntity) {
			// Built anew, or gone: its finer ground lay on what stood there before.
			dropFine();
			mTerrainEntity = ecss::INVALID_ID;
			mTerrainBody = {};
			mTerrainShape = nullptr;
		}
		for (size_t k = 0; k < shapes.size() && k < made.size(); ++k) {
			if (TerrainCollider::field(*shapes[k])) {
				mTerrainEntity = entity;
				mTerrainBody = made[k];
				mTerrainShape = shapes[k];
			}
		}
	}

	void PhysicsSystem::dropFine() {
		auto& bodies = mWorld->GetBodyInterface();
		for (const auto& [place, id] : mFine) {
			bodies.RemoveBody(id);
			bodies.DestroyBody(id);
		}
		mFine.clear();
	}

	void PhysicsSystem::swapFine() {
		std::vector<FineGround> given;
		{
			std::lock_guard lock(mFineMutex);
			given.swap(mFineGiven);
		}
		auto* field = mTerrainShape ? TerrainCollider::field(*mTerrainShape) : nullptr;
		if (given.empty() || !field) {
			return;
		}
		const auto grid = TerrainCollider::gridOf(*mTerrainShape);
		auto& bodies = mWorld->GetBodyInterface();
		// What lay on ground that moved is woken, round each place on its own: one box round all would take in all
		// between them.
		const auto wake = [&bodies](JPH::AABox box) {
			box.ExpandBy(JPH::Vec3::sReplicate(kWake));
			bodies.ActivateBodiesInAABox(box, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});
		};
		bool cut = false;
		for (const auto& ground : given) {
			const uint64_t place = static_cast<uint64_t>(static_cast<uint32_t>(ground.x)) << 32 | static_cast<uint32_t>(ground.z);
			JPH::BodyID id;
			if (ground.field) {
				const auto [found, fresh] = mFine.try_emplace(place);
				if (fresh) {
					JPH::BodyCreationSettings body(ground.field, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
					found->second = bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate);
					TerrainCollider::cut(*field, grid, ground.x, ground.z, ground.cells, *mTempAllocator);
					cut = true;
				}
				else {
					bodies.SetShape(found->second, ground.field, false, JPH::EActivation::DontActivate);
				}
				id = found->second;
				if (ground.lower.IsValid()) {
					wake(ground.lower);
				}
			}
			else if (const auto found = mFine.find(place); found != mFine.end()) {
				const auto moved = TerrainCollider::setHeights(*bodies.GetShape(found->second), ground.u, ground.v, ground.width, ground.height,
					ground.heights, *mTempAllocator);
				bodies.NotifyShapeChanged(found->second, JPH::Vec3::sZero(), false, JPH::EActivation::DontActivate);
				if (moved.IsValid()) {
					id = found->second;
					wake(moved);
				}
			}
			// What rests there meets it anew, not as it met what was there before.
			if (!id.IsInvalid()) {
				bodies.InvalidateContactCache(id);
			}
		}
		if (cut) {
			bodies.InvalidateContactCache(mTerrainBody);
		}
	}

	void PhysicsSystem::replaceStatic(ecss::EntityId entity, std::vector<JPH::ShapeRefC> shapes) {
		std::lock_guard lock(mReplacedMutex);
		mReplaced.emplace_back(entity, std::move(shapes));
	}

	void PhysicsSystem::swapStatic() {
		std::vector<std::pair<ecss::EntityId, std::vector<JPH::ShapeRefC>>> replaced;
		std::optional<std::vector<JPH::ShapeRefC>> capsules;
		{
			std::lock_guard lock(mReplacedMutex);
			replaced.swap(mReplaced);
			capsules.swap(mReplacedCapsules);
		}
		if (replaced.empty() && !capsules) {
			return;
		}
		auto& bodies = mWorld->GetBodyInterface();
		// The bodies stand where their shapes are, at the world's origin: their own bounds are where they are.
		JPH::AABox changed;
		for (auto& [entity, shapes] : replaced) {
			auto& made = mStatic[entity];
			for (const auto id : made) {
				changed.Encapsulate(bodies.GetShape(id)->GetLocalBounds());
			}
			if (!made.empty()) {
				bodies.RemoveBodies(made.data(), static_cast<int>(made.size()));
				bodies.DestroyBodies(made.data(), static_cast<int>(made.size()));
				made.clear();
			}
			for (const auto& shape : shapes) {
				changed.Encapsulate(shape->GetLocalBounds());
				JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
				made.push_back(bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate));
			}
			noteTerrain(entity, shapes, made);
			if (made.empty()) {
				mStatic.erase(entity);
			}
		}
		// The capsules asked for anew (a scene loaded after the world stepped) stand in for the ones before.
		if (capsules) {
			for (const auto id : mCapsuleBodies) {
				changed.Encapsulate(bodies.GetShape(id)->GetLocalBounds());
			}
			for (const auto& shape : *capsules) {
				if (shape) {
					changed.Encapsulate(shape->GetLocalBounds());
				}
			}
			placeCapsules(std::move(*capsules));
		}
		// What lay on a prop taken away falls, and what a prop is put down on is pushed out of it.
		if (changed.IsValid()) {
			changed.ExpandBy(JPH::Vec3::sReplicate(kWake));
			bodies.ActivateBodiesInAABox(changed, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});
		}
	}

	void PhysicsSystem::addStaticCapsules(std::function<void(std::vector<StaticCapsule>&)> source) {
		mCapsuleSources.push_back(std::move(source));
		std::unique_lock lock(mStepping);
		if (!mStepped.load(std::memory_order_relaxed)) {
			// Nothing steps the world yet: in at once, for whatever is made on it next.
			placeCapsules(capsuleShapes());
			mWorld->OptimizeBroadPhase();
			return;
		}
		lock.unlock();
		auto shapes = capsuleShapes();
		std::lock_guard replaced(mReplacedMutex);
		mReplacedCapsules = std::move(shapes);
	}

	std::vector<JPH::ShapeRefC> PhysicsSystem::capsuleShapes() const {
		// Whatever else stands in the world: an application reports upright capsules -- tree trunks, boulders -- and they
		// are batched into static compounds here. A body each would be far more than a world takes.
		std::vector<StaticCapsule> capsules;
		for (const auto& source : mCapsuleSources) {
			source(capsules);
		}
		if (!capsules.empty()) {
			// Grouped by where they stand, so one compound holds neighbours and its bounds stay tight.
			constexpr float kGroupMetres = 64.f;
			std::unordered_map<int64_t, std::vector<const StaticCapsule*>> groups;
			for (const auto& capsule : capsules) {
				const int64_t gx = static_cast<int64_t>(std::floor(capsule.base.x / kGroupMetres));
				const int64_t gz = static_cast<int64_t>(std::floor(capsule.base.z / kGroupMetres));
				groups[(gx << 32) ^ (gz & 0xffffffff)].push_back(&capsule);
			}
			std::vector<std::vector<const StaticCapsule*>> cells;
			cells.reserve(groups.size());
			for (auto& [key, group] : groups) {
				cells.push_back(std::move(group));
			}
			std::vector<JPH::ShapeRefC> shapes(cells.size());
			std::vector<size_t> order(cells.size());
			std::iota(order.begin(), order.end(), size_t{ 0 });
			std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t index) {
				JPH::StaticCompoundShapeSettings compound;
				for (const auto* capsule : cells[index]) {
					const float radius = capsule->radius;
					const float halfHeight = (std::max)(capsule->height * 0.5f - radius, 0.01f);
					compound.AddShape(JPH::Vec3(capsule->base.x, capsule->base.y + halfHeight + radius, capsule->base.z), JPH::Quat::sIdentity(),
						new JPH::CapsuleShape(halfHeight, radius));
				}
				if (compound.mSubShapes.empty()) {
					return;
				}
				const auto result = compound.Create();
				if (result.HasError()) {
					LogsModule::Logger::LOG_ERROR("static capsules: %s", result.GetError().c_str());
					return;
				}
				shapes[index] = result.Get();
			});
			return shapes;
		}
		return {};
	}

	void PhysicsSystem::placeCapsules(std::vector<JPH::ShapeRefC> shapes) {
		auto& bodies = mWorld->GetBodyInterface();
		if (!mCapsuleBodies.empty()) {
			bodies.RemoveBodies(mCapsuleBodies.data(), static_cast<int>(mCapsuleBodies.size()));
			bodies.DestroyBodies(mCapsuleBodies.data(), static_cast<int>(mCapsuleBodies.size()));
			mCapsuleBodies.clear();
		}
		for (const auto& shape : shapes) {
			if (shape) {
				JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
				mCapsuleBodies.push_back(bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate));
			}
		}
	}
}
