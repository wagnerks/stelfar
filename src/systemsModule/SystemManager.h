#pragma once
#include <typeinfo>

#include "core/LoadTrace.h"

#include <initializer_list>
#include <memory>
#include <type_traits>

#include "systemsModule/SystemBase.h"

namespace ecss {
	class SystemManager final {
		SystemManager(const SystemManager&) = delete;
		SystemManager& operator=(SystemManager&) = delete;
	public:
		SystemManager();
		~SystemManager();
		
		void update(float_t dt);
		
		// mSystemsMap is indexed BY type id, never by creation order. SystemTypeCounter hands
		// out ids lazily on first touch, so a getSystem<X>() from inside another system's
		// constructor claims X's id early and the two orders diverge for good -- which used to
		// make createSystem<T>() hand back an unrelated system and skip constructing T.
		template <class T>
		T* getSystem() {
			const size_t index = SystemTypeCounter::type<T>();
			return index < mSystemsMap.size() ? static_cast<T*>(mSystemsMap[index]) : nullptr;
		}

		template <class T, class... ARGS>
		T* createSystem(ARGS&&... systemArgs) {
			const size_t index = SystemTypeCounter::type<T>();
			if (index < mSystemsMap.size() && mSystemsMap[index]) {
				return static_cast<T*>(mSystemsMap[index]);
			}

			// Timed by type name: a start that grew slower then points at the system that made it slow.
			SFE::LoadTrace::Step step(SFE::LoadTrace::plainName(typeid(T).name()));
			auto system = new T(std::forward<ARGS>(systemArgs)...);
			system->bindWorkers();

			// The constructor above may have created further systems, so claim the slot only
			// once it has returned.
			if (mSystemsMap.size() <= index) {
				mSystemsMap.resize(index + 1, nullptr);
			}
			mSystemsMap[index] = system;
			mMade.push_back(system);
			// One with an update of its own is one to schedule; one never scheduled is said at the first update.
			if constexpr (!std::is_same_v<decltype(&T::update), void (System::*)(float)>) {
				mUpdating.push_back(system);
			}

			return system;
		}

		// Each takes a system createSystem made: one never made is an error, said and left out; so is one scheduled
		// twice, or an anchor not on the main thread.
		//
		// On the main thread, after those there already, in this order.
		template <class... ARGS>
		void addRootSystems() {
			addRoots(nullptr, nullptr, { Named{ getSystem<ARGS>(), typeid(ARGS).name() }... });
		}

		// On the main thread, right before `Anchor` (one already there), in this order.
		template <class Anchor, class... ARGS>
		void addRootSystemsBefore() {
			addRoots(getSystem<Anchor>(), typeid(Anchor).name(), { Named{ getSystem<ARGS>(), typeid(ARGS).name() }... });
		}

		// Tick systems run at their own rate: each on a thread of its own, or, when this says so, stepped from
		// update() by the frame time, so a fixed frame step makes them deterministic. Settled at the first update, for
		// every tick system whenever it was added (SystemManager.cpp).
		void setStepTickSystems(bool stepped) { mStepTickSystems = stepped; }

		template <class... ARGS>
		void addTickSystems(float ticks = 32) {
			addTicks({ Named{ getSystem<ARGS>(), typeid(ARGS).name() }... }, ticks);
		}

		// Made, with an update of their own, and neither on the main thread nor ticked: in the order they were made.
		std::vector<System*> unscheduled() const;

	private:
		struct SteppedTickSystem {
			System* system = nullptr;
			float accumulated = 0.f;
		};
		// A system to schedule, and its type's name for what is said of it (null when it was never made).
		struct Named {
			System* system;
			const char* type;
		};

		// `anchorType` null: appended; else each goes right before `anchor`, which must be on the main thread.
		void addRoots(System* anchor, const char* anchorType, std::initializer_list<Named> systems);
		void addTicks(std::initializer_list<Named> systems, float ticks);
		void addTickSystem(System* system, float ticks);

		std::vector<System*> mSystemsMap;
		// In the order they were made, each after the ones its constructor made or asked for: deleted the other way, so
		// a system goes before those it was made on (a character's bodies before the world they are in).
		std::vector<System*> mMade;

		std::vector<System*> mMainThreadSystems;
		// The made ones with an update of their own, and the ticked ones: what unscheduled() tells apart.
		std::vector<System*> mUpdating;
		std::vector<System*> mTicked;

		// Tick systems on their own threads, all woken on time by one scheduler. See SystemManager.cpp.
		class TickScheduler;
		std::unique_ptr<TickScheduler> mTickScheduler;
		std::vector<SteppedTickSystem> mSteppedTickSystems;
		bool mStepTickSystems = false;
	};
}
