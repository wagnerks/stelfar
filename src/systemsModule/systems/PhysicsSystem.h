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
#include "systemsModule/PhysicsLayers.h"
#include "systemsModule/SystemBase.h"

namespace SFE::SystemsModule {
	// The Jolt world. Its static colliders come from the scenes, each by replaceStatic: what a scene loaded before the
	// world is made holds is taken when it is made, a scene loaded after when it is told of (SceneLoaded) -- every entity
	// with a HeightfieldComponent as a field of its heights (TerrainCollider), every model with a collider as its finest
	// level's triangles -- and an entity's are built again when the scene is read again (PropertiesSystem::reloadScene,
	// replaceStatic); each world its own. An application's upright capsules (addStaticCapsules) go by the same rule. The
	// heightfield is made finer where an application asks (setFineGround). Moving things step at a fixed rate from
	// update(), which runs on the physics tick thread (or stepped from the frame, for a fixed-step run or a capture). The
	// ECS is read on the main thread only: when the world is made, when a scene is told of, and by staticShapes on its
	// caller's thread.
	class PhysicsSystem : public ecss::System {
	public:
		// The layers of PhysicsLayers.h, by the names the code calls them.
		static constexpr JPH::ObjectLayer kStatic = PhysicsLayers::kStatic;
		static constexpr JPH::ObjectLayer kMoving = PhysicsLayers::kMoving;
		static constexpr JPH::ObjectLayer kHeld = PhysicsLayers::kHeld;
		static constexpr JPH::ObjectLayer kCharacter = PhysicsLayers::kCharacter;

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
		// Upright capsules an application brings -- the trunks of its trees, its boulders -- batched by where they stand
		// into static compounds. By the rule of the scenes' colliders: what `source` reports is taken when it is given, and
		// every source given is asked again when a scene is loaded after (SceneLoaded), its compounds standing in for the
		// ones before. Main thread; a world that has not stepped yet takes them at once, one that has between two steps.
		void addStaticCapsules(std::function<void(std::vector<StaticCapsule>&)> source);

		// An entity's static collider as shapes, where `world` puts it: its heightfield's (TerrainCollider), else its
		// model's finest level of detail when that has a collider, else none (an entity gone, too). Built with these, for
		// the scene read again (PropertiesSystem::reloadScene), a set for each world: the heightfield's field is cut into
		// by that world's finer ground.
		static std::vector<JPH::ShapeRefC> staticShapes(ecss::EntityId entity, const Math::Mat4& world);
		// The static bodies made from an entity replaced with bodies of `shapes` -- none: just taken out -- and what
		// lay on either woken. Main thread; they change over at the start of the next update, between steps.
		void replaceStatic(ecss::EntityId entity, std::vector<JPH::ShapeRefC> shapes);
		// The heightfield's collider made finer over a square of its cells, `cells` across from cell (x, z) (as its
		// heights count their samples, x along x and z along z): the square cut out of it for good and
		// `field` (TerrainCollider::fine over it) laid there instead. Given again at the same place, it takes the place
		// of the one before; setFineHeights gives part of it anew. Any thread; they change over at the start of the next
		// update, between steps, in the order given, and what lies where the ground moved is woken to meet it: within
		// `lower` (where the field is under the heightfield it takes the place of) and where the heights given anew moved.
		// Gone if the heightfield's collider is built anew.
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
		// What replaceStatic was given, and the capsules asked for anew, in place of what stood there. Physics thread,
		// between steps.
		void swapStatic();
		// What setFineGround was given, laid in; and whether `shapes`, just made `made` for `entity`, are the
		// heightfield's (its field then the one cut into). Physics thread.
		void swapFine();
		void noteTerrain(ecss::EntityId entity, const std::vector<JPH::ShapeRefC>& shapes, const std::vector<JPH::BodyID>& made);
		void dropFine();
		// Every capsule source asked, and what they report batched into static compounds by where it stands. Main thread.
		std::vector<JPH::ShapeRefC> capsuleShapes() const;
		// The capsules' bodies taken out and bodies of `shapes` put in their place. Main thread before the world steps,
		// the physics thread after.
		void placeCapsules(std::vector<JPH::ShapeRefC> shapes);

		// Jolt keeps references to these three: declared before the world, so they outlive it.
		PhysicsLayers::BroadPhase mBroadPhase;
		PhysicsLayers::ObjectVsBroadPhase mObjectVsBroadPhase;
		PhysicsLayers::ObjectPairs mObjectPairs;
		// Whether the world has begun stepping, and the lock its first update and a scene laid in at once take turns with
		// (the scene listener, addStaticCapsules).
		std::mutex mStepping;
		std::atomic<bool> mStepped{ false };
		std::unique_ptr<JPH::TempAllocator> mTempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> mJobs;
		std::unique_ptr<JPH::PhysicsSystem> mWorld;
		std::vector<std::function<void(float)>> mStepCallbacks; // the physics thread's
		// Added since the last update, under the mutex: a system set up while the physics already ticks registers then.
		std::vector<std::function<void(float)>> mNewStepCallbacks;
		std::mutex mStepCallbacksMutex;
		std::vector<std::function<void(std::vector<StaticCapsule>&)>> mCapsuleSources; // the main thread's
		// The static bodies of the heightfields and the props, by the entity each was made from, and the capsules': the
		// physics thread's after construction. What replaceStatic left for it, and the capsules asked for anew, under the
		// mutex.
		std::unordered_map<ecss::EntityId, std::vector<JPH::BodyID>> mStatic;
		std::vector<JPH::BodyID> mCapsuleBodies;
		std::mutex mReplacedMutex;
		std::vector<std::pair<ecss::EntityId, std::vector<JPH::ShapeRefC>>> mReplaced;
		std::optional<std::vector<JPH::ShapeRefC>> mReplacedCapsules;
		// The heightfield's body and its shape (TerrainCollider) -- the last one built -- and the finer ground laid on it
		// by place: the physics thread's. What setFineGround left for it, under the mutex.
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
