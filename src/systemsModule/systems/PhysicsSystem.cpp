#include "PhysicsSystem.h"

#include <algorithm>
#include <execution>
#include <mutex>
#include <numeric>
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

#include "assetsModule/modelModule/Model.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/TerrainComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "logsModule/logger.h"

namespace SFE::SystemsModule {
	namespace {
		constexpr float kStep = 1.f / 60.f;
		// The main world's workers: a few characters and what they hold move in it, two are plenty. Each world had half
		// the machine's threads, and the main world's sat idle beside the loose one's on the same cores.
		constexpr int kMainThreads = 2;

		namespace BroadPhaseLayers {
			constexpr JPH::BroadPhaseLayer kStatic{ 0 };
			constexpr JPH::BroadPhaseLayer kMoving{ 1 };
			constexpr JPH::uint kCount = 2;
		}

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
	}

	struct PhysicsSystem::Layers {
		struct BroadPhase final : JPH::BroadPhaseLayerInterface {
			JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
				return layer == kStatic ? BroadPhaseLayers::kStatic : BroadPhaseLayers::kMoving;
			}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
				return layer == BroadPhaseLayers::kStatic ? "static" : "moving";
			}
#endif
		} broadPhase;

		struct ObjectVsBroadPhase final : JPH::ObjectVsBroadPhaseLayerFilter {
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override {
				// Static against static is never tested, and what is held against nothing.
				return layer != kHeld && (layer != kStatic || broadPhaseLayer == BroadPhaseLayers::kMoving);
			}
		} objectVsBroadPhase;

		struct ObjectPairs final : JPH::ObjectLayerPairFilter {
			bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
				return a != kHeld && b != kHeld && (a != kStatic || b != kStatic)
					&& !((a == kCharacter && b == kHair) || (a == kHair && b == kCharacter));
			}
		} objectPairs;
	};

	PhysicsSystem::PhysicsSystem() : PhysicsSystem(kMainThreads) {}

	PhysicsSystem::PhysicsSystem(const int threads) {
		initJolt();
		mLayers = std::make_unique<Layers>();
		mTempAllocator = std::make_unique<StackThenHeap>(kTempBytes);
		mJobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
		mWorld = std::make_unique<JPH::PhysicsSystem>();
		mWorld->Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, mLayers->broadPhase, mLayers->objectVsBroadPhase, mLayers->objectPairs);
		mWorld->SetGravity({ 0.f, -9.81f, 0.f });
		addStaticColliders();
		mWorld->OptimizeBroadPhase();
	}

	PhysicsSystem::~PhysicsSystem() = default;

	LoosePhysicsSystem::LoosePhysicsSystem()
		: PhysicsSystem((std::max)(1, static_cast<int>(std::thread::hardware_concurrency()) / 2 - 1)) {}

	const JPH::ObjectVsBroadPhaseLayerFilter& PhysicsSystem::broadPhaseFilter() const {
		return mLayers->objectVsBroadPhase;
	}

	const JPH::ObjectLayerPairFilter& PhysicsSystem::layerPairFilter() const {
		return mLayers->objectPairs;
	}

	void PhysicsSystem::update(float dt) {
		swapStatic();
		// A long frame (a hitch, a breakpoint) is not replayed in full: the world just runs slow for a moment.
		mAccumulated = (std::min)(mAccumulated + dt, kStep * 4.f);
		const double now = Engine::clockSeconds();
		while (mAccumulated >= kStep) {
			// What is left over after this step is time the world has not reached yet.
			mStepTime = now - (mAccumulated - kStep);
			const double started = Engine::clockSeconds();
			for (const auto& callback : mStepCallbacks) {
				callback(kStep);
			}
			mWorld->Update(kStep, 1, mTempAllocator.get(), mJobs.get());
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
		auto& registry = ECSHandler::registry();
		auto& bodies = mWorld->GetBodyInterface();
		// The terrain, and the props set in the scene with a collider.
		for (const auto& [id, model, matrix] : registry.view<const ModelComponent, const ComponentsModule::TransformMatComp>()) {
			if (!model || !matrix || !(model->collider || registry.hasComponent<ComponentsModule::TerrainComponent>(id))) {
				continue;
			}
			for (const auto& shape : meshShapes(*model, matrix->mTransform)) {
				JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
				mStatic[id].push_back(bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate));
			}
		}
	}

	std::vector<JPH::ShapeRefC> PhysicsSystem::staticShapes(ecss::EntityId entity, const Math::Mat4& world) {
		auto& registry = ECSHandler::registry();
		const auto model = registry.pinComponent<const ModelComponent>(entity);
		if (!model || !(model->collider || registry.hasComponent<ComponentsModule::TerrainComponent>(entity))) {
			return {};
		}
		return meshShapes(*model.get(), world);
	}

	void PhysicsSystem::replaceStatic(ecss::EntityId entity, std::vector<JPH::ShapeRefC> shapes) {
		std::lock_guard lock(mReplacedMutex);
		mReplaced.emplace_back(entity, std::move(shapes));
	}

	void PhysicsSystem::swapStatic() {
		std::vector<std::pair<ecss::EntityId, std::vector<JPH::ShapeRefC>>> replaced;
		{
			std::lock_guard lock(mReplacedMutex);
			replaced.swap(mReplaced);
		}
		if (replaced.empty()) {
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
			if (made.empty()) {
				mStatic.erase(entity);
			}
		}
		// What lay on a prop taken away falls, and what a prop is put down on is pushed out of it.
		if (changed.IsValid()) {
			changed.ExpandBy(JPH::Vec3::sReplicate(kWake));
			bodies.ActivateBodiesInAABox(changed, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{});
		}
	}

	void PhysicsSystem::addStaticCapsuleBodies() {
		auto& bodies = mWorld->GetBodyInterface();
		// Whatever else stands in the world: a game reports upright capsules -- tree trunks, boulders -- and they are
		// batched into static compounds here. A body each would be far more than a world takes.
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
			for (const auto& shape : shapes) {
				if (shape) {
					JPH::BodyCreationSettings body(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStatic);
					bodies.CreateAndAddBody(body, JPH::EActivation::DontActivate);
				}
			}
		}
		mWorld->OptimizeBroadPhase();
	}
}
