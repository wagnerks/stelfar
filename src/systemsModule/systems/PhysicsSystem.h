#pragma once

#include <atomic>

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <optional>

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "mathModule/Forward.h"
#include "systemsModule/SystemBase.h"

namespace SFE::SystemsModule {
	// The Jolt world. Static colliders are built from the scene when the system is created (every entity with a
	// TerrainComponent, from its heights: TerrainCollider), and an entity's built again when the scene is read again
	// (replaceStatic), each world its own; the terrain's made finer where a game asks (setFineGround); moving
	// things step at a fixed rate from update(), which runs on the physics tick thread (or stepped from the frame, for a
	// fixed-step run or a capture). Nothing here touches the ECS after construction but staticShapes, on its caller's
	// thread.
	class PhysicsSystem : public ecss::System {
	public:
		// Object layers: what never moves, and what does (characters, rigid bodies); and what touches nothing while it is
		// moved by hand (a thing carried), which still moves and is still found by what asks for kHeld.
		static constexpr JPH::ObjectLayer kStatic = 0;
		static constexpr JPH::ObjectLayer kMoving = 1;
		static constexpr JPH::ObjectLayer kHeld = 2;
		// A character's own standing capsule, and hair hung off a character: both move like kMoving and meet everything
		// it meets, except each other. Hair lies on a body and on the ground, but a capsule is only the room a
		// character takes up, and hair hanging inside that room would be shoved out of it to hang in the air.
		static constexpr JPH::ObjectLayer kCharacter = 3;
		static constexpr JPH::ObjectLayer kHair = 4;

		// The main world: the characters and what they hold, few things moving at once, stepped by kMainThreads workers
		// besides its own thread.
		PhysicsSystem();
		// A world whose steps `threads` workers help with, besides its own thread.
		explicit PhysicsSystem(int threads);
		~PhysicsSystem() override;
		void update(float dt) override;

		/// @brief An upright capsule standing in the world: where its foot is, how thick and how tall.
		struct StaticCapsule {
			Math::Vec3 base;
			float radius = 0.f;
			float height = 0.f;
		};
		// Static bodies a game brings -- the trunks of its trees, its boulders. Called once, by addStaticCapsuleBodies;
		// fill the list with everything that stands. Register while the game's systems are created.
		void addStaticCapsules(std::function<void(std::vector<StaticCapsule>&)> source) { mCapsuleSources.push_back(std::move(source)); }
		// The registered capsules into the world. After the game's systems exist -- they register them -- and before
		// the world starts stepping; the terrain is in from the start, for whatever those systems stand on it.
		void addStaticCapsuleBodies();
		// The capsules another world was given, for this one too (LoosePhysicsSystem). Before addStaticCapsuleBodies.
		void shareStaticCapsules(const PhysicsSystem& with) { mCapsuleSources = with.mCapsuleSources; }

		// An entity's static collider as shapes, where `world` puts it: the terrain's height field (TerrainCollider),
		// a model's finest level of detail when that has a collider, else none (an entity gone, too). Built with these,
		// for the scene read again (PropertiesSystem::reloadScene), a set for each world: the terrain's field is cut into
		// by that world's finer ground.
		static std::vector<JPH::ShapeRefC> staticShapes(ecss::EntityId entity, const Math::Mat4& world);
		// The static bodies made from an entity replaced with bodies of `shapes` -- none: just taken out -- and what
		// lay on either woken. Main thread; they change over at the start of the next update, between steps.
		void replaceStatic(ecss::EntityId entity, std::vector<JPH::ShapeRefC> shapes);
		// The terrain's collider made finer over a square of its cells, `cells` across from cell (x, z) (as
		// terrainHeights counts its samples, x along x and z along z): the square cut out of the terrain for good and
		// `field` (TerrainCollider::fine over it) laid there instead. Given again at the same place, it takes the place
		// of the one before; setFineHeights gives part of it anew. Any thread; they change over at the start of the next
		// update, between steps, in the order given, and what lies where the ground moved is woken to meet it: within
		// `lower` (where the field is under the terrain it takes the place of) and where the heights given anew moved.
		// Gone if the terrain's collider is built anew.
		void setFineGround(int x, int z, int cells, JPH::ShapeRefC field, const JPH::AABox& lower);
		void setFineHeights(int x, int z, int u, int v, int width, int height, std::vector<float> heights);

