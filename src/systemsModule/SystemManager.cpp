#include "SystemManager.h"

#include "systemsModule/SystemBase.h"
#include "core/FileSystem.h"
#include "debugModule/Benchmark.h"
#include "debugModule/Profiler.h"
#include "logsModule/logger.h"
#include "propertiesModule/SceneEvents.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ecss {
	namespace {
		using Clock = std::chrono::steady_clock;

		// Waits until a deadline as closely as the platform allows, without asking anything of the platform: it sleeps
		// in short naps while more time is left than a nap has been seen to overrun, and yields for the rest. The
		// overrun is measured as it goes, so where the OS wakes a thread on time almost all of the wait is sleep, and
		// where it wakes it in coarse slices (Windows, ~15.6 ms by default) the last slice is spent yielding.
		class DeadlineWaiter {
		public:
			void waitUntil(Clock::time_point deadline) {
				using Seconds = std::chrono::duration<double>;
				while (true) {
					const auto now = Clock::now();
					if (Seconds(deadline - now).count() <= mNapEstimate) {
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					observe(Seconds(Clock::now() - now).count());
				}
				while (Clock::now() < deadline) {
					std::this_thread::yield();
				}
			}

		private:
			// A running mean and variance of how long a 1 ms nap really takes; the estimate keeps two deviations of
			// margin, so a nap rarely carries the thread past its deadline.
			void observe(double seconds) {
				constexpr double kWeight = 0.05;
				const double delta = seconds - mMean;
				mMean += kWeight * delta;
				mVariance = (1.0 - kWeight) * (mVariance + kWeight * delta * delta);
				mNapEstimate = mMean + 2.0 * std::sqrt(mVariance);
			}

			double mMean = 0.002;
			double mVariance = 0.0;
			double mNapEstimate = 0.002;
		};

		// "A, B, C": the systems by their plain class names.
		std::string namesOf(const std::vector<System*>& systems) {
			std::string names;
			for (const auto* system : systems) {
				names += (names.empty() ? "" : ", ") + SFE::LoadTrace::plainName(typeid(*system).name());
			}
			return names.empty() ? std::string("none") : names;
		}

		// The systems as the first update finds them, for one start to be put beside another's: to the log, and to
		// load_times.log beside the executable, where they follow that start's load (LoadTrace::report).
		void writeOrder(const std::vector<std::string>& lines) {
			std::string text;
			for (const auto& line : lines) {
				SFE::LogsModule::Logger::LOG_INFO("%s", line.c_str());
				text += line + "\n";
			}
			if (FILE* file = std::fopen((SFE::FileSystem::executableDir() / "load_times.log").string().c_str(), "a")) {
				std::fputs(text.c_str(), file);
				std::fputc('\n', file);
				std::fclose(file);
			}
		}
	}

	// One thread keeps the time for every tick system; each system runs on a thread of its own that sleeps on a
	// condition variable until its tick is due. Waiting precisely costs the waiting thread (see DeadlineWaiter), and
	// there is one of those however many systems tick; a woken worker costs nothing until it works.
	//
	// The schedule is absolute: a tick is due one period after the one before, whenever that one ran, so the rate holds
	// on average whatever the waits overrun, and every update gets exactly one period as its step. A system that falls
	// behind runs the ticks it owes back to back, up to a few, and lets the rest go rather than spiral.
	//
	// Made at the first update, which settles for good how the tick systems run: stepped from the frame on the main
	// thread (`stepped`), or each on a thread of its own as above.
	class SystemManager::TickScheduler {
	public:
		explicit TickScheduler(bool stepped) : mStepped(stepped) {}

		~TickScheduler() {
			mAlive.store(false);
			if (mThread.joinable()) {
				mThread.join();
			}
			for (auto& worker : mWorkers) {
				{
					std::lock_guard lock(worker->mutex);
					worker->alive = false;
				}
				worker->wake.notify_one();
				worker->thread.join();
			}
		}

		// Main thread: at the first update, for those queued until then, and for each added after it.
		void add(System* system) {
			auto worker = std::make_unique<Worker>();
			worker->system = system;
			worker->next = Clock::now();
			auto* raw = worker.get();
			size_t number = 0;
			{
				std::lock_guard lock(mWorkersMutex);
				mWorkers.push_back(std::move(worker));
				number = mWorkers.size();
			}
			raw->thread = std::thread([raw, number] {
				SFE::Debug::Profiler::nameThread("Tick", static_cast<int>(number));
				work(*raw);
			});
			if (!mThread.joinable()) {
				mThread = std::thread([this] { schedule(); });
			}
		}

		bool stepped() const { return mStepped; }

	private:
		static constexpr int kMaxCatchUp = 4;

		struct Worker {
			System* system = nullptr;
			Clock::time_point next; // scheduler thread only
			std::mutex mutex;
			std::condition_variable wake;
			int owed = 0;
			bool alive = true;
			std::thread thread;
		};

		static void work(Worker& worker) {
			while (true) {
				int owed = 0;
				{
					std::unique_lock lock(worker.mutex);
					worker.wake.wait(lock, [&] { return worker.owed > 0 || !worker.alive; });
					if (!worker.alive) {
						return;
					}
					owed = std::exchange(worker.owed, 0);
				}
				const float step = 1.f / worker.system->getTicks();
				for (int i = 0; i < owed; ++i) {
					SFE_PROFILE_ZONE(typeid(*worker.system).name());
					worker.system->update(step);
				}
			}
		}

		void schedule() {
			DeadlineWaiter waiter;
			while (mAlive.load()) {
				auto earliest = Clock::time_point::max();
				{
					std::lock_guard lock(mWorkersMutex);
					for (const auto& worker : mWorkers) {
						earliest = (std::min)(earliest, worker->next);
					}
				}
				waiter.waitUntil(earliest);

				const auto now = Clock::now();
				std::lock_guard lock(mWorkersMutex);
				for (const auto& worker : mWorkers) {
					const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / worker->system->getTicks()));
					int due = 0;
					while (now >= worker->next && due < kMaxCatchUp) {
						worker->next += period;
						++due;
					}
					if (now >= worker->next) {
						worker->next = now + period;
					}
					if (due > 0) {
						{
							std::lock_guard workerLock(worker->mutex);
							worker->owed = (std::min)(worker->owed + due, kMaxCatchUp);
						}
						worker->wake.notify_one();
					}
				}
			}
		}

		const bool mStepped;
		std::atomic<bool> mAlive{ true };
		std::mutex mWorkersMutex;
		std::vector<std::unique_ptr<Worker>> mWorkers;
		std::thread mThread;
	};

	SystemManager::SystemManager() = default;

	SystemManager::~SystemManager() {
		mTickScheduler.reset();

		// Every system's async work must be over before the first one is deleted: a pool task still running
		// updateAsync on a deleted system crashed on exit, and a destructor may still notify the other systems. Nor is
		// any told of a scene from here on (SceneEvents.h). In the order they were made, as everything here goes.
		for (const auto system : mMade) {
			system->stopAsync();
			SFE::PropertiesModule::forgetSceneListenersOf(system);
		}

		for (auto system = mMade.rbegin(); system != mMade.rend(); ++system) {
			delete *system;
		}
	}

	void SystemManager::addRoots(System* anchor, const char* anchorType, std::initializer_list<Named> systems) {
		auto at = mMainThreadSystems.end();
		if (anchorType) {
			at = anchor ? std::find(mMainThreadSystems.begin(), mMainThreadSystems.end(), anchor) : mMainThreadSystems.end();
			if (at == mMainThreadSystems.end()) {
				std::string names;
				for (const auto& named : systems) {
					names += (names.empty() ? "" : ", ") + SFE::LoadTrace::plainName(named.type);
				}
				SFE::LogsModule::Logger::LOG_ERROR("%s is not on the main thread: %s not scheduled",
					SFE::LoadTrace::plainName(anchorType).c_str(), names.empty() ? "nothing" : names.c_str());
				return;
			}
		}
		for (const auto& named : systems) {
			if (!named.system) {
				SFE::LogsModule::Logger::LOG_ERROR("%s is scheduled but was never made (createSystem first)",
					SFE::LoadTrace::plainName(named.type).c_str());
				continue;
			}
			if (std::find(mMainThreadSystems.begin(), mMainThreadSystems.end(), named.system) != mMainThreadSystems.end()
				|| std::find(mTicked.begin(), mTicked.end(), named.system) != mTicked.end()) {
				SFE::LogsModule::Logger::LOG_ERROR("%s is scheduled twice", SFE::LoadTrace::plainName(named.type).c_str());
				continue;
			}
			if (anchorType) {
				// Before the anchor, and the next before it again, after this one.
				at = mMainThreadSystems.insert(at, named.system) + 1;
			}
			else {
				mMainThreadSystems.push_back(named.system);
			}
		}
	}

	void SystemManager::addTicks(std::initializer_list<Named> systems, float ticks) {
		for (const auto& named : systems) {
			if (!named.system) {
				SFE::LogsModule::Logger::LOG_ERROR("%s is scheduled but was never made (createSystem first)",
					SFE::LoadTrace::plainName(named.type).c_str());
				continue;
			}
			if (std::find(mMainThreadSystems.begin(), mMainThreadSystems.end(), named.system) != mMainThreadSystems.end()
				|| std::find(mTicked.begin(), mTicked.end(), named.system) != mTicked.end()) {
				SFE::LogsModule::Logger::LOG_ERROR("%s is scheduled twice", SFE::LoadTrace::plainName(named.type).c_str());
				continue;
			}
			addTickSystem(named.system, ticks);
		}
	}

	std::vector<System*> SystemManager::unscheduled() const {
		std::vector<System*> left;
		for (const auto system : mUpdating) {
			if (std::find(mMainThreadSystems.begin(), mMainThreadSystems.end(), system) == mMainThreadSystems.end()
				&& std::find(mTicked.begin(), mTicked.end(), system) == mTicked.end()) {
				left.push_back(system);
			}
		}
		return left;
	}

	void SystemManager::addTickSystem(System* system, float ticks) {
		mTicked.push_back(system);
		system->setTick(ticks);
		// Queued until the first update settles how tick systems run, so one added before setStepTickSystems -- a game's,
		// made before the engine sets the mode -- runs as the engine's do; one added after runs as settled. Until then
		// mSteppedTickSystems is that queue.
		if (mTickScheduler && !mTickScheduler->stepped()) {
			mTickScheduler->add(system);
			return;
		}
		mSteppedTickSystems.push_back({ system });
	}

	void SystemManager::update(float_t dt) {
		FUNCTION_BENCHMARK;

		// The first update settles how the tick systems run, whenever each was added (addTickSystem), and says once what
		// runs and in what order.
		if (!mTickScheduler) {
			mTickScheduler = std::make_unique<TickScheduler>(mStepTickSystems);
			std::string ticking;
			for (const auto& queued : mSteppedTickSystems) {
				char rate[32];
				std::snprintf(rate, sizeof(rate), " %g/s", static_cast<double>(queued.system->getTicks()));
				ticking += (ticking.empty() ? "" : ", ") + SFE::LoadTrace::plainName(typeid(*queued.system).name()) + rate;
			}
			writeOrder({
				"systems made, in order: " + namesOf(mMade),
				"systems on the main thread, in order: " + namesOf(mMainThreadSystems),
				std::string(mTickScheduler->stepped() ? "tick systems, stepped from the frame: " : "tick systems, each on a thread of its own: ")
					+ (ticking.empty() ? std::string("none") : ticking),
				"systems made but not scheduled: " + namesOf(unscheduled()),
			});
			if (!mTickScheduler->stepped()) {
				for (const auto& queued : mSteppedTickSystems) {
					mTickScheduler->add(queued.system);
				}
				mSteppedTickSystems.clear();
			}
		}

		for (auto& ticked : mSteppedTickSystems) {
			const float step = 1.f / ticked.system->getTicks();
			ticked.accumulated += dt;
			while (ticked.accumulated >= step) {
				SFE_PROFILE_ZONE(typeid(*ticked.system).name());
				ticked.system->update(step);
				ticked.accumulated -= step;
			}
		}

		for (const auto system : mMainThreadSystems) {
			SFE_PROFILE_ZONE(typeid(*system).name());
			system->update(dt);
		}

		SFE_PROFILE_ZONE("Debug updates");
		// In the order the systems were made: a slot of mSystemsMap goes by type id, which the first getSystem of a type
		// claims, however long before the system is made.
		for (const auto system : mMade) {
			system->debugUpdate(dt);
		}
	}
}