		// Called on the physics thread before every fixed step with its length: the place for anything that moves
		// through the world with it, such as a character controller. Any thread: it joins at the start of the next update.
		void addStepCallback(std::function<void(float)> callback);
		// Inside a step callback: the time the step brings the world to, on the simulation's clock (Clocks::simulation;
		// the wall clock while the world runs in real time). Steps that run back to back in one update still get times a
		// step apart, so what follows them can be eased smoothly.
		double stepTime() const { return mStepTime; }
		// What a step costs, eased over the last second or so: milliseconds for the step callbacks and the world's
		// update, and the bodies in the world, awake and all.
		struct Stats {
			float stepMs = 0.f;
			uint32_t bodies = 0;
			uint32_t activeBodies = 0;
		};
		Stats stats() const { return { mStepMs.load(std::memory_order_relaxed), mBodyCount.load(std::memory_order_relaxed), mActiveCount.load(std::memory_order_relaxed) }; }

		JPH::PhysicsSystem& world() { return *mWorld; }
		JPH::TempAllocator& tempAllocator() { return *mTempAllocator; }
		const JPH::ObjectVsBroadPhaseLayerFilter& broadPhaseFilter() const;
		const JPH::ObjectLayerPairFilter& layerPairFilter() const;

		// Where a ray from `from` along `direction` (not normalised: its length is the reach) first hits something,
		// as a fraction of that reach. Physics thread, from a step callback: not while the world is stepping.
		std::optional<float> castRay(const Math::Vec3& from, const Math::Vec3& direction) const;

	private:
		void addStaticColliders();
		// What replaceStatic was given, in place of what stood there. Physics thread, between steps.
		void swapStatic();
		// What setFineGround was given, laid in; and whether `shapes`, just made `made` for `entity`, are the terrain's
		// (its field then the one cut into). Physics thread.
		void swapFine();
		void noteTerrain(ecss::EntityId entity, const std::vector<JPH::ShapeRefC>& shapes, const std::vector<JPH::BodyID>& made);
		void dropFine();

		struct Layers;
		std::unique_ptr<Layers> mLayers;
		std::unique_ptr<JPH::TempAllocator> mTempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> mJobs;
		std::unique_ptr<JPH::PhysicsSystem> mWorld;
		std::vector<std::function<void(float)>> mStepCallbacks; // the physics thread's
		// Added since the last update, under the mutex: a system set up while the physics already ticks registers then.
		std::vector<std::function<void(float)>> mNewStepCallbacks;
		std::mutex mStepCallbacksMutex;
		std::vector<std::function<void(std::vector<StaticCapsule>&)>> mCapsuleSources;
		// The static bodies of the terrain and the props, by the entity each was made from: the physics thread's after
		// construction. What replaceStatic left for it, under the mutex.
		std::unordered_map<ecss::EntityId, std::vector<JPH::BodyID>> mStatic;
		std::mutex mReplacedMutex;
		std::vector<std::pair<ecss::EntityId, std::vector<JPH::ShapeRefC>>> mReplaced;
		// The terrain's body and its shape (TerrainCollider), and the finer ground laid on it by place: the physics
		// thread's. What setFineGround left for it, under the mutex.
		struct FineGround {
			int x = 0;
			int z = 0;
			int cells = 0;
			JPH::ShapeRefC field; // laid; none: part of it given anew
			JPH::AABox lower;
			int u = 0;
			int v = 0;
			int width = 0;
			int height = 0;
			std::vector<float> heights;
		};
		ecss::EntityId mTerrainEntity = ecss::INVALID_ID;
		JPH::BodyID mTerrainBody;
		JPH::ShapeRefC mTerrainShape;
		std::unordered_map<uint64_t, JPH::BodyID> mFine;
		std::mutex mFineMutex;
		std::vector<FineGround> mFineGiven;
		float mAccumulated = 0.f;
		std::atomic<float> mStepMs{ 0.f };
		std::atomic<uint32_t> mBodyCount{ 0 };
		std::atomic<uint32_t> mActiveCount{ 0 };
		double mStepTime = 0.0;
	};

	// A second Jolt world, stepped on a thread of its own at the same rate, standing on the same static colliders: for
	// the many loose bodies a game strews about (litter, debris). However many of them tumble at once and however long
	// its steps take, the main world -- the characters -- steps on undisturbed. The two worlds do not touch: what moves
	// in both (a character) is a body in each, a kinematic one here that pushes the loose bodies it walks into.
	class LoosePhysicsSystem final : public PhysicsSystem {
	public:
		// Stepped by half the machine's threads, less its own.
		LoosePhysicsSystem();
	};
}